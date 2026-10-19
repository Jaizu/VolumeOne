#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <vector>

#include "shared.h"

using Microsoft::WRL::ComPtr;

#define APP_NAME L"VolumeOne"
#include "version.h"

#define APP_VERSION_W2(s) L##s
#define APP_VERSION_W(s) APP_VERSION_W2(s)
#define APP_VERSION APP_VERSION_W(APP_VERSION_A)
#define APP_REG_KEY REG_KEY_PATH

// Window messages shared between modules.
enum : UINT {
    WM_APP_TRAY = WM_APP + 1,       // tray icon callback
    WM_APP_DEVICES_CHANGED,         // device added/removed/default changed
    WM_APP_VOLUME_CHANGED,          // endpoint or session volume changed
    WM_APP_SESSIONS_CHANGED,        // session created/expired
    WM_APP_SHOW_FLYOUT,             // second instance asked us to open
    WM_APP_TAP_DONE,                // explorer dll sync finished (wParam = ok)
    WM_APP_UPDATE_CHECKED,          // update check finished (lParam = ReleaseInfo*, may be null)
    WM_APP_UPDATE_INSTALLED,        // update install finished (wParam = ok)
    WM_APP_BATTERY_CHANGED,         // a headset's battery level changed (battery.cpp)
};
