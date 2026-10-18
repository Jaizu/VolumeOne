#include "settings.h"

#include <shlobj.h>

static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

void Settings::Load() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, APP_REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;

    DWORD value = 0, size = sizeof(value);
    if (RegQueryValueExW(key, REG_HIDE_ICON, nullptr, nullptr, (BYTE*)&value, &size) == ERROR_SUCCESS)
        hideSystemVolumeIcon = value != 0;
    size = sizeof(value);
    if (RegQueryValueExW(key, REG_USE_SYSTEM_ICON, nullptr, nullptr, (BYTE*)&value, &size) == ERROR_SUCCESS)
        useSystemVolumeIcon = value != 0;
    size = sizeof(value);
    if (RegQueryValueExW(key, REG_HIDE_QS_VOLUME, nullptr, nullptr, (BYTE*)&value, &size) == ERROR_SUCCESS)
        hideQuickSettingsVolume = value != 0;

    hiddenDevices.clear();
    size = 0;
    if (RegQueryValueExW(key, L"HiddenDevices", nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS && size > 0) {
        std::wstring buf(size / sizeof(wchar_t) + 2, L'\0');
        if (RegQueryValueExW(key, L"HiddenDevices", nullptr, nullptr, (BYTE*)buf.data(), &size) == ERROR_SUCCESS) {
            for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1)
                hiddenDevices.emplace_back(p);
        }
    }

    deviceIcons.clear();
    HKEY icons;
    if (RegOpenKeyExW(key, L"DeviceIcons", 0, KEY_READ, &icons) == ERROR_SUCCESS) {
        wchar_t name[512], data[MAX_PATH * 2];
        for (DWORD i = 0;; i++) {
            DWORD nameLen = ARRAYSIZE(name), dataLen = sizeof(data), type = 0;
            if (RegEnumValueW(icons, i, name, &nameLen, nullptr, &type, (BYTE*)data, &dataLen) != ERROR_SUCCESS) break;
            if (type == REG_SZ) deviceIcons[name] = data;
        }
        RegCloseKey(icons);
    }
    RegCloseKey(key);
}

void Settings::Save() const {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    DWORD value = hideSystemVolumeIcon ? 1 : 0;
    RegSetValueExW(key, REG_HIDE_ICON, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    value = useSystemVolumeIcon ? 1 : 0;
    RegSetValueExW(key, REG_USE_SYSTEM_ICON, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    value = hideQuickSettingsVolume ? 1 : 0;
    RegSetValueExW(key, REG_HIDE_QS_VOLUME, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));

    std::wstring multi;
    for (const auto& id : hiddenDevices) {
        multi += id;
        multi.push_back(L'\0');
    }
    multi.push_back(L'\0');
    RegSetValueExW(key, L"HiddenDevices", 0, REG_MULTI_SZ, (const BYTE*)multi.data(),
                   (DWORD)(multi.size() * sizeof(wchar_t)));

    RegDeleteTreeW(key, L"DeviceIcons");
    HKEY icons;
    if (RegCreateKeyExW(key, L"DeviceIcons", 0, nullptr, 0, KEY_WRITE, nullptr, &icons, nullptr) == ERROR_SUCCESS) {
        for (const auto& [id, value] : deviceIcons)
            RegSetValueExW(icons, id.c_str(), 0, REG_SZ, (const BYTE*)value.c_str(),
                           (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(icons);
    }
    RegCloseKey(key);
}

bool Settings::IsDeviceHidden(const std::wstring& id) const {
    return std::find(hiddenDevices.begin(), hiddenDevices.end(), id) != hiddenDevices.end();
}

void Settings::SetDeviceHidden(const std::wstring& id, bool hidden) {
    auto it = std::find(hiddenDevices.begin(), hiddenDevices.end(), id);
    if (hidden && it == hiddenDevices.end())
        hiddenDevices.push_back(id);
    else if (!hidden && it != hiddenDevices.end())
        hiddenDevices.erase(it);
}

std::wstring AppDataDir(const wchar_t* sub) {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) dir = base;
    CoTaskMemFree(base);
    dir += L"\\" APP_NAME;
    CreateDirectoryW(dir.c_str(), nullptr);
    if (sub) {
        dir += L"\\";
        dir += sub;
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

static std::wstring RunCommand() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return L"\"" + std::wstring(path) + L"\" /background";
}

bool GetStartWithWindows() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, APP_NAME, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    return _wcsicmp(buf, RunCommand().c_str()) == 0;
}

void SetStartWithWindows(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (enable) {
        std::wstring cmd = RunCommand();
        RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, APP_NAME);
    }
    RegCloseKey(key);
}
