#include "tapclient.h"
#include "settings.h"
#include "log.h"

#include <restartmanager.h>
#include <tlhelp32.h>

namespace TapClient {

static DWORD ExplorerPid() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD pid = 0;
    if (tray) GetWindowThreadProcessId(tray, &pid);
    return pid;
}

// The Quick Settings panel lives in ShellHost.exe (one per session).
static DWORD ShellHostPid() {
    DWORD session = 0, pid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        DWORD s = 0;
        if (_wcsicmp(pe.szExeFile, L"ShellHost.exe") == 0 && ProcessIdToSessionId(pe.th32ProcessID, &s) && s == session) {
            pid = pe.th32ProcessID;
            break;
        }
    }
    CloseHandle(snap);
    return pid;
}

// The dll is embedded in the exe (resource 2) and written to
// %LOCALAPPDATA%\VolumeOne\VolumeOneTap-<hash>.dll: every build has its own
// name, so we can tell whether a process has this build's dll or an older one,
// and an update never has to overwrite a file that is loaded somewhere.
static bool DllResource(const BYTE** data, DWORD* size) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(2), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    *data = mem ? (const BYTE*)LockResource(mem) : nullptr;
    *size = res ? SizeofResource(nullptr, res) : 0;
    return *data && *size;
}

static std::wstring DllName() {
    static std::wstring name;
    if (name.empty()) {
        const BYTE* data;
        DWORD size;
        if (!DllResource(&data, &size)) return {};
        UINT32 hash = 2166136261u;  // FNV-1a
        for (DWORD i = 0; i < size; i++) hash = (hash ^ data[i]) * 16777619u;
        wchar_t buf[64];
        swprintf_s(buf, L"VolumeOneTap-%08x.dll", hash);
        name = buf;
    }
    return name;
}

static void DeleteOldDlls(const std::wstring& dir, const std::wstring& keep) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\VolumeOneTap-*.dll").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (_wcsicmp(fd.cFileName, keep.c_str()) != 0) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());  // fails if still loaded
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

static std::wstring ExtractDll() {
    const BYTE* data;
    DWORD size;
    if (!DllResource(&data, &size)) return {};
    std::wstring dir = AppDataDir();
    std::wstring path = dir + L"\\" + DllName();
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || fad.nFileSizeLow != size) {
        std::wstring tmp = path + L".tmp";
        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return {};
        DWORD written = 0;
        BOOL ok = WriteFile(f, data, size, &written, nullptr) && written == size;
        CloseHandle(f);
        if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp.c_str());
            return {};
        }
    }
    DeleteOldDlls(dir, DllName());
    return path;
}

// Name of the VolumeOne dll loaded in a process, empty if none.
static std::wstring LoadedDll(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    MODULEENTRY32W me = {sizeof(me)};
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (_wcsnicmp(me.szModule, L"VolumeOneTap", 12) == 0) {
            found = me.szModule;
            break;
        }
    }
    CloseHandle(snap);
    return found;
}

static bool Inject(DWORD pid) {
    std::wstring path = ExtractDll();
    if (path.empty()) {
        LOG(L"tap: couldn't write the dll to %s (%lu)", AppDataDir().c_str(), GetLastError());
        return false;
    }

    using InitFn = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!xaml) return false;
    auto init = (InitFn)GetProcAddress(xaml, "InitializeXamlDiagnosticsEx");
    if (!init) return false;

    // The taskbar's XAML may not exist yet right after sign-in; the caller
    // retries until this succeeds.
    HRESULT hr = init(L"VisualDiagConnection1", pid, nullptr, path.c_str(), CLSID_VolumeOneTap, nullptr);
    LOG(L"tap: inject into pid %lu: %08X", pid, hr);
    return SUCCEEDED(hr);
}

static DWORD WaitForNewPid(DWORD (*find)(), DWORD oldPid) {
    for (int i = 0; i < 100; i++) {
        DWORD pid = find();
        if (pid && pid != oldPid) return pid;
        Sleep(100);
    }
    return 0;
}

// explorer goes through the Restart Manager, the supported way to restart it
// (it also reopens File Explorer windows). ShellHost is restarted by sihost
// as soon as it exits.
static DWORD RestartExplorer(DWORD pid) {
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
    if (RmStartSession(&session, 0, key) == ERROR_SUCCESS) {
        RM_UNIQUE_PROCESS proc = {pid};
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            FILETIME exitTime, kernel, user;
            GetProcessTimes(h, &proc.ProcessStartTime, &exitTime, &kernel, &user);
            CloseHandle(h);
        }
        if (RmRegisterResources(session, 0, nullptr, 1, &proc, 0, nullptr) == ERROR_SUCCESS &&
            RmShutdown(session, RmForceShutdown, nullptr) == ERROR_SUCCESS)
            RmRestart(session, 0, nullptr);
        RmEndSession(session);
    }
    DWORD newPid = WaitForNewPid(ExplorerPid, pid);
    if (!newPid && !ExplorerPid()) {  // the Restart Manager didn't bring it back
        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        wchar_t cmd[] = L"explorer.exe";
        if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        newPid = WaitForNewPid(ExplorerPid, pid);
    }
    return newPid;
}

static DWORD RestartShellHost(DWORD pid) {
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
        TerminateProcess(h, 0);
        WaitForSingleObject(h, 5000);
        CloseHandle(h);
    }
    return WaitForNewPid(ShellHostPid, pid);
}

static void Signal(DWORD pid) {
    std::wstring name = TAP_REFRESH_PREFIX + std::to_wstring(pid);
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str())) {
        SetEvent(ev);
        CloseHandle(ev);
    }
}

static bool SyncProcess(DWORD pid, bool needed, DWORD (*restart)(DWORD)) {
    if (!pid) return !needed;
    std::wstring loaded = LoadedDll(pid);
    LOG(L"tap: pid %lu, needed=%d, loaded dll: %s", pid, needed, loaded.empty() ? L"none" : loaded.c_str());
    // An older build's dll (e.g. after an update) can't be replaced while
    // loaded: restart the process so this build's dll can go in.
    if (needed && !loaded.empty() && _wcsicmp(loaded.c_str(), DllName().c_str()) != 0) {
        pid = restart(pid);
        LOG(L"tap: restarted for the new dll, new pid %lu", pid);
        if (!pid) return false;
        loaded.clear();
    }
    if (loaded.empty()) {
        if (!needed) return true;  // nothing to restore
        if (!Inject(pid)) return false;
    }
    Signal(pid);
    return true;
}

bool Sync(bool explorer, bool shellHost) {
    bool ok = SyncProcess(ExplorerPid(), explorer, RestartExplorer);
    ok &= SyncProcess(ShellHostPid(), shellHost, RestartShellHost);
    return ok;
}

void Notify() {
    if (DWORD pid = ExplorerPid()) Signal(pid);
    if (DWORD pid = ShellHostPid()) Signal(pid);
}

void Unload() {
    DWORD pid = ExplorerPid();
    if (pid && !LoadedDll(pid).empty()) RestartExplorer(pid);
    pid = ShellHostPid();
    if (pid && !LoadedDll(pid).empty()) RestartShellHost(pid);
}

}  // namespace TapClient
