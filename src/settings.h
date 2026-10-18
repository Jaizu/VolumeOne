#pragma once

#include "common.h"

// Persistent settings, stored under HKCU\Software\VolumeOne.
struct Settings {
    bool hideSystemVolumeIcon = false;
    std::vector<std::wstring> hiddenDevices;  // endpoint ids

    void Load();
    void Save() const;

    bool IsDeviceHidden(const std::wstring& id) const;
    void SetDeviceHidden(const std::wstring& id, bool hidden);
};

bool GetStartWithWindows();
void SetStartWithWindows(bool enable);
