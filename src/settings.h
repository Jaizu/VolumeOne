#pragma once

#include "common.h"

#include <functional>
#include <map>

// Persistent settings, stored in %LOCALAPPDATA%\VolumeOne\settings.ini
// (moved there from HKCU\Software\VolumeOne on the first run of 1.6.0).
struct Settings {
    bool hideSystemVolumeIcon = true;
    bool useSystemVolumeIcon = true;  // open VolumeOne from the Windows icon, no own tray icon
    bool hideQuickSettingsVolume = true;  // hide the volume slider in Windows' Quick Settings
    enum MeterColor : DWORD { MeterAccent, MeterWhite, MeterGreen, MeterCustom };
    enum DeviceNameMode : DWORD { NameBoth, NameNickname, NameTechnical };
    DWORD meterColor = MeterAccent;
    DWORD meterCustomColor = 0xFF8C00;  // 0xRRGGBB, used with MeterCustom
    DWORD meterCustomBar = 0x0078D4;    // 0xRRGGBB, the slider fill behind it with MeterCustom
    DWORD deviceNameMode = NameBoth;
    bool nyanCat = false;  // sliders drawn as Nyan Cat with a rainbow trail
    std::vector<std::wstring> hiddenDevices;  // endpoint ids (outputs and inputs)
    // Per-device icon (outputs and inputs): "speakers", "headphones" or the
    // path of a custom icon copied into AppDataDir("icons"). Missing =
    // automatic (outputs by form factor, inputs a microphone).
    std::map<std::wstring, std::wstring> deviceIcons;

    // The names of the devices in hiddenDevices and deviceIcons, so their
    // settings can follow them when Windows gives a device a new id (some
    // Windows updates recreate every audio endpoint).
    struct DeviceLabel {
        std::wstring name, technical;
    };
    mutable std::map<std::wstring, DeviceLabel> deviceNames;
    // Set by the app: the names of a device Windows knows (false if it
    // doesn't). Save() uses it to keep deviceNames current.
    std::function<bool(const std::wstring& id, DeviceLabel& out)> lookupDevice;

    struct ActiveDevice {
        std::wstring id;
        DeviceLabel label;
    };
    // Moves the icon and hidden state of each device whose id no longer
    // exists (gone(id)) to the one active device of the same kind with the
    // same names and no settings of its own. Saves if anything changed.
    void RelinkDevices(const std::vector<ActiveDevice>& active, const std::function<bool(const std::wstring&)>& gone);
    bool HasDeviceSettings(const std::wstring& id) const;

    void Load();
    void Save() const;
    std::wstring DisplayDeviceName(const std::wstring& nickname, const std::wstring& technical) const;

    bool IsDeviceHidden(const std::wstring& id) const;
    void SetDeviceHidden(const std::wstring& id, bool hidden);
};

// %LOCALAPPDATA%\VolumeOne[\sub], created if needed.
std::wstring AppDataDir(const wchar_t* sub = nullptr);

bool GetStartWithWindows();
// exePath: the exe to start (default: this one).
void SetStartWithWindows(bool enable, const wchar_t* exePath = nullptr);
