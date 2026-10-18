#include "tapclient.h"
#include "settings.h"

#include <tlhelp32.h>

namespace TapClient {

static DWORD ExplorerPid() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD pid = 0;
    if (tray) GetWindowThreadProcessId(tray, &pid);
    return pid;
}

static bool IsLoaded(DWORD pid) {
    std::wstring name = TAP_LOADED_PREFIX + std::to_wstring(pid);
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    if (!h) return false;
    CloseHandle(h);
    return true;
}

// The dll is embedded in the exe (resource 2) and written to
// %LOCALAPPDATA%\VolumeOne\VolumeOneTap-<hash>.dll. explorer keeps it loaded
// until it restarts, so every build gets its own file name and an update
// never has to overwrite a locked file.
static std::wstring ExtractDll() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(2), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const BYTE* data = mem ? (const BYTE*)LockResource(mem) : nullptr;
    DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!data || !size) return {};

    UINT32 hash = 2166136261u;  // FNV-1a
    for (DWORD i = 0; i < size; i++) hash = (hash ^ data[i]) * 16777619u;
    wchar_t name[64];
    swprintf_s(name, L"VolumeOneTap-%08x.dll", hash);

    std::wstring dir = AppDataDir();
    std::wstring path = dir + L"\\" + name;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeLow == size) return path;

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

    // Remove dlls of previous builds; the one explorer has loaded stays
    // locked and is cleaned up on a later run.
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\VolumeOneTap-*.dll").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (_wcsicmp(fd.cFileName, name) != 0) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return path;
}

static bool Inject(DWORD pid) {
    std::wstring path = ExtractDll();
    if (path.empty()) return false;

    using InitFn = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!xaml) return false;
    auto init = (InitFn)GetProcAddress(xaml, "InitializeXamlDiagnosticsEx");
    if (!init) return false;

    // The taskbar's XAML may not exist yet right after sign-in; the caller
    // retries until this succeeds.
    HRESULT hr = init(L"VisualDiagConnection1", pid, nullptr, path.c_str(), CLSID_VolumeOneTap, nullptr);
    return SUCCEEDED(hr);
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

static bool SyncProcess(DWORD pid, bool needed) {
    if (!pid) return !needed;
    if (!IsLoaded(pid)) {
        if (!needed) return true;  // nothing to restore
        if (!Inject(pid)) return false;
    }
    std::wstring name = TAP_REFRESH_PREFIX + std::to_wstring(pid);
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str())) {
        SetEvent(ev);
        CloseHandle(ev);
    }
    return true;
}

bool Sync(bool explorer, bool shellHost) {
    bool ok = SyncProcess(ExplorerPid(), explorer);
    ok &= SyncProcess(ShellHostPid(), shellHost);
    return ok;
}

void Notify() { Sync(false, false); }

}  // namespace TapClient
