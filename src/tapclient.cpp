#include "tapclient.h"

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

static bool Inject(DWORD pid) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!len || len >= MAX_PATH) return false;
    PathRemoveFileSpecW(path);
    if (!PathAppendW(path, TAP_DLL_NAME) || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;

    using InitFn = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!xaml) return false;
    auto init = (InitFn)GetProcAddress(xaml, "InitializeXamlDiagnosticsEx");
    if (!init) return false;

    // The taskbar's XAML may not exist yet right after sign-in; the caller
    // retries until this succeeds.
    HRESULT hr = init(L"VisualDiagConnection1", pid, nullptr, path, CLSID_VolumeOneTap, nullptr);
    return SUCCEEDED(hr);
}

bool Sync(bool hide) {
    DWORD pid = ExplorerPid();
    if (!pid) return false;
    if (!IsLoaded(pid)) {
        if (!hide) return true;  // nothing to restore
        if (!Inject(pid)) return false;
    }
    Notify();
    return true;
}

void Notify() {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, TAP_REFRESH_EVENT_NAME);
    if (ev) {
        SetEvent(ev);
        CloseHandle(ev);
    }
}

}  // namespace TapClient
