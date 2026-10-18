#pragma once

#include "common.h"
#include "audio.h"
#include "settings.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <functional>
#include <map>

struct Palette {
    bool dark = true;
    D2D1_COLOR_F text, textSecondary, textTertiary;
    D2D1_COLOR_F subtleHover, subtlePressed;
    D2D1_COLOR_F card, cardStroke, divider, footer;
    D2D1_COLOR_F trackOff, thumb, thumbStroke;
    D2D1_COLOR_F accent, onAccent;
    D2D1_COLOR_F toggleOffFill;
    D2D1_COLOR_F tint;  // painted over the acrylic, like WinUI's acrylic tint layer
};

// Owned by the host; the flyout shows it and asks for checks/installs.
struct UpdateState {
    enum Status { Idle, Checking, UpToDate, Available, Failed, Installing, InstallFailed };
    Status status = Idle;
    std::wstring version;  // when Available
};

class Flyout {
public:
    std::function<void()> onHideIconChanged;
    std::function<void()> onExit;
    std::function<void()> onCheckUpdates;
    std::function<void()> onInstallUpdate;

    bool Create(HINSTANCE inst, HWND owner, AudioManager* audio, Settings* settings, const UpdateState* update);
    void Toggle();
    void Show(bool settingsView = false);
    void Hide();
    bool IsVisible() const { return visible_; }
    HWND Hwnd() const { return hwnd_; }

    // Called by the host when Core Audio reports changes.
    void OnAudioStructureChanged();
    void OnVolumeChanged();
    void OnThemeChanged();
    void OnUpdateStateChanged();

private:
    enum class View { Mixer, Settings };
    enum class HitKind {
        None, Mute, Slider, Collapse, Gear, Back, SoundSettings, ToggleHideIcon, ToggleStartup,
        ToggleDevice, ToggleInput, IconPick, CheckUpdates, InstallUpdate, Exit
    };
    struct Hit {
        HitKind kind = HitKind::None;
        int device = -1;
        int app = -1;  // -1 = device master volume
        D2D1_RECT_F rect{};
        float trackX0 = 0, trackX1 = 0;  // sliders only
        bool operator==(const Hit& o) const { return kind == o.kind && device == o.device && app == o.app; }
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT, WPARAM, LPARAM);

    void ApplyWindowStyle();
    void LoadPalette();
    bool EnsureResources();
    void DiscardTarget();
    void Render();
    float Layout(bool draw);
    float LayoutMixer(float y, bool draw);
    float LayoutSettings(float y, bool draw);
    void LayoutFooter(float top, bool draw);
    void Reposition(bool animate);
    std::vector<int> VisibleDevices();
    bool IsExpanded(const AudioDevice& d) const;
    void PickIcon(int device);

    // Drawing helpers (all coordinates in DIPs).
    void Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius = 0);
    void Stroke(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius, float width = 1);
    void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    float TextHeight(const std::wstring& s, IDWriteTextFormat* f, float width);
    float TextWidth(const std::wstring& s, IDWriteTextFormat* f);
    void Glyph(wchar_t g, float size, const D2D1_RECT_F& r, const D2D1_COLOR_F& c);
    void Button(const Hit& h, bool draw, float radius = 4);
    void Slider(const Hit& h, float cy, float value, bool muted, bool draw);
    void ToggleSwitch(const Hit& h, float x, float cy, bool on, bool draw);
    void AppIcon(const AppEntry& a, const D2D1_RECT_F& r, float opacity);
    void DeviceIcon(const AudioDevice& d, const D2D1_RECT_F& r, float size, const D2D1_COLOR_F& c);
    ComPtr<ID2D1Bitmap> LoadIconBitmap(const std::wstring& path, int px);
    ComPtr<ID2D1Bitmap> LoadImageBitmap(const std::wstring& path, int px);
    ComPtr<ID2D1Bitmap> WicToD2D(IWICBitmapSource* src);

    Hit HitTest(float x, float y) const;
    void Activate(const Hit& h);
    void SetSliderValue(const Hit& h, float x);
    void AdjustSlider(const Hit& h, float delta);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    int slideDx_ = 0, slideDy_ = 1;  // direction the flyout slides in from
    AudioManager* audio_ = nullptr;
    Settings* settings_ = nullptr;
    const UpdateState* update_ = nullptr;
    bool visible_ = false;
    bool modal_ = false;  // a menu or file dialog is open: don't hide on deactivate
    std::map<std::wstring, bool> expanded_;  // user's collapse/expand choices, by device id
    ULONGLONG lastHideTick_ = 0;
    View view_ = View::Mixer;
    bool startup_ = false;

    float scale_ = 1.0f;
    float winH_ = 100000.0f;  // window height in DIPs
    float contentBottom_ = 0;
    float scroll_ = 0, maxScroll_ = 0;

    std::vector<Hit> hits_;
    Hit hover_, pressed_;
    bool dragging_ = false;
    bool tracking_ = false;

    Palette pal_;
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> fTitle_, fBody_, fCaption_, fDesc_;
    std::map<int, ComPtr<IDWriteTextFormat>> iconFormats_;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> icons_;
};
