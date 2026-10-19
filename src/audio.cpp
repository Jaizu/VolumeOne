#include "audio.h"
#include "strings.h"

#include <propvarutil.h>
#include <atomic>
#include <cmath>
#include <unordered_map>

static const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
// The endpoint's own name, the one you can change in Sound settings ("Speakers").
static const PROPERTYKEY kDeviceDescription = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
// The audio adapter's name, from its driver ("Realtek(R) Audio").
static const PROPERTYKEY kInterfaceFriendlyName = {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};
static const PROPERTYKEY kFormFactor = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};

// Identifies volume changes we made ourselves.
static const GUID kEventContext = {0x5b1e7a52, 0x3c1d, 0x4b9e, {0x9a, 0x0e, 0x51, 0x7c, 0x0d, 0x4f, 0x22, 0x11}};

// ---------------------------------------------------------------------------
// One COM object receives every Core Audio notification and turns it into a
// posted window message, so all real work happens on the UI thread.
class AudioEvents final : public IMMNotificationClient,
                          public IAudioEndpointVolumeCallback,
                          public IAudioSessionNotification,
                          public IAudioSessionEvents {
public:
    explicit AudioEvents(HWND hwnd) : hwnd_(hwnd) {}

    void Detach() { hwnd_ = nullptr; }
    void VolumeHandled() { volumePending_ = false; }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
            *ppv = static_cast<IMMNotificationClient*>(this);
        else if (riid == __uuidof(IAudioEndpointVolumeCallback))
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
        else if (riid == __uuidof(IAudioSessionNotification))
            *ppv = static_cast<IAudioSessionNotification*>(this);
        else if (riid == __uuidof(IAudioSessionEvents))
            *ppv = static_cast<IAudioSessionEvents*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // IMMNotificationClient
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return Post(WM_APP_DEVICES_CHANGED); }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return Post(WM_APP_DEVICES_CHANGED); }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return Post(WM_APP_DEVICES_CHANGED); }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if ((flow == eRender && role == eMultimedia) || (flow == eCapture && role == eConsole))
            Post(WM_APP_DEVICES_CHANGED);
        return S_OK;
    }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
        // Renaming in Sound settings changes the description; the friendly name is derived from it.
        if (key == kFriendlyName || key == kDeviceDescription || key == kInterfaceFriendlyName)
            Post(WM_APP_DEVICES_CHANGED);
        return S_OK;
    }

    // IAudioEndpointVolumeCallback
    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override { return PostVolume(); }

    // IAudioSessionNotification
    STDMETHODIMP OnSessionCreated(IAudioSessionControl*) override { return Post(WM_APP_SESSIONS_CHANGED); }

    // IAudioSessionEvents
    STDMETHODIMP OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return PostVolume(); }
    STDMETHODIMP OnChannelVolumeChanged(DWORD, float*, DWORD, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnStateChanged(AudioSessionState state) override {
        if (state == AudioSessionStateExpired) Post(WM_APP_SESSIONS_CHANGED);
        return S_OK;
    }
    STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override { return Post(WM_APP_SESSIONS_CHANGED); }

private:
    HRESULT Post(UINT msg) {
        if (HWND h = hwnd_) PostMessageW(h, msg, 0, 0);
        return S_OK;
    }
    // Coalesce bursts (another app dragging a slider) into one message.
    HRESULT PostVolume() {
        if (!volumePending_.exchange(true)) Post(WM_APP_VOLUME_CHANGED);
        return S_OK;
    }

    std::atomic<HWND> hwnd_;
    std::atomic<ULONG> refs_{1};
    std::atomic<bool> volumePending_{false};
};

// ---------------------------------------------------------------------------
// IPolicyConfig: undocumented but stable since Windows 7; it's what the Sound
// control panel (and EarTrumpet, SoundSwitch...) use to change the default
// device. Only SetDefaultEndpoint is called; the rest just fill the vtable.

static const CLSID kPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

// ---------------------------------------------------------------------------
// Helpers

static std::wstring GetExePath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD size = ARRAYSIZE(buf);
    std::wstring result;
    DWORD exitCode = 0;
    if (GetExitCodeProcess(h, &exitCode) && exitCode == STILL_ACTIVE && QueryFullProcessImageNameW(h, 0, buf, &size))
        result.assign(buf, size);
    CloseHandle(h);
    return result;
}

static bool ProcessIsDead(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError() == ERROR_INVALID_PARAMETER;
    DWORD exitCode = 0;
    bool dead = GetExitCodeProcess(h, &exitCode) && exitCode != STILL_ACTIVE;
    CloseHandle(h);
    return dead;
}

static std::wstring GetFileDescription(const std::wstring& path) {
    static std::unordered_map<std::wstring, std::wstring> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;

    std::wstring desc;
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
            struct Lang { WORD lang, codepage; }* langs = nullptr;
            UINT len = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &len) && len >= sizeof(Lang)) {
                wchar_t sub[64];
                swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].codepage);
                wchar_t* value = nullptr;
                if (VerQueryValueW(data.data(), sub, (void**)&value, &len) && value && len > 1) desc = value;
            }
        }
    }
    while (!desc.empty() && iswspace(desc.back())) desc.pop_back();
    if (desc.empty()) {
        desc = PathFindFileNameW(path.c_str());
        size_t dot = desc.rfind(L'.');
        if (dot != std::wstring::npos) desc.resize(dot);
        if (!desc.empty()) desc[0] = towupper(desc[0]);
    }
    cache[path] = desc;
    return desc;
}

static std::wstring ResolveIndirect(const wchar_t* s) {
    if (!s || !*s) return {};
    if (s[0] != L'@') return s;
    wchar_t buf[512];
    if (SUCCEEDED(SHLoadIndirectString(s, buf, ARRAYSIZE(buf), nullptr))) return buf;
    return {};
}

static std::wstring ToLower(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

// ---------------------------------------------------------------------------

bool AudioManager::Init(HWND notifyWindow) {
    hwnd_ = notifyWindow;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&enumerator_))))
        return false;
    events_ = new AudioEvents(hwnd_);
    enumerator_->RegisterEndpointNotificationCallback(events_);
    RefreshDevices();
    return true;
}

void AudioManager::Shutdown() {
    ClearDevices();
    if (enumerator_ && events_) enumerator_->UnregisterEndpointNotificationCallback(events_);
    if (events_) {
        events_->Detach();
        events_->Release();
        events_ = nullptr;
    }
    enumerator_.Reset();
}

void AudioManager::ClearSessions() {
    for (auto& d : devices_) {
        for (auto& a : d.apps)
            for (auto& c : a.controls) c->UnregisterAudioSessionNotification(events_);
        d.apps.clear();
        if (d.sessionManager) {
            d.sessionManager->UnregisterSessionNotification(events_);
            d.sessionManager.Reset();
        }
    }
}

void AudioManager::ClearDevices() {
    ClearSessions();
    for (auto& d : devices_)
        if (d.endpoint) d.endpoint->UnregisterControlChangeNotify(events_);
    devices_.clear();
}

void AudioManager::RefreshDevices() {
    ClearDevices();
    if (!enumerator_) return;

    std::wstring defaultId;
    ComPtr<IMMDevice> def;
    if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, &def))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) return;
    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        AudioDevice d;
        if (FAILED(collection->Item(i, &d.device))) continue;
        LPWSTR id = nullptr;
        if (FAILED(d.device->GetId(&id))) continue;
        d.id = id;
        CoTaskMemFree(id);
        d.isDefault = d.id == defaultId;

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(d.device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kDeviceDescription, &v)) && v.vt == VT_LPWSTR) d.name = v.pwszVal;
            PropVariantClear(&v);
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kInterfaceFriendlyName, &v)) && v.vt == VT_LPWSTR)
                d.technicalName = v.pwszVal;
            PropVariantClear(&v);
            if (d.name.empty()) {
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) d.name = v.pwszVal;
                PropVariantClear(&v);
            }
            if (SUCCEEDED(props->GetValue(kFormFactor, &v)) && v.vt == VT_UI4) {
                switch (v.ulVal) {
                    case 1: case 2: case 8: case 9: d.formFactor = FormFactor::Speakers; break;
                    case 3: case 5: case 6: d.formFactor = FormFactor::Headphones; break;
                    default: d.formFactor = FormFactor::Other; break;
                }
            }
            PropVariantClear(&v);
        }

        if (SUCCEEDED(d.device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                                         (void**)d.endpoint.GetAddressOf()))) {
            d.endpoint->RegisterControlChangeNotify(events_);
        }
        d.device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_INPROC_SERVER, nullptr,
                           (void**)d.meter.GetAddressOf());
        devices_.push_back(std::move(d));
    }

    std::stable_sort(devices_.begin(), devices_.end(), [](const AudioDevice& a, const AudioDevice& b) {
        if (a.isDefault != b.isDefault) return b.isDefault;  // current output last, next to the taskbar
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    if (trackSessions_)
        for (auto& d : devices_) LoadSessions(d);
    LoadInputs();
    RefreshVolumes();
}

static std::wstring DevicePropertyName(IMMDevice* device, const PROPERTYKEY& key) {
    std::wstring name;
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(key, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
        PropVariantClear(&v);
    }
    return name;
}

void AudioManager::LoadInputs() {
    inputs_.clear();
    std::wstring defaultId;
    ComPtr<IMMDevice> def;
    if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eCapture, eConsole, &def))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator_->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) return;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        LPWSTR id = nullptr;
        if (FAILED(collection->Item(i, &device)) || FAILED(device->GetId(&id))) continue;
        InputDevice in;
        in.id = id;
        CoTaskMemFree(id);
        in.name = DevicePropertyName(device.Get(), kDeviceDescription);
        in.technicalName = DevicePropertyName(device.Get(), kInterfaceFriendlyName);
        if (in.name.empty()) in.name = DevicePropertyName(device.Get(), kFriendlyName);
        in.isDefault = in.id == defaultId;
        inputs_.push_back(std::move(in));
    }
    std::sort(inputs_.begin(), inputs_.end(),
              [](const InputDevice& a, const InputDevice& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
}

bool AudioManager::SetDefaultDevice(const std::wstring& id) {
    ComPtr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), (void**)&policy)))
        return false;
    bool ok = true;
    for (ERole role : {eConsole, eMultimedia, eCommunications})
        ok &= SUCCEEDED(policy->SetDefaultEndpoint(id.c_str(), role));
    return ok;
}

void AudioManager::SetTrackSessions(bool track) {
    if (track == trackSessions_) return;
    trackSessions_ = track;
    if (track)
        RefreshSessions();
    else
        ClearSessions();
}

void AudioManager::RefreshSessions() {
    ClearSessions();
    if (!trackSessions_) return;
    for (auto& d : devices_) LoadSessions(d);
    RefreshVolumes();
}

void AudioManager::LoadSessions(AudioDevice& d) {
    d.playing = false;
    if (FAILED(d.device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr,
                                  (void**)d.sessionManager.ReleaseAndGetAddressOf())))
        return;
    d.sessionManager->RegisterSessionNotification(events_);

    // Enumerating is also what makes session notifications start flowing.
    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(d.sessionManager->GetSessionEnumerator(&sessions))) return;
    int count = 0;
    sessions->GetCount(&count);

    for (int i = 0; i < count; i++) {
        ComPtr<IAudioSessionControl> ctl;
        if (FAILED(sessions->GetSession(i, &ctl))) continue;
        ComPtr<IAudioSessionControl2> ctl2;
        if (FAILED(ctl.As(&ctl2))) continue;

        AudioSessionState state;
        if (FAILED(ctl->GetState(&state)) || state == AudioSessionStateExpired) continue;
        if (state == AudioSessionStateActive) d.playing = true;

        bool isSystem = ctl2->IsSystemSoundsSession() == S_OK;
        DWORD pid = 0;
        ctl2->GetProcessId(&pid);
        if (!isSystem && ProcessIsDead(pid)) continue;

        std::wstring exe = isSystem ? std::wstring() : GetExePath(pid);
        std::wstring key = isSystem ? L"system" : (!exe.empty() ? ToLower(exe) : L"pid:" + std::to_wstring(pid));

        ComPtr<ISimpleAudioVolume> vol;
        if (FAILED(ctl.As(&vol))) continue;

        auto it = std::find_if(d.apps.begin(), d.apps.end(), [&](const AppEntry& a) { return a.key == key; });
        if (it == d.apps.end()) {
            AppEntry a;
            a.key = key;
            a.isSystem = isSystem;
            if (isSystem) {
                a.name = S().systemSounds;
            } else {
                LPWSTR dn = nullptr;
                if (SUCCEEDED(ctl->GetDisplayName(&dn)) && dn) {
                    a.name = ResolveIndirect(dn);
                    CoTaskMemFree(dn);
                }
                if (a.name.empty()) a.name = exe.empty() ? L"PID " + std::to_wstring(pid) : GetFileDescription(exe);
                LPWSTR ip = nullptr;
                if (SUCCEEDED(ctl->GetIconPath(&ip)) && ip) {
                    if (*ip && ip[0] != L'@') a.iconPath = ip;
                    CoTaskMemFree(ip);
                }
                if (a.iconPath.empty()) a.iconPath = exe;
            }
            d.apps.push_back(std::move(a));
            it = d.apps.end() - 1;
        }
        ctl->RegisterAudioSessionNotification(events_);
        it->controls.push_back(ctl);
        it->volumes.push_back(vol);
        ComPtr<IAudioMeterInformation> meter;
        if (SUCCEEDED(ctl.As(&meter))) it->meters.push_back(meter);
    }

    std::stable_sort(d.apps.begin(), d.apps.end(), [](const AppEntry& a, const AppEntry& b) {
        if (a.isSystem != b.isSystem) return !a.isSystem;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

void AudioManager::RefreshVolumes() {
    if (events_) events_->VolumeHandled();
    for (auto& d : devices_) {
        if (d.endpoint) {
            d.endpoint->GetMasterVolumeLevelScalar(&d.volume);
            BOOL m = FALSE;
            d.endpoint->GetMute(&m);
            d.muted = m != FALSE;
        }
        for (auto& a : d.apps) {
            if (a.volumes.empty()) continue;
            a.volumes[0]->GetMasterVolume(&a.volume);
            BOOL m = FALSE;
            a.volumes[0]->GetMute(&m);
            a.muted = m != FALSE;
        }
    }
}

// Meters report the signal before the volume is applied; scale by the volume
// like EarTrumpet so the bar sits inside the filled part of the slider. Rises
// instantly, falls smoothly.
static bool Smooth(float& shown, float target) {
    float next = target >= shown ? target : std::max(target, shown - 0.04f);
    bool changed = std::abs(next - shown) > 0.002f;
    shown = next;
    return changed;
}

bool AudioManager::UpdatePeaks() {
    bool changed = false;
    for (auto& d : devices_) {
        float p = 0;
        if (d.meter) d.meter->GetPeakValue(&p);
        changed |= Smooth(d.peak, d.muted ? 0.0f : p * d.volume);
        for (auto& a : d.apps) {
            float ap = 0;
            for (auto& m : a.meters) {
                float v = 0;
                m->GetPeakValue(&v);
                ap = std::max(ap, v);
            }
            changed |= Smooth(a.peak, (a.muted || d.muted) ? 0.0f : ap * a.volume);
        }
    }
    return changed;
}

AudioDevice* AudioManager::DefaultDevice() {
    for (auto& d : devices_)
        if (d.isDefault) return &d;
    return nullptr;
}

void AudioManager::SetDeviceVolume(AudioDevice& d, float v) {
    d.volume = v;
    if (!d.endpoint) return;
    d.endpoint->SetMasterVolumeLevelScalar(v, &kEventContext);
    if (d.muted && v > 0.0f) SetDeviceMute(d, false);
}

void AudioManager::SetDeviceMute(AudioDevice& d, bool mute) {
    d.muted = mute;
    if (d.endpoint) d.endpoint->SetMute(mute, &kEventContext);
}

void AudioManager::SetAppVolume(AppEntry& a, float v) {
    a.volume = v;
    for (auto& vol : a.volumes) vol->SetMasterVolume(v, &kEventContext);
    if (a.muted && v > 0.0f) SetAppMute(a, false);
}

void AudioManager::SetAppMute(AppEntry& a, bool mute) {
    a.muted = mute;
    for (auto& vol : a.volumes) vol->SetMute(mute, &kEventContext);
}
