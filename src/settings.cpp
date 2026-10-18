#include "settings.h"

static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

void Settings::Load() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, APP_REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;

    DWORD value = 0, size = sizeof(value);
    if (RegQueryValueExW(key, REG_HIDE_ICON, nullptr, nullptr, (BYTE*)&value, &size) == ERROR_SUCCESS)
        hideSystemVolumeIcon = value != 0;

    hiddenDevices.clear();
    size = 0;
    if (RegQueryValueExW(key, L"HiddenDevices", nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS && size > 0) {
        std::wstring buf(size / sizeof(wchar_t) + 2, L'\0');
        if (RegQueryValueExW(key, L"HiddenDevices", nullptr, nullptr, (BYTE*)buf.data(), &size) == ERROR_SUCCESS) {
            for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1)
                hiddenDevices.emplace_back(p);
        }
    }
    RegCloseKey(key);
}

void Settings::Save() const {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    DWORD value = hideSystemVolumeIcon ? 1 : 0;
    RegSetValueExW(key, REG_HIDE_ICON, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));

    std::wstring multi;
    for (const auto& id : hiddenDevices) {
        multi += id;
        multi.push_back(L'\0');
    }
    multi.push_back(L'\0');
    RegSetValueExW(key, L"HiddenDevices", 0, REG_MULTI_SZ, (const BYTE*)multi.data(),
                   (DWORD)(multi.size() * sizeof(wchar_t)));
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
