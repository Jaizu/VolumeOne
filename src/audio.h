#pragma once

#include "common.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>

// One entry in the mixer: all audio sessions of the same executable on a
// device, grouped like the Windows mixer and EarTrumpet do.
struct AppEntry {
    std::wstring key;       // lower-case exe path, "system" or "pid:N"
    std::wstring name;
    std::wstring iconPath;  // exe (or session icon) to extract the icon from
    bool isSystem = false;
    float volume = 1.0f;
    bool muted = false;
    std::vector<ComPtr<ISimpleAudioVolume>> volumes;
    std::vector<ComPtr<IAudioSessionControl>> controls;
};

enum class FormFactor { Speakers, Headphones, Other };

struct AudioDevice {
    std::wstring id;
    std::wstring name;
    FormFactor formFactor = FormFactor::Speakers;
    bool isDefault = false;
    float volume = 0.0f;
    bool muted = false;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> endpoint;
    ComPtr<IAudioSessionManager2> sessionManager;
    std::vector<AppEntry> apps;
};

class AudioEvents;

class AudioManager {
public:
    bool Init(HWND notifyWindow);
    void Shutdown();

    // Rebuilds the device list (and sessions, if they are being tracked).
    void RefreshDevices();
    // Re-reads volume/mute values of everything we track. Cheap.
    void RefreshVolumes();
    // Session tracking is only enabled while the flyout is open.
    void SetTrackSessions(bool track);
    void RefreshSessions();

    std::vector<AudioDevice>& Devices() { return devices_; }
    AudioDevice* DefaultDevice();

    void SetDeviceVolume(AudioDevice& d, float v);
    void SetDeviceMute(AudioDevice& d, bool mute);
    void SetAppVolume(AppEntry& a, float v);
    void SetAppMute(AppEntry& a, bool mute);

private:
    void ClearDevices();
    void ClearSessions();
    void LoadSessions(AudioDevice& d);

    HWND hwnd_ = nullptr;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    AudioEvents* events_ = nullptr;
    std::vector<AudioDevice> devices_;
    bool trackSessions_ = false;
};
