#pragma once

#include "common.h"

#include <map>

// Persistent settings, stored under HKCU\Software\VolumeOne.
struct Settings {
    bool hideSystemVolumeIcon = false;
    bool useSystemVolumeIcon = false;  // open VolumeOne from the Windows icon, no own tray icon
    bool hideQuickSettingsVolume = false;  // hide the volume slider in Windows' Quick Settings
    std::vector<std::wstring> hiddenDevices;  // endpoint ids (outputs and inputs)
    // Per-device icon: "speakers", "headphones" or the path of a custom icon
    // copied into AppDataDir("icons"). Missing = automatic (from form factor).
    std::map<std::wstring, std::wstring> deviceIcons;

    void Load();
    void Save() const;

    bool IsDeviceHidden(const std::wstring& id) const;
    void SetDeviceHidden(const std::wstring& id, bool hidden);
};

// %LOCALAPPDATA%\VolumeOne[\sub], created if needed.
std::wstring AppDataDir(const wchar_t* sub = nullptr);

bool GetStartWithWindows();
void SetStartWithWindows(bool enable);
