// VolumeOneRemover.exe: removes everything VolumeOne leaves on the PC and puts
// Windows back as it was. It closes VolumeOne, deletes its startup entry, its
// exe(s) and %LOCALAPPDATA%\VolumeOne, and restarts explorer and ShellHost so
// the dll is gone from them and the Windows volume icon and the Quick Settings
// slider come back.

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shlobj.h>
#include <restartmanager.h>
#include <tlhelp32.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../shared.h"
#include "../version.h"

namespace fs = std::filesystem;

#define APP_NAME L"VolumeOne"
static const wchar_t kTitle[] = L"VolumeOne Remover";
static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kLegacyKey[] = L"Software\\VolumeOne";
static const wchar_t kNotifyIconKey[] = L"Control Panel\\NotifyIconSettings";

// --- Strings ----------------------------------------------------------------

struct Strings {
    const wchar_t* confirm;
    const wchar_t* done;
    const wchar_t* leftovers;
};

static const Strings kEn = {
    L"This removes VolumeOne completely:\n\n"
    L"\u2022 Closes VolumeOne and removes it from startup\n"
    L"\u2022 Deletes VolumeOne.exe, its settings, custom icons and logs\n"
    L"\u2022 Restarts Windows Explorer to bring back the Windows volume icon and the Quick Settings volume slider\n\n"
    L"The taskbar will disappear for a few seconds. Continue?",
    L"VolumeOne has been removed and Windows is back as it was.\n\nYou can delete VolumeOneRemover.exe now.",
    L"VolumeOne has been removed, but these files couldn't be deleted (they may be in use). You can delete them by hand:\n\n",
};

static const Strings kEs = {
    L"Esto elimina VolumeOne por completo:\n\n"
    L"\u2022 Cierra VolumeOne y lo quita del inicio de Windows\n"
    L"\u2022 Borra VolumeOne.exe, sus ajustes, iconos personalizados y registros\n"
    L"\u2022 Reinicia el Explorador de Windows para recuperar el icono de volumen de Windows y el control de volumen de Configuraci\u00f3n r\u00e1pida\n\n"
    L"La barra de tareas desaparecer\u00e1 unos segundos. \u00bfContinuar?",
    L"VolumeOne se ha eliminado y Windows ha quedado como estaba.\n\nYa puedes borrar VolumeOneRemover.exe.",
    L"VolumeOne se ha eliminado, pero no se han podido borrar estos archivos (puede que est\u00e9n en uso). Puedes borrarlos a mano:\n\n",
};

static const Strings& S() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH ? kEs : kEn;
}

// --- Helpers ----------------------------------------------------------------

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

static bool SameFile(const std::wstring& a, const std::wstring& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}

static void AddUnique(std::vector<std::wstring>& list, const std::wstring& path) {
    if (path.empty()) return;
    for (const auto& p : list)
        if (_wcsicmp(p.c_str(), path.c_str()) == 0) return;
    list.push_back(path);
}

static std::wstring ProcessPath(DWORD pid) {
    std::wstring path;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD size = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(h, 0, buf, &size)) path = buf;
        CloseHandle(h);
    }
    return path;
}

static std::vector<DWORD> FindProcesses(const wchar_t* exeName, bool sameSession) {
    std::vector<DWORD> pids;
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        DWORD s = 0;
        if (_wcsicmp(pe.szExeFile, exeName) != 0) continue;
        if (sameSession && (!ProcessIdToSessionId(pe.th32ProcessID, &s) || s != session)) continue;
        pids.push_back(pe.th32ProcessID);
    }
    CloseHandle(snap);
    return pids;
}

// --- Steps ------------------------------------------------------------------

// Asks VolumeOne to quit the normal way (so it gives the Windows icon back),
// then ends whatever is left. Returns the paths of the exes that were running.
static std::vector<std::wstring> CloseVolumeOne() {
    std::vector<std::wstring> exes;
    std::vector<HANDLE> procs;
    for (DWORD pid : FindProcesses(L"VolumeOne.exe", true)) {
        AddUnique(exes, ProcessPath(pid));
        if (HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid)) procs.push_back(h);
    }
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, HOST_WINDOW_CLASS, nullptr));) PostMessageW(w, WM_CLOSE, 0, 0);
    for (HANDLE h : procs) {
        if (WaitForSingleObject(h, 5000) != WAIT_OBJECT_0) {
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 5000);
        }
        CloseHandle(h);
    }
    return exes;
}

// The startup entry, and the registry key settings lived in before 1.6.0.
static std::wstring RemoveRegistry() {
    std::wstring startupExe;
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, APP_NAME, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
        // "\"C:\...\VolumeOne.exe\" /background"
        std::wstring cmd = buf;
        if (cmd.size() > 1 && cmd[0] == L'"') {
            size_t end = cmd.find(L'"', 1);
            if (end != std::wstring::npos) startupExe = cmd.substr(1, end - 1);
        }
    }
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, APP_NAME);
        RegCloseKey(key);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, kLegacyKey);
    RegDeleteKeyW(HKEY_CURRENT_USER, kLegacyKey);
    return startupExe;
}

// Windows remembers every tray icon it has seen (shown or in the overflow) in
// HKCU\Control Panel\NotifyIconSettings; forget VolumeOne's.
static void RemoveTrayIconSettings() {
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kNotifyIconKey, 0, KEY_READ | KEY_WRITE, &root) != ERROR_SUCCESS) return;
    std::vector<std::wstring> remove;
    wchar_t name[256];
    for (DWORD i = 0;; i++) {
        DWORD len = ARRAYSIZE(name);
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t path[MAX_PATH * 2];
        DWORD size = sizeof(path);
        if (RegGetValueW(root, name, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) continue;
        size_t n = wcslen(path);
        const wchar_t suffix[] = L"\\VolumeOne.exe";
        size_t m = ARRAYSIZE(suffix) - 1;
        if (n >= m && _wcsicmp(path + n - m, suffix) == 0) remove.push_back(name);
    }
    for (const auto& k : remove) RegDeleteTreeW(root, k.c_str()), RegDeleteKeyW(root, k.c_str());
    RegCloseKey(root);
}

static DWORD ExplorerPid() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD pid = 0;
    if (tray) GetWindowThreadProcessId(tray, &pid);
    return pid;
}

static DWORD WaitForNewExplorer(DWORD oldPid) {
    for (int i = 0; i < 100; i++) {
        DWORD pid = ExplorerPid();
        if (pid && pid != oldPid) return pid;
        Sleep(100);
    }
    return 0;
}

// Same as VolumeOne's TapClient: the Restart Manager is the supported way to
// restart explorer (it also reopens File Explorer windows).
static void RestartExplorer() {
    DWORD pid = ExplorerPid();
    if (pid) {
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
        if (WaitForNewExplorer(pid)) return;
    }
    if (!ExplorerPid()) {  // the Restart Manager didn't bring it back
        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        wchar_t cmd[] = L"explorer.exe";
        if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        WaitForNewExplorer(pid);
    }
}

// The Quick Settings panel; sihost starts a new one as soon as it exits.
static void RestartShellHost() {
    for (DWORD pid : FindProcesses(L"ShellHost.exe", true)) {
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 5000);
            CloseHandle(h);
        }
    }
}

// Deletes a file, retrying for a while: the dll stays locked until the
// restarted processes have fully exited.
static bool DeleteWithRetry(const fs::path& p) {
    std::error_code ec;
    for (int i = 0; i < 20; i++) {
        if (!fs::exists(p, ec)) return true;
        if (fs::is_directory(p, ec))
            fs::remove_all(p, ec);
        else {
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            fs::remove(p, ec);
        }
        if (!fs::exists(p, ec)) return true;
        Sleep(250);
    }
    return false;
}

static void DeleteExe(const std::wstring& exe, std::vector<std::wstring>& failed) {
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    if (SameFile(exe, self)) return;
    std::error_code ec;
    if (!fs::exists(exe, ec)) return;
    for (const wchar_t* ext : {L"", L".old", L".new"}) {
        std::wstring p = exe + ext;
        if (fs::exists(p, ec) && !DeleteWithRetry(p)) failed.push_back(p);
    }
    // The folder "Move to Documents" makes, if nothing else is left in it.
    fs::path dir = fs::path(exe).parent_path();
    if (_wcsicmp(dir.filename().c_str(), APP_NAME) == 0 && fs::is_empty(dir, ec)) fs::remove(dir, ec);
}

// --- Main -------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int) {
    bool quiet = cmdLine && wcsstr(cmdLine, L"/quiet");
    if (!quiet && MessageBoxW(nullptr, S().confirm, kTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return 1;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HCURSOR wait = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    std::vector<std::wstring> exes = CloseVolumeOne();
    AddUnique(exes, RemoveRegistry());
    std::wstring docs = KnownFolder(FOLDERID_Documents);
    if (!docs.empty()) AddUnique(exes, docs + L"\\VolumeOne\\VolumeOne.exe");

    // Unload the dll and bring back the Windows volume icon and slider.
    RestartExplorer();
    RestartShellHost();
    RemoveTrayIconSettings();

    std::vector<std::wstring> failed;
    std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    if (!local.empty()) {
        std::wstring dir = local + L"\\" APP_NAME;
        if (!DeleteWithRetry(dir)) failed.push_back(dir);
    }
    for (const auto& exe : exes) DeleteExe(exe, failed);

    SetCursor(wait);
    CoUninitialize();

    if (!quiet) {
        if (failed.empty())
            MessageBoxW(nullptr, S().done, kTitle, MB_OK | MB_ICONINFORMATION);
        else {
            std::wstring msg = S().leftovers;
            for (const auto& f : failed) msg += f + L"\n";
            MessageBoxW(nullptr, msg.c_str(), kTitle, MB_OK | MB_ICONWARNING);
        }
    }
    return failed.empty() ? 0 : 2;
}
