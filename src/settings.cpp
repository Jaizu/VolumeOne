#include "settings.h"
#include "log.h"

#include <shlobj.h>

static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static std::wstring SettingsPath() { return AppDataDir() + L"\\" SETTINGS_FILE; }

// Before 1.6.0 settings lived in HKCU\Software\VolumeOne. Returns false if
// that key doesn't exist.
static bool LoadLegacy(Settings& s) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, APP_LEGACY_REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    DWORD value = 0, size = sizeof(value);
    if (RegQueryValueExW(key, SETTING_HIDE_ICON, nullptr, nullptr, (BYTE*)&value, &size) == ERROR_SUCCESS)
        s.hideSystemVolumeIcon = value != 0;
    size = sizeof(value);
    if (RegQueryValueExW(key, SETTING_USE_SYSTEM_ICON, nullptr, nullptr, (BYTE*)&value, &size) == ERROR_SUCCESS)
        s.useSystemVolumeIcon = value != 0;
    size = sizeof(value);
    if (RegQueryValueExW(key, SETTING_HIDE_QS_VOLUME, nullptr, nullptr, (BYTE*)&value, &size) == ERROR_SUCCESS)
        s.hideQuickSettingsVolume = value != 0;
    size = sizeof(value);
    if (RegQueryValueExW(key, L"MeterColor", nullptr, nullptr, (BYTE*)&value, &size) == ERROR_SUCCESS &&
        value <= Settings::MeterCustom)
        s.meterColor = value;
    size = sizeof(value);
    if (RegQueryValueExW(key, L"MeterCustomColor", nullptr, nullptr, (BYTE*)&value, &size) == ERROR_SUCCESS)
        s.meterCustomColor = value & 0xFFFFFF;
    size = sizeof(value);
    if (RegQueryValueExW(key, L"MeterCustomBar", nullptr, nullptr, (BYTE*)&value, &size) == ERROR_SUCCESS)
        s.meterCustomBar = value & 0xFFFFFF;
    size = sizeof(value);
    if (RegQueryValueExW(key, L"DeviceNameMode", nullptr, nullptr, (BYTE*)&value, &size) == ERROR_SUCCESS &&
        value <= Settings::NameTechnical)
        s.deviceNameMode = value;

    size = 0;
    if (RegQueryValueExW(key, L"HiddenDevices", nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS && size > 0) {
        std::wstring buf(size / sizeof(wchar_t) + 2, L'\0');
        if (RegQueryValueExW(key, L"HiddenDevices", nullptr, nullptr, (BYTE*)buf.data(), &size) == ERROR_SUCCESS) {
            for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1)
                s.hiddenDevices.emplace_back(p);
        }
    }

    HKEY icons;
    if (RegOpenKeyExW(key, L"DeviceIcons", 0, KEY_READ, &icons) == ERROR_SUCCESS) {
        wchar_t name[512], data[MAX_PATH * 2];
        for (DWORD i = 0;; i++) {
            DWORD nameLen = ARRAYSIZE(name), dataLen = sizeof(data), type = 0;
            if (RegEnumValueW(icons, i, name, &nameLen, nullptr, &type, (BYTE*)data, &dataLen) != ERROR_SUCCESS) break;
            if (type == REG_SZ) s.deviceIcons[name] = data;
        }
        RegCloseKey(icons);
    }
    RegCloseKey(key);
    return true;
}

// The "key=value" lines of an ini section.
static std::vector<std::pair<std::wstring, std::wstring>> ReadSection(const std::wstring& path, const wchar_t* section) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    std::wstring buf(32768, L'\0');
    DWORD len = GetPrivateProfileSectionW(section, buf.data(), (DWORD)buf.size(), path.c_str());
    for (const wchar_t* p = buf.c_str(); p < buf.c_str() + len && *p; p += wcslen(p) + 1) {
        const wchar_t* eq = wcschr(p, L'=');
        if (!eq || eq == p) continue;
        out.emplace_back(std::wstring(p, eq), std::wstring(eq + 1));
    }
    return out;
}

static std::wstring ReadString(const std::wstring& path, const wchar_t* name) {
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(SETTINGS_SECTION, name, L"", buf, ARRAYSIZE(buf), path.c_str());
    return buf;
}

static void ReadBool(const std::wstring& path, const wchar_t* name, bool& out) {
    std::wstring v = ReadString(path, name);
    if (!v.empty()) out = _wtoi(v.c_str()) != 0 || _wcsicmp(v.c_str(), L"true") == 0;
}

// "#RRGGBB" (the # is optional).
static void ReadColor(const std::wstring& path, const wchar_t* name, DWORD& out) {
    std::wstring v = ReadString(path, name);
    const wchar_t* p = v.c_str() + (v[0] == L'#');
    wchar_t* end = nullptr;
    unsigned long c = wcstoul(p, &end, 16);
    if (end != p && !*end) out = c & 0xFFFFFF;
}

void Settings::Load() {
    std::wstring path = SettingsPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // First run of 1.6.0 or later: move the old registry settings to the file.
        // The file is written either way: the dll reads it and a missing key is
        // 0 there, which isn't the default for every setting.
        bool legacy = LoadLegacy(*this);
        Save();
        if (legacy) {
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                RegDeleteTreeW(HKEY_CURRENT_USER, APP_LEGACY_REG_KEY);
                RegDeleteKeyW(HKEY_CURRENT_USER, APP_LEGACY_REG_KEY);
                LOG(L"settings: migrated from the registry to %s", path.c_str());
            }
        }
        return;
    }

    ReadBool(path, SETTING_HIDE_ICON, hideSystemVolumeIcon);
    ReadBool(path, SETTING_USE_SYSTEM_ICON, useSystemVolumeIcon);
    ReadBool(path, SETTING_HIDE_QS_VOLUME, hideQuickSettingsVolume);
    DWORD value = (DWORD)GetPrivateProfileIntW(SETTINGS_SECTION, L"MeterColor", meterColor, path.c_str());
    if (value <= MeterCustom) meterColor = value;
    ReadColor(path, L"MeterCustomColor", meterCustomColor);
    ReadColor(path, L"MeterCustomBar", meterCustomBar);
    value = (DWORD)GetPrivateProfileIntW(SETTINGS_SECTION, L"DeviceNameMode", deviceNameMode, path.c_str());
    if (value <= NameTechnical) deviceNameMode = value;
    ReadBool(path, L"NyanCat", nyanCat);

    hiddenDevices.clear();
    for (const auto& [name, id] : ReadSection(path, L"HiddenDevices"))
        if (!id.empty()) hiddenDevices.push_back(id);
    deviceIcons.clear();
    for (const auto& [id, icon] : ReadSection(path, L"DeviceIcons"))
        if (!icon.empty()) deviceIcons[id] = icon;
}

void Settings::Save() const {
    auto color = [](DWORD c) {
        wchar_t buf[16];
        swprintf_s(buf, L"#%06X", c & 0xFFFFFF);
        return std::wstring(buf);
    };
    // UTF-16 with a BOM, so the ini functions keep non-ASCII paths intact.
    std::wstring text = L"﻿"
                        L"; VolumeOne settings. Close VolumeOne before editing: it rewrites this file.\r\n"
                        L"; MeterColor: 0 accent, 1 white, 2 green, 3 custom (MeterCustomColor, MeterCustomBar)\r\n"
                        L"; DeviceNameMode: 0 both, 1 name in Windows, 2 hardware name\r\n"
                        L"[" SETTINGS_SECTION L"]\r\n";
    auto line = [&](const std::wstring& name, const std::wstring& value) { text += name + L"=" + value + L"\r\n"; };
    line(SETTING_HIDE_ICON, hideSystemVolumeIcon ? L"1" : L"0");
    line(SETTING_USE_SYSTEM_ICON, useSystemVolumeIcon ? L"1" : L"0");
    line(SETTING_HIDE_QS_VOLUME, hideQuickSettingsVolume ? L"1" : L"0");
    line(L"MeterColor", std::to_wstring(meterColor));
    line(L"MeterCustomColor", color(meterCustomColor));
    line(L"MeterCustomBar", color(meterCustomBar));
    line(L"DeviceNameMode", std::to_wstring(deviceNameMode));
    line(L"NyanCat", nyanCat ? L"1" : L"0");
    text += L"\r\n[HiddenDevices]\r\n";
    for (size_t i = 0; i < hiddenDevices.size(); i++) line(L"Device" + std::to_wstring(i + 1), hiddenDevices[i]);
    text += L"\r\n; speakers, headphones or the path of an image\r\n[DeviceIcons]\r\n";
    for (const auto& [id, icon] : deviceIcons) line(id, icon);

    // Written next to it and swapped in, so the dll never reads half a file.
    std::wstring path = SettingsPath(), tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD bytes = (DWORD)(text.size() * sizeof(wchar_t)), written = 0;
    BOOL ok = WriteFile(f, text.data(), bytes, &written, nullptr) && written == bytes;
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

static bool ContainsCaseInsensitive(const std::wstring& text, const std::wstring& value) {
    if (value.empty() || value.size() > text.size()) return false;
    for (size_t i = 0; i <= text.size() - value.size(); i++)
        if (_wcsnicmp(text.c_str() + i, value.c_str(), value.size()) == 0) return true;
    return false;
}

std::wstring Settings::DisplayDeviceName(const std::wstring& nickname, const std::wstring& technical) const {
    if (deviceNameMode == NameNickname) return nickname.empty() ? technical : nickname;
    if (deviceNameMode == NameTechnical) return technical.empty() ? nickname : technical;
    if (nickname.empty()) return technical;
    if (technical.empty() || ContainsCaseInsensitive(nickname, technical)) return nickname;
    return nickname + L" (" + technical + L")";
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

static std::wstring RunCommand(const wchar_t* exe = nullptr) {
    wchar_t path[MAX_PATH];
    if (!exe) {
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        exe = path;
    }
    return L"\"" + std::wstring(exe) + L"\" /background";
}

bool GetStartWithWindows() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, APP_NAME, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    return _wcsicmp(buf, RunCommand().c_str()) == 0;
}

void SetStartWithWindows(bool enable, const wchar_t* exePath) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (enable) {
        std::wstring cmd = RunCommand(exePath);
        RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, APP_NAME);
    }
    RegCloseKey(key);
}
