#include "common.h"
#include "audio.h"
#include "flyout.h"
#include "settings.h"
#include "strings.h"
#include "tapclient.h"
#include "updater.h"

#include <atomic>
#include <cmath>

namespace {

const wchar_t kHostClass[] = HOST_WINDOW_CLASS;
const UINT kTrayId = 1;
const UINT_PTR kTimerTrayRetry = 1;
const UINT_PTR kTimerTapRetry = 2;
const UINT_PTR kTimerUpdateCheck = 3;
const UINT kFirstUpdateCheckMs = 15 * 1000;
const UINT kUpdateCheckIntervalMs = 24 * 60 * 60 * 1000;

enum MenuId { IdOpen = 1, IdSettings, IdSoundSettings, IdWindowsMixer, IdUpdate, IdExit, IdInputFirst = 100 };

HINSTANCE g_inst;
HWND g_host;
HANDLE g_runningMutex;
UINT g_taskbarCreated;
Settings g_settings;
AudioManager g_audio;
Flyout g_flyout;
bool g_flyoutCreated = false;

bool g_trayAdded = false;
HICON g_trayIcon = nullptr;
wchar_t g_trayGlyph = 0;
bool g_trayLight = false;

std::atomic<bool> g_tapBusy{false};
int g_tapRetries = 0;

UpdateState g_update;
Updater::ReleaseInfo g_release;
bool g_updateManual = false;  // user clicked "check": also report "up to date"

// --- Dark mode for the context menu (undocumented uxtheme exports) ---------

void InitDarkMenus() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(1);  // AllowDark: follow the system app theme
    if (flush) flush();
}

void FlushMenuThemes() {
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) return;
    using FlushMenuThemesFn = void(WINAPI*)();
    if (auto flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136))) flush();
}

// --- Tray icon --------------------------------------------------------------

bool TaskbarIsLight() {
    DWORD light = 0, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light != 0;
}

// Renders a Segoe Fluent Icons glyph into an icon, the same glyphs the
// Windows volume icon uses, so it looks native in the tray.
HICON MakeGlyphIcon(wchar_t glyph, bool light) {
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    HGDIOBJ oldBmp = SelectObject(dc, color);

    // Draw white on black, then use the coverage as alpha.
    memset(bits, 0, size * size * 4);
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe Fluent Icons");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rc = {0, 0, size, size};
    DrawTextW(dc, &glyph, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();

    BYTE c = light ? 0 : 255;
    auto px = (BYTE*)bits;
    for (int i = 0; i < size * size; i++, px += 4) {
        BYTE a = px[1];  // green channel = grayscale coverage
        px[0] = px[1] = px[2] = (BYTE)(c * a / 255);
        px[3] = a;
    }

    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(font);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

wchar_t TrayGlyph(const AudioDevice* d) {
    if (!d || d->muted) return 0xE74F;
    if (d->volume <= 0.0f) return 0xE992;
    if (d->volume < 0.34f) return 0xE993;
    if (d->volume < 0.67f) return 0xE994;
    return 0xE995;
}

void UpdateTray(bool forceIcon = false) {
    const AudioDevice* d = g_audio.DefaultDevice();
    wchar_t glyph = TrayGlyph(d);
    bool light = TaskbarIsLight();

    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;

    HICON oldIcon = nullptr;
    if (!g_trayIcon || forceIcon || glyph != g_trayGlyph || light != g_trayLight || !g_trayAdded) {
        oldIcon = g_trayIcon;
        g_trayIcon = MakeGlyphIcon(glyph, light);
        g_trayGlyph = glyph;
        g_trayLight = light;
    }
    nid.uFlags |= NIF_ICON;
    nid.hIcon = g_trayIcon;

    std::wstring tip = APP_NAME;
    if (d) {
        tip = d->name + L": " + std::to_wstring((int)std::lround(d->volume * 100)) + L"%";
        if (d->muted) tip += std::wstring(L" (") + S().muted + L")";
    }
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);

    if (g_trayAdded && !Shell_NotifyIconW(NIM_MODIFY, &nid)) g_trayAdded = false;
    if (!g_trayAdded) {
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            g_trayAdded = true;
            KillTimer(g_host, kTimerTrayRetry);
        } else {
            SetTimer(g_host, kTimerTrayRetry, 1000, nullptr);  // explorer not ready yet
        }
    }
    if (oldIcon) DestroyIcon(oldIcon);
}

void RemoveTray() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_host;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

// --- Explorer dll -----------------------------------------------------------

void StartTapSync() {
    if (g_tapBusy.exchange(true)) return;
    bool hide = g_settings.hideSystemVolumeIcon;
    HANDLE h = CreateThread(nullptr, 0, [](void* p) -> DWORD {
        bool ok = TapClient::Sync(p != nullptr);
        PostMessageW(g_host, WM_APP_TAP_DONE, ok, 0);
        return 0;
    }, hide ? (void*)1 : nullptr, 0, nullptr);
    if (h)
        CloseHandle(h);
    else
        g_tapBusy = false;
}

// --- Updates ----------------------------------------------------------------

void SetUpdateStatus(UpdateState::Status status) {
    g_update.status = status;
    if (g_flyoutCreated) g_flyout.OnUpdateStateChanged();
}

void CheckForUpdates(bool manual) {
    if (g_update.status == UpdateState::Checking || g_update.status == UpdateState::Installing) return;
    g_updateManual = manual;
    if (manual) SetUpdateStatus(UpdateState::Checking);
    HANDLE h = CreateThread(nullptr, 0, [](void*) -> DWORD {
        auto info = new Updater::ReleaseInfo;
        if (!Updater::FetchLatest(*info)) {
            delete info;
            info = nullptr;
        }
        PostMessageW(g_host, WM_APP_UPDATE_CHECKED, 0, (LPARAM)info);
        return 0;
    }, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

void OnUpdateChecked(Updater::ReleaseInfo* info) {
    if (info && Updater::IsNewer(info->version, APP_VERSION)) {
        g_release = *info;
        g_update.version = info->version;
        SetUpdateStatus(UpdateState::Available);
    } else if (g_updateManual || g_update.status == UpdateState::Checking) {
        SetUpdateStatus(info ? UpdateState::UpToDate : UpdateState::Failed);
    }
    delete info;
}

void InstallUpdate() {
    if (g_update.status != UpdateState::Available && g_update.status != UpdateState::InstallFailed) return;
    SetUpdateStatus(UpdateState::Installing);
    HANDLE h = CreateThread(nullptr, 0, [](void*) -> DWORD {
        bool ok = Updater::Install(g_release);
        PostMessageW(g_host, WM_APP_UPDATE_INSTALLED, ok, 0);
        return 0;
    }, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

// --- Flyout & menu ----------------------------------------------------------

void EnsureFlyout() {
    if (g_flyoutCreated) return;
    g_flyoutCreated = g_flyout.Create(g_inst, g_host, &g_audio, &g_settings, &g_update);
    g_flyout.onHideIconChanged = [] {
        g_tapRetries = 0;
        StartTapSync();
    };
    g_flyout.onExit = [] { DestroyWindow(g_host); };
    g_flyout.onCheckUpdates = [] { CheckForUpdates(true); };
    g_flyout.onInstallUpdate = [] { InstallUpdate(); };
}

void ShowMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();

    // Default audio input, as a radio list (minus the ones hidden in settings).
    std::vector<std::wstring> inputIds;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, S().audioInput);
    for (const InputDevice& in : g_audio.Inputs()) {
        if (g_settings.IsDeviceHidden(in.id)) continue;
        UINT id = IdInputFirst + (UINT)inputIds.size();
        AppendMenuW(menu, MF_STRING, id, in.name.c_str());
        if (in.isDefault) CheckMenuRadioItem(menu, id, id, id, MF_BYCOMMAND);
        inputIds.push_back(in.id);
    }
    if (inputIds.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, S().noInputs);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IdOpen, S().openMixer);
    AppendMenuW(menu, MF_STRING, IdSettings, S().settings);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IdSoundSettings, S().soundSettings);
    AppendMenuW(menu, MF_STRING, IdWindowsMixer, S().windowsMixer);
    if (g_update.status == UpdateState::Available) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        std::wstring label = std::wstring(S().updateNow) + L" (" + g_update.version + L")";
        AppendMenuW(menu, MF_STRING, IdUpdate, label.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IdExit, S().exit);
    SetMenuDefaultItem(menu, IdOpen, FALSE);

    SetForegroundWindow(g_host);
    UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align, x, y, g_host, nullptr);
    PostMessageW(g_host, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd >= IdInputFirst && cmd < IdInputFirst + (int)inputIds.size()) {
        g_audio.SetDefaultDevice(inputIds[cmd - IdInputFirst]);
        return;
    }
    switch (cmd) {
        case IdUpdate:
            InstallUpdate();
            break;
        case IdOpen:
            EnsureFlyout();
            g_flyout.Show();
            break;
        case IdSettings:
            EnsureFlyout();
            g_flyout.Show(true);
            break;
        case IdSoundSettings:
            ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IdWindowsMixer:
            ShellExecuteW(nullptr, L"open", L"ms-settings:apps-volume", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IdExit:
            DestroyWindow(g_host);
            break;
    }
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && msg != 0) {  // explorer (re)started
        g_trayAdded = false;
        UpdateTray(true);
        g_tapRetries = 0;
        if (g_settings.hideSystemVolumeIcon) StartTapSync();
        return 0;
    }

    switch (msg) {
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    EnsureFlyout();
                    g_flyout.Toggle();
                    break;
                case WM_CONTEXTMENU:
                    ShowMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
                    break;
                case WM_MBUTTONUP:  // middle click toggles mute
                    if (AudioDevice* d = g_audio.DefaultDevice()) g_audio.SetDeviceMute(*d, !d->muted);
                    break;
            }
            return 0;

        case WM_APP_DEVICES_CHANGED:
            g_audio.RefreshDevices();
            UpdateTray();
            if (g_flyoutCreated) g_flyout.OnAudioStructureChanged();
            return 0;

        case WM_APP_SESSIONS_CHANGED:
            if (g_flyoutCreated && g_flyout.IsVisible()) {
                g_audio.RefreshSessions();
                g_flyout.OnAudioStructureChanged();
            }
            return 0;

        case WM_APP_VOLUME_CHANGED:
            g_audio.RefreshVolumes();
            UpdateTray();
            if (g_flyoutCreated) g_flyout.OnVolumeChanged();
            return 0;

        case WM_APP_SHOW_FLYOUT:
            EnsureFlyout();
            g_flyout.Show();
            return 0;

        case WM_APP_TAP_DONE:
            g_tapBusy = false;
            if (!wp && g_settings.hideSystemVolumeIcon && g_tapRetries++ < 60)
                SetTimer(hwnd, kTimerTapRetry, 2000, nullptr);  // taskbar not ready, e.g. right after sign-in
            return 0;

        case WM_APP_UPDATE_CHECKED:
            OnUpdateChecked((Updater::ReleaseInfo*)lp);
            return 0;

        case WM_APP_UPDATE_INSTALLED:
            if (wp)
                DestroyWindow(hwnd);  // the new version is already starting
            else
                SetUpdateStatus(UpdateState::InstallFailed);
            return 0;

        case WM_TIMER:
            if (wp == kTimerUpdateCheck) {
                SetTimer(hwnd, kTimerUpdateCheck, kUpdateCheckIntervalMs, nullptr);
                CheckForUpdates(false);
                return 0;
            }
            KillTimer(hwnd, wp);
            if (wp == kTimerTrayRetry) UpdateTray(true);
            if (wp == kTimerTapRetry) StartTapSync();
            return 0;

        case WM_SETTINGCHANGE:
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) {
                FlushMenuThemes();
                UpdateTray(true);
                if (g_flyoutCreated) g_flyout.OnThemeChanged();
            }
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            UpdateTray(true);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    g_inst = inst;

    // After an update the new exe is started by the old one: wait for it to
    // exit (it holds the single-instance mutex) and delete the old file.
    if (const wchar_t* upd = wcsstr(cmdLine, UPDATED_ARG)) Updater::FinishUpdate((DWORD)_wtoi(upd + wcslen(UPDATED_ARG)));

    // Single instance: a second launch just opens the running one's flyout.
    g_runningMutex = CreateMutexW(nullptr, FALSE, RUNNING_MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kHostClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_APP_SHOW_FLYOUT, 0, 0);
        }
        return 0;
    }
    bool background = wcsstr(cmdLine, L"/background") != nullptr || wcsstr(cmdLine, UPDATED_ARG) != nullptr;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitDarkMenus();
    g_settings.Load();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = inst;
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);
    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, APP_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_host, g_taskbarCreated, MSGFLT_ALLOW, nullptr);

    // Only what the tray icon needs happens before the message loop; the
    // flyout's Direct2D resources are created on first use.
    g_audio.Init(g_host);
    UpdateTray(true);
    if (g_settings.hideSystemVolumeIcon) StartTapSync();
    if (!background) PostMessageW(g_host, WM_APP_SHOW_FLYOUT, 0, 0);
    SetTimer(g_host, kTimerUpdateCheck, kFirstUpdateCheckMs, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveTray();
    g_audio.Shutdown();
    // The host window is gone now, so the explorer dll restores the Windows
    // volume icon when it re-checks.
    TapClient::Notify();
    CloseHandle(g_runningMutex);
    CoUninitialize();
    return 0;
}
