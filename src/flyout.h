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
    D2D1_COLOR_F text, textSecondary, textTertiary, textDisabled;
    D2D1_COLOR_F meter;
    D2D1_COLOR_F subtleHover, subtlePressed;
    D2D1_COLOR_F card, cardStroke, divider, footer;
    D2D1_COLOR_F trackOff, thumb, thumbStroke;
    D2D1_COLOR_F accent, onAccent;
    D2D1_COLOR_F toggleOffFill;
    D2D1_COLOR_F tint;  // painted over the acrylic, like WinUI's acrylic tint layer
    D2D1_COLOR_F border;  // panel outline
};

// Owned by the host; the flyout shows it and asks for checks/installs.
struct UpdateState {
    enum Status { Idle, Checking, UpToDate, Available, Failed, Installing, InstallFailed };
    Status status = Idle;
    std::wstring version;  // when Available
    std::wstring notes;    // release notes, plain text
};

// Milliseconds from the high-resolution counter (GetTickCount64 moves in
// ~15.6 ms steps, which makes animations visibly step).
inline double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / freq;
}

// A value eased from one number to another over time (ease-out cubic).
struct Anim {
    float from = 0, to = 0, ms = 1;
    double start = 0;
    float Progress() const { return std::min(1.0f, (float)((NowMs() - start) / ms)); }
    bool Running() const { return Progress() < 1.0f; }
    float Value() const {
        float t = Progress();
        return from + (to - from) * (1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t));
    }
    void Go(float target, float duration) {
        from = Value();
        to = target;
        ms = duration;
        start = NowMs();
    }
    void Set(float v) { from = to = v; start = 0; }
};

class Flyout {
public:
    std::function<void()> onHideIconChanged;   // also called when "use the Windows icon" changes
    std::function<void()> onCheckUpdates;
    std::function<void()> onInstallUpdate;
    std::function<void()> onCloseAndRestore;
    std::function<void()> onMoveToDocuments;
    std::function<void()> onOpenLogs;
    std::wstring locationName;  // folder the exe is in, if it's a risky one (Downloads...); shows a warning

    bool Create(HINSTANCE inst, HWND owner, AudioManager* audio, Settings* settings, const UpdateState* update);
    void Toggle();
    void Show(bool settingsView = false);
    void Hide();
    bool IsVisible() const { return visible_; }
    HWND Hwnd() const { return hwnd_; }
    RECT PanelRect() const;  // the visible panel, screen pixels (the window is bigger)

    // Called by the host when Core Audio reports changes.
    void OnAudioStructureChanged();
    void OnVolumeChanged();
    void OnThemeChanged();
    void OnUpdateStateChanged();

    // Small pill above the taskbar with the device's volume, shown while the
    // volume is changed with the mouse wheel over the taskbar icon.
    void ShowVolumeOsd(const AudioDevice& d);

private:
    enum class View { Mixer, Settings };
    enum class HitKind {
        None, Mute, Slider, Collapse, DeviceMenu, SelectDevice, Gear, Back, SoundSettings, ToggleUseSystemIcon, ToggleHideIcon, ToggleHideQs, ToggleStartup,
        ToggleDevice, ToggleInput, IconPick, CheckUpdates, InstallUpdate, Section, MeterOption, NameOption,
        CloseRestore, MoveToDocuments, OpenLogs, ColorSwatch, ColorSV, ColorHue, ToggleNyan, Info
    };
    struct Hit {
        HitKind kind = HitKind::None;
        int device = -1;
        int app = -1;  // -1 = device master volume
        D2D1_RECT_F rect{};
        float trackX0 = 0, trackX1 = 0;  // sliders only
        bool footer = false;              // in the fixed footer, not the scrolling content
        bool operator==(const Hit& o) const { return kind == o.kind && device == o.device && app == o.app; }
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK OsdProc(HWND, UINT, WPARAM, LPARAM);
    void RenderOsd();

    // Settings gear drawn as a path (the Quick Settings one isn't the font
    // glyph). It turns when clicked, then the view changes with a short
    // transition; the back arrow turns from left to down the same way.
    void DrawGear(const D2D1_RECT_F& r, const D2D1_COLOR_F& c);
    void DrawRotatedGlyph(wchar_t g, float size, const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float degrees);
    void DrawChevron(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float open);
    ID2D1StrokeStyle* RoundStroke();
    void GoToView(View v);   // animates the footer icon, then switches
    void SwitchView(View v);
    bool Animating() const;
    D2D1_COLOR_F MeterColorFor(Settings::MeterColor c) const;
    D2D1_COLOR_F SliderFill() const;  // filled part of the sliders (neutral with the green meter)
    void UpdateHexEdit();
    LRESULT WndProc(UINT, WPARAM, LPARAM);

    void ApplyWindowStyle();
    void LoadPalette();
    bool EnsureResources();
    void Render();
    float Layout(bool draw);
    float LayoutMixer(float y, bool draw);
    float LayoutSettings(float y, bool draw);
    void LayoutFooter(float top, bool draw);
    void Reposition(bool animate);
    // `full * fraction` in whole device pixels: the panel is whole pixels
    // tall, so content under an opening section moves in whole pixels too
    // instead of shimmering.
    float PixelPart(float full, float fraction) const {
        if (fraction >= 1) return full;
        return std::round(full * fraction * scale_) / scale_;
    }

    // Windows.UI.Composition visual tree (see flyout.cpp).
    struct Composition;
    Composition* comp_ = nullptr;  // lives as long as the app
    bool CreateComposition();
    bool EnsureDevice();
    void UpdateBackdrop();
    void UpdateShadowMask();
    void PlacePanel(int heightPx);
    void UpdateRegion(bool settleLater);
    D2D1_POINT_2F ToPanel(int clientX, int clientY) const;
    int panelX_ = 0, panelY_ = 0, panelW_ = 0, panelH_ = 0;  // client pixels
    int panelEdge_ = 0;       // y of the edge that stays put (next to the taskbar)
    bool anchorTop_ = false;  // taskbar at the top: the panel grows down
    RECT region_{};           // current input region
    D2D1::Matrix3x2F drawBase_ = D2D1::Matrix3x2F::Identity();  // surface offset
    std::vector<int> VisibleDevices();
    template <class D> std::wstring DeviceName(const D& d) const {
        return settings_->DisplayDeviceName(d.name, d.technicalName);
    }
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
    void Slider(const Hit& h, float cy, float value, bool muted, float peak, bool draw);
    void SliderBar(float x0, float x1, float cy, float value, bool muted, float peak, float dot);
    void NyanSliderBar(float x0, float x1, float cy, float value, bool muted, float peak, float dot);
    int nyanFrame_ = 0;  // the Nyan Cat animation step last drawn
    void VolumeIcon(float volume, bool muted, const D2D1_RECT_F& r);
    void ToggleSwitch(const Hit& h, float x, float cy, bool on, bool draw);
    void AppIcon(const AppEntry& a, const D2D1_RECT_F& r, float opacity);
    void DeviceIcon(const AudioDevice& d, const D2D1_RECT_F& r, float size, const D2D1_COLOR_F& c);
    // The battery of the device's headset, ending at `right`; returns where it starts.
    float BatteryBadge(const AudioDevice& d, float right, float top, float bottom, bool draw);
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
    bool deviceMenuOpen_ = false;
    Anim menuAnim_;
    void SetDeviceMenu(bool open);
    D2D1_RECT_F switchIconRect_{};  // the output selector's switch icon, for its tooltip
    bool tipArmed_ = false, tipShown_ = false, tipBlocked_ = false;
    const wchar_t* tipText_ = nullptr;  // what the tooltip says, for what the mouse is over
    D2D1_RECT_F tipAnchor_{};           // ...and where that is
    // Settings descriptions behind (i) icons: an Info hit's `device` indexes this.
    std::vector<const wchar_t*> infoTexts_;
    int InfoId(const wchar_t* text);
    void DrawTooltip();
    void HideTooltip();
    std::map<std::wstring, bool> expanded_;  // user's collapse/expand choices, by device id
    ULONGLONG lastHideTick_ = 0;
    View view_ = View::Mixer;
    bool startup_ = false;
    bool upToDateStale_ = false;  // "up to date" is from an earlier Settings visit: don't show it

    float scale_ = 1.0f;
    float winH_ = 100000.0f;  // panel height in DIPs
    float maxH_ = 100000.0f;  // tallest the panel can be on its monitor
    float contentBottom_ = 0;
    float scroll_ = 0, maxScroll_ = 0;

    std::vector<Hit> hits_;
    Hit hover_, pressed_;
    bool dragging_ = false;
    bool tracking_ = false;

    Palette pal_;
    ComPtr<ID2D1Factory1> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID2D1RenderTarget> rt_;  // target being drawn: the panel surface, or the OSD
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1PathGeometry> gearGeo_;
    ComPtr<ID2D1StrokeStyle> roundStroke_;
    Anim gear_, arrow_;    // footer icon rotation, degrees
    Anim transition_;      // 0..1 entrance of the current view
    int transitionDir_ = 1;
    bool pending_ = false;  // a view switch waiting for the icon animation
    View pendingView_ = View::Mixer;
    double pendingAt_ = 0;
    bool pumping_ = false;  // rendering frame after frame while animating
    bool warmed_ = false;   // the other view has been laid out once
    void StartFramePump();
    void QueueFrame();
    void AnimFrame();

    // Settings sections: Settings, Personalization, Inputs, Outputs. Each has a 0..1 expansion.
    static constexpr int kSections = 4;
    bool sectionOpen_[kSections] = {true, false, false, false};
    Anim sectionAnim_[kSections] = {{1, 1, 1, 0}, {}, {}, {}};
    std::map<std::wstring, Anim> deviceAnim_;
    bool LayoutAnimating() const;
    // Custom colour fields (real EDIT controls): 0 = meter, 1 = slider bar.
    HWND hexEdit_[2] = {};
    HFONT hexFont_ = nullptr;
    HBRUSH hexBrush_ = nullptr;
    D2D1_RECT_F hexRect_[2] = {};
    bool hexVisible_ = false;
    bool previewVisible_ = false;  // the animated meter preview in Personalization
    static LRESULT CALLBACK HexEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    // Colour picker under the hex fields, opened from a field's swatch:
    // a saturation/brightness square and a hue bar.
    int pickerField_ = -1;  // hex field it edits, -1 = closed
    float pickerH_ = 0, pickerS_ = 0, pickerV_ = 0;  // hue 0..360, the rest 0..1
    bool pickerSyncing_ = false;  // writing the picked colour into the hex field
    void SyncPickerFromColor();
    void SetPickerValue(const Hit& h, float x, float y);
    void DrawColorPicker(const D2D1_RECT_F& sv, const D2D1_RECT_F& hue);

    HWND osd_ = nullptr;
    ComPtr<ID2D1HwndRenderTarget> osdRt_;
    ComPtr<ID2D1SolidColorBrush> osdBrush_;
    float osdScale_ = 1.0f;
    std::wstring osdName_;
    float osdVolume_ = 0;
    bool osdMuted_ = false;
    ComPtr<IDWriteTextFormat> fTitle_, fBody_, fCaption_, fDesc_;
    std::map<int, ComPtr<IDWriteTextFormat>> iconFormats_;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> icons_;
};
