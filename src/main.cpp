#include "common.h"
#include "audio.h"
#include "battery.h"
#include "flyout.h"
#include "log.h"
#include "settings.h"
#include "strings.h"
#include "tapclient.h"
#include "updater.h"

#include <shlobj.h>

#include <atomic>
#include <filesystem>
#include <cmath>

namespace {

const wchar_t kHostClass[] = HOST_WINDOW_CLASS;
const UINT kTrayId = 1;
const UINT_PTR kTimerTrayRetry = 1;
const UINT_PTR kTimerTapRetry = 2;
const UINT_PTR kTimerUpdateCheck = 3;
const UINT_PTR kTimerTapWatch = 4;  // re-inject if explorer or ShellHost restarted
const UINT kTapWatchMs = 60 * 1000;
const UINT kUpdateCheckIntervalMs = 24 * 60 * 60 * 1000;

enum MenuId { IdOpen = 1, IdSettings, IdSoundSettings, IdWindowsMixer, IdUpdate, IdLogs, IdFolder, IdInputFirst = 100, IdOutputFirst = 200 };

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

bool g_restoreOnExit = false;  // "Close and restore Windows" was chosen

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
    RECT rc = {0, 0, size, size};
    if (glyph != 0xE74F) {
        // All the bars at 40%, under the level glyph, like the Windows icon.
        const wchar_t bars = 0xEBC5;
        SetTextColor(dc, RGB(102, 102, 102));
        DrawTextW(dc, &bars, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SetTextColor(dc, RGB(255, 255, 255));
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

void RemoveTray();

// explorer needs the dll to hide the Windows icon or take its clicks;
// ShellHost to hide the Quick Settings volume slider.
bool ExplorerNeedsTap() { return g_settings.hideSystemVolumeIcon || g_settings.useSystemVolumeIcon; }
bool NeedsTap() { return ExplorerNeedsTap() || g_settings.hideQuickSettingsVolume; }

void UpdateTray(bool forceIcon = false) {
    if (g_settings.useSystemVolumeIcon) {  // the Windows icon is our icon
        if (g_trayAdded) RemoveTray();
        return;
    }
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

    if (g_trayAdded && !Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        LOG(L"tray: modify failed (%lu), adding it again", GetLastError());
        g_trayAdded = false;
    }
    if (!g_trayAdded) {
        static bool loggedFail = false;  // it retries every second until explorer is ready
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            BOOL v4 = Shell_NotifyIconW(NIM_SETVERSION, &nid);
            LOG(L"tray: icon added (version 4: %d)", v4);
            loggedFail = false;
            g_trayAdded = true;
            KillTimer(g_host, kTimerTrayRetry);
        } else {
            if (!loggedFail) LOG(L"tray: add failed (%lu), retrying", GetLastError());
            loggedFail = true;
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
    UINT_PTR need = (ExplorerNeedsTap() ? 1 : 0) | (g_settings.hideQuickSettingsVolume ? 2 : 0);
    LOG(L"tap: sync (explorer=%d, shellhost=%d)", (int)(need & 1), (int)((need & 2) != 0));
    HANDLE h = CreateThread(nullptr, 0, [](void* p) -> DWORD {
        UINT_PTR need = (UINT_PTR)p;
        bool ok = TapClient::Sync((need & 1) != 0, (need & 2) != 0);
        PostMessageW(g_host, WM_APP_TAP_DONE, ok, 0);
        return 0;
    }, (void*)need, 0, nullptr);
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
    HANDLE h = CreateThread(nullptr, 0, [](void* manual) -> DWORD {
        ULONGLONG start = GetTickCount64();
        auto info = new Updater::ReleaseInfo;
        if (!Updater::FetchLatest(*info)) {
            delete info;
            info = nullptr;
        }
        // A manual check keeps "Checking…" up for a moment, or it never shows.
        ULONGLONG elapsed = GetTickCount64() - start;
        if (manual && elapsed < 1000) Sleep(DWORD(1000 - elapsed));
        PostMessageW(g_host, WM_APP_UPDATE_CHECKED, 0, (LPARAM)info);
        return 0;
    }, (void*)(INT_PTR)manual, 0, nullptr);
    if (h) CloseHandle(h);
}

void OnUpdateChecked(Updater::ReleaseInfo* info) {
    LOG(L"update: check %s%s", info ? L"ok, latest " : L"failed", info ? info->version.c_str() : L"");
    if (info && Updater::IsNewer(info->version, APP_VERSION)) {
        g_release = *info;
        g_update.version = info->version;
        g_update.notes = info->notes;
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

// --- Where the exe lives ------------------------------------------------------

#define MOVED_ARG L"/moved"

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

// Localized name of a known folder ("Descargas", "Downloads", ...).
static std::wstring KnownFolderName(REFKNOWNFOLDERID id) {
    ComPtr<IShellItem> item;
    PWSTR name = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderItem(id, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&item))) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)))
        s = name;
    CoTaskMemFree(name);
    return s;
}

static bool IsInside(const std::wstring& path, const std::wstring& folder) {
    if (folder.empty() || path.size() <= folder.size()) return false;
    return _wcsnicmp(path.c_str(), folder.c_str(), folder.size()) == 0 && (path[folder.size()] == L'\\');
}

// Name of the folder if the exe is somewhere that tends to get cleaned up
// (Downloads, Desktop, Temp), empty otherwise. Known folders, so it works
// whatever the Windows language or if the folders were moved.
std::wstring RiskyLocationName() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    for (REFKNOWNFOLDERID id : {FOLDERID_Downloads, FOLDERID_Desktop})
        if (IsInside(exe, KnownFolder(id))) return KnownFolderName(id);
    wchar_t temp[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temp)) {
        std::wstring t = temp;
        if (!t.empty() && t.back() == L'\\') t.pop_back();
        if (IsInside(exe, t)) return L"Temp";
    }
    return {};
}

// Copies the exe to Documents\VolumeOne, points "Start with Windows" there,
// and starts the copy, which deletes this one once we have exited.
bool MoveToDocuments() {
    std::wstring dir = KnownFolder(FOLDERID_Documents);
    if (dir.empty()) return false;
    dir += L"\\VolumeOne";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring target = dir + L"\\VolumeOne.exe";
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!CopyFileW(exe, target.c_str(), FALSE)) return false;
    if (GetStartWithWindows()) SetStartWithWindows(true, target.c_str());

    std::wstring cmd = L"\"" + target + L"\" " MOVED_ARG L" " + std::to_wstring(GetCurrentProcessId()) + L" \"" + exe + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(target.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// The moved copy: wait for the old process, then delete the old exe.
void FinishMove(const wchar_t* cmdLine) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    for (int i = 0; argv && i + 2 < argc; i++) {
        if (wcscmp(argv[i], MOVED_ARG) != 0) continue;
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)_wtoi(argv[i + 1]))) {
            WaitForSingleObject(old, 15000);
            CloseHandle(old);
        }
        for (int t = 0; t < 10 && !DeleteFileW(argv[i + 2]) && GetLastError() != ERROR_FILE_NOT_FOUND; t++) Sleep(500);
        break;
    }
    LocalFree(argv);
}

// "Close and restore Windows": no startup entry, and after the message loop
// ends, explorer/ShellHost are restarted without our dll and its copies are
// deleted. Settings and custom icons stay for the next launch.
void CloseAndRestore() {
    LOG(L"close and restore Windows");
    SetStartWithWindows(false);
    g_restoreOnExit = true;
    DestroyWindow(g_host);
}

// --- Flyout & menu ----------------------------------------------------------

void EnsureFlyout() {
    if (g_flyoutCreated) return;
    static bool failed = false;
    if (failed) return;  // already reported; Create isn't safe to call twice
    g_flyoutCreated = g_flyout.Create(g_inst, g_host, &g_audio, &g_settings, &g_update);
    LOG(L"flyout: create %s", g_flyoutCreated ? L"ok" : L"FAILED");
    if (!g_flyoutCreated) {
        // Without the flyout there are no settings to undo anything with:
        // offer the way out here instead.
        failed = true;
        std::wstring msg = std::wstring(S().flyoutFailed) + L"\n\n" + Log::Folder();
        int answer = MessageBoxW(nullptr, msg.c_str(), APP_NAME, MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND);
        Log::OpenFolder();
        if (answer == IDYES) CloseAndRestore();
        return;
    }
    g_flyout.onHideIconChanged = [] {
        UpdateTray(true);  // adds or removes our own icon
        g_tapRetries = 0;
        StartTapSync();
    };
    g_flyout.onCheckUpdates = [] { CheckForUpdates(true); };
    g_flyout.onCloseAndRestore = [] { CloseAndRestore(); };
    g_flyout.onMoveToDocuments = [] {
        if (MoveToDocuments())
            DestroyWindow(g_host);
        else
            MessageBoxW(g_flyout.Hwnd(), S().moveFailed, APP_NAME, MB_OK | MB_ICONWARNING);
    };
    g_flyout.locationName = RiskyLocationName();
    g_flyout.onInstallUpdate = [] { InstallUpdate(); };
    g_flyout.onOpenLogs = [] { Log::OpenFolder(); };
}

void ShowMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();

    // Default output and input, as radio lists (minus the ones hidden in
    // settings).
    std::vector<std::wstring> outputIds;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, S().audioOutput);
    for (const AudioDevice& d : g_audio.Devices()) {
        if (!d.isDefault && g_settings.IsDeviceHidden(d.id)) continue;  // the current one always shows
        UINT id = IdOutputFirst + (UINT)outputIds.size();
        std::wstring name = g_settings.DisplayDeviceName(d.name, d.technicalName);
        AppendMenuW(menu, MF_STRING, id, name.c_str());
        if (d.isDefault) CheckMenuRadioItem(menu, id, id, id, MF_BYCOMMAND);
        outputIds.push_back(d.id);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    std::vector<std::wstring> inputIds;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, S().audioInput);
    for (const InputDevice& in : g_audio.Inputs()) {
        if (!in.isDefault && g_settings.IsDeviceHidden(in.id)) continue;
        UINT id = IdInputFirst + (UINT)inputIds.size();
        std::wstring name = g_settings.DisplayDeviceName(in.name, in.technicalName);
        AppendMenuW(menu, MF_STRING, id, name.c_str());
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
    AppendMenuW(menu, MF_STRING, IdLogs, S().openLogs);
    AppendMenuW(menu, MF_STRING, IdFolder, S().openContainingFolder);
    if (g_update.status == UpdateState::Available) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        std::wstring label = std::wstring(S().updateNow) + L" (" + g_update.version + L")";
        AppendMenuW(menu, MF_STRING, IdUpdate, label.c_str());
    }
    SetMenuDefaultItem(menu, IdOpen, FALSE);

    SetForegroundWindow(g_host);
    UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align, x, y, g_host, nullptr);
    LOG(L"menu at %d,%d: command %d", x, y, cmd);
    PostMessageW(g_host, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd >= IdOutputFirst && cmd < IdOutputFirst + (int)outputIds.size()) {
        g_audio.SetDefaultDevice(outputIds[cmd - IdOutputFirst]);
        return;
    }
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
            if (g_flyoutCreated) g_flyout.Show();
            break;
        case IdSettings:
            EnsureFlyout();
            if (g_flyoutCreated) g_flyout.Show(true);
            break;
        case IdSoundSettings:
            ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IdWindowsMixer:
            ShellExecuteW(nullptr, L"open", L"ms-settings:apps-volume", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IdLogs:
            Log::OpenFolder();
            break;
        case IdFolder: {
            // Opens Explorer on our folder with VolumeOne.exe selected.
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(exe)) {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
            break;
        }
    }
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && msg != 0) {  // explorer (re)started
        LOG(L"TaskbarCreated: explorer (re)started");
        g_trayAdded = false;
        UpdateTray(true);
        g_tapRetries = 0;
        if (NeedsTap()) StartTapSync();
        return 0;
    }

    switch (msg) {
        case WM_APP_TRAY:
            if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT || LOWORD(lp) == WM_CONTEXTMENU ||
                LOWORD(lp) == WM_MBUTTONUP)
                LOG(L"tray: event %04X", LOWORD(lp));
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    EnsureFlyout();
                    if (g_flyoutCreated) g_flyout.Toggle();
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
            Battery::Refresh();
            return 0;

        case WM_APP_BATTERY_CHANGED:
            if (g_flyoutCreated) g_flyout.OnVolumeChanged();
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

        case WM_VOLUMEONE_SYSICON:  // the Windows volume icon was clicked (see tap.cpp)
            if (wp != SYSICON_WHEEL) LOG(L"system icon: event %d (useSystemIcon=%d)", (int)wp, g_settings.useSystemVolumeIcon);
            if (!g_settings.useSystemVolumeIcon) return 0;
            if (wp == SYSICON_CLICK) {
                EnsureFlyout();
                if (g_flyoutCreated) g_flyout.Toggle();
            } else if (wp == SYSICON_MENU) {
                // The cursor is on the taskbar, which would cover the bottom of
                // the menu: anchor it to the edge of the work area instead.
                POINT pt;
                GetCursorPos(&pt);
                MONITORINFO mi = {sizeof(mi)};
                GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
                pt.x = std::clamp(pt.x, mi.rcWork.left, mi.rcWork.right - 1);
                pt.y = std::clamp(pt.y, mi.rcWork.top, mi.rcWork.bottom - 1);
                ShowMenu(pt.x, pt.y);
            } else if (wp == SYSICON_MIDDLE || wp == SYSICON_WHEEL) {
                AudioDevice* d = g_audio.DefaultDevice();
                if (!d) return 0;
                if (wp == SYSICON_MIDDLE) {
                    g_audio.SetDeviceMute(*d, !d->muted);
                } else {
                    float v = d->volume + (int)(short)lp / (float)WHEEL_DELTA * 0.02f;
                    g_audio.SetDeviceVolume(*d, std::clamp(std::round(v * 100.0f) / 100.0f, 0.0f, 1.0f));
                }
                // Feedback, unless the mixer is open and already shows it.
                EnsureFlyout();
                if (g_flyoutCreated && !g_flyout.IsVisible()) g_flyout.ShowVolumeOsd(*d);
            }
            return 0;

        case WM_APP_SHOW_FLYOUT:
            LOG(L"show flyout requested (settings=%d)", (int)wp);
            EnsureFlyout();
            if (g_flyoutCreated) g_flyout.Show(wp != 0);  // wParam 1: open in Settings (the /settings argument)
            return 0;

        case WM_APP_TAP_DONE:
            LOG(L"tap: sync %s (retry %d)", wp ? L"ok" : L"failed", g_tapRetries);
            g_tapBusy = false;
            if (!wp && NeedsTap() && g_tapRetries++ < 60)
                SetTimer(hwnd, kTimerTapRetry, 2000, nullptr);  // taskbar not ready, e.g. right after sign-in
            return 0;

        case WM_APP_UPDATE_CHECKED:
            OnUpdateChecked((Updater::ReleaseInfo*)lp);
            return 0;

        case WM_APP_UPDATE_INSTALLED:
            LOG(L"update: install %s", wp ? L"ok, restarting" : L"failed");
            if (wp)
                DestroyWindow(hwnd);  // the new version is already starting
            else
                SetUpdateStatus(UpdateState::InstallFailed);
            return 0;

        case WM_TIMER:
            if (wp == kTimerTapWatch) {
                if (NeedsTap()) StartTapSync();
                return 0;
            }
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
    Log::Init(cmdLine);

    // After an update the new exe is started by the old one: wait for it to
    // exit (it holds the single-instance mutex) and delete the old file.
    if (const wchar_t* upd = wcsstr(cmdLine, UPDATED_ARG)) Updater::FinishUpdate((DWORD)_wtoi(upd + wcslen(UPDATED_ARG)));
    // Same after "Move to Documents".
    if (wcsstr(cmdLine, MOVED_ARG)) FinishMove(GetCommandLineW());

    // Single instance: a second launch just opens the running one's flyout.
    g_runningMutex = CreateMutexW(nullptr, FALSE, RUNNING_MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kHostClass, nullptr);
        LOG(L"already running (host window %s): asking it to open, then exiting", other ? L"found" : L"NOT found");
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_APP_SHOW_FLYOUT, wcsstr(cmdLine, L"/settings") != nullptr, 0);
        }
        return 0;
    }
    bool background = wcsstr(cmdLine, L"/background") != nullptr || wcsstr(cmdLine, UPDATED_ARG) != nullptr ||
                      wcsstr(cmdLine, MOVED_ARG) != nullptr;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitDarkMenus();
    g_settings.Load();
    LOG(L"settings: useSystemIcon=%d hideSystemIcon=%d hideQsVolume=%d startWithWindows=%d background=%d",
        g_settings.useSystemVolumeIcon, g_settings.hideSystemVolumeIcon, g_settings.hideQuickSettingsVolume,
        GetStartWithWindows(), background);

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
    Battery::Start(g_host);
    UpdateTray(true);
    if (NeedsTap()) StartTapSync();
    if (!background) PostMessageW(g_host, WM_APP_SHOW_FLYOUT, 0, 0);
    CheckForUpdates(false);  // on a worker thread
    SetTimer(g_host, kTimerUpdateCheck, kUpdateCheckIntervalMs, nullptr);
    SetTimer(g_host, kTimerTapWatch, kTapWatchMs, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LOG(L"exiting (restore Windows: %d)", g_restoreOnExit);

    RemoveTray();
    Battery::Stop();
    g_audio.Shutdown();
    // The host window is gone now, so the explorer dll restores the Windows
    // volume icon when it re-checks.
    TapClient::Notify();
    if (g_restoreOnExit) {
        TapClient::Unload();
        std::error_code ec;
        wchar_t* local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(local) / APP_NAME, ec))
                if (_wcsnicmp(e.path().filename().c_str(), L"VolumeOneTap", 12) == 0) std::filesystem::remove(e.path(), ec);
        }
        CoTaskMemFree(local);
    }
    CloseHandle(g_runningMutex);
    CoUninitialize();
    LOG(L"exited");
    return 0;
}
