#include "flyout.h"
#include "strings.h"

#include <shellscalingapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <cmath>
#include <d3d11.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>

#undef GetCurrentTime  // clashes with C++/WinRT
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

namespace wuc = winrt::Windows::UI::Composition;
namespace abi = ABI::Windows::UI::Composition;

static const float kWidth = 360.0f;
static const float kFooterHeight = 48.0f;  // same as the Quick Settings footer
static const UINT_PTR kMeterTimer = 1;
static const UINT WM_OUTSIDE_CLICK = WM_APP + 100;
static const UINT WM_ANIM_FRAME = WM_APP + 101;  // next frame of an animation
static const UINT WM_WARMUP = WM_APP + 102;      // lay out the other view once, after showing
static const UINT_PTR kRegionTimer = 2;          // shrink the input region once the panel settled
static const UINT_PTR kTipTimer = 3;             // hover delay before the "Change output" tooltip
static const float kShadowMargin = 32.0f;        // room around the panel for its shadow, DIPs
static const float kCornerRadius = 8.0f;

// While the flyout is open, a low-level mouse hook closes it on a click
// anywhere else. Losing activation isn't enough: when it is opened from the
// Windows volume icon, explorer keeps the foreground and the flyout is never
// activated, so it would never get WA_INACTIVE.
static Flyout* g_hookOwner = nullptr;
static HHOOK g_mouseHook = nullptr;

static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_hookOwner &&
        (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN || wp == WM_XBUTTONDOWN)) {
        auto info = (MSLLHOOKSTRUCT*)lp;
        RECT rc = g_hookOwner->PanelRect();
        if (!PtInRect(&rc, info->pt)) PostMessageW(g_hookOwner->Hwnd(), WM_OUTSIDE_CLICK, 0, 0);
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}
static const wchar_t kTextFont[] = L"Segoe UI Variable Text";
static const wchar_t kIconFont[] = L"Segoe Fluent Icons";

// Segoe Fluent Icons code points.
enum : wchar_t {
    kGlyphMute = 0xE74F,
    kGlyphVolume0 = 0xE992,
    kGlyphVolume1 = 0xE993,
    kGlyphVolume2 = 0xE994,
    kGlyphVolume3 = 0xE995,
    kGlyphSpeakers = 0xE7F5,
    kGlyphHeadphones = 0xE7F6,
    kGlyphSettings = 0xE713,
    kGlyphBack = 0xE72B,
    kGlyphApp = 0xECAA,
    kGlyphMicrophone = 0xE720,
    kGlyphDownload = 0xE896,
    kGlyphVolumeBars = 0xEBC5,
    kGlyphWarning = 0xE7BA,
    kGlyphSwitch = 0xE8AB,
};

#ifdef VO_PERF
#include <cstdio>
static double PerfNow() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / f.QuadPart;
}
static void PerfLog(const char* what, double ms) {
    static FILE* f = nullptr;
    if (!f) {
        wchar_t path[MAX_PATH];
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"VolumeOnePerf.log");
        _wfopen_s(&f, path, L"w");
    }
    if (f) {
        fprintf(f, "%10.1f %-14s %7.2f ms\n", PerfNow(), what, ms);
        fflush(f);
    }
}
#define PERF_BEGIN(name) double perf_##name = PerfNow()
#define PERF_END(name, label) PerfLog(label, PerfNow() - perf_##name)
#else
#define PERF_BEGIN(name) ((void)0)
#define PERF_END(name, label) ((void)0)
#endif

static D2D1_COLOR_F Rgba(UINT rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

static wchar_t VolumeGlyph(float v, bool muted) {
    if (muted) return kGlyphMute;
    if (v <= 0.0f) return kGlyphVolume0;
    if (v < 0.34f) return kGlyphVolume1;
    if (v < 0.67f) return kGlyphVolume2;
    return kGlyphVolume3;
}

static std::wstring Percent(float v) { return std::to_wstring((int)std::lround(v * 100.0f)); }


// ---------------------------------------------------------------------------

bool Flyout::Create(HINSTANCE inst, HWND owner, AudioManager* audio, Settings* settings, const UpdateState* update) {
    owner_ = owner;
    update_ = update;
    audio_ = audio;
    settings_ = settings;

    // Multi-threaded: the compositor uses the Direct2D device from its own thread.
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)dwrite_.GetAddressOf())))
        return false;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VolumeOne.Flyout";
    RegisterClassExW(&wc);

    // No redirection bitmap: everything shown is the composition visual tree.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, APP_NAME,
                            WS_POPUP | WS_CLIPCHILDREN, 0, 0, 10, 10, owner, nullptr, inst, this);
    if (!hwnd_) return false;

    LoadPalette();
    if (!CreateComposition()) return false;
    ApplyWindowStyle();
    return true;
}

// ---------------------------------------------------------------------------
// Composition. The window covers the tallest the panel can be and never
// changes size while it is open (DWM can't show a resized window and the new
// frame drawn for it in sync, which made content jump for a frame). The panel
// is a visual tree: a drop shadow, then a container clipped to a rounded
// rectangle with the blurred desktop (acrylic) and the content Direct2D draws
// into a composition surface. Its size and its content change in the same
// composition commit, so animating its height can't show a mismatched frame.

struct Flyout::Composition {
    winrt::Windows::System::DispatcherQueueController queue{nullptr};
    wuc::Compositor compositor{nullptr};
    wuc::Desktop::DesktopWindowTarget target{nullptr};
    wuc::ContainerVisual root{nullptr};
    wuc::SpriteVisual shadow{nullptr};
    wuc::DropShadow dropShadow{nullptr};
    wuc::ContainerVisual panel{nullptr};
    wuc::CompositionRoundedRectangleGeometry clipShape{nullptr};
    wuc::SpriteVisual backdrop{nullptr};
    wuc::SpriteVisual content{nullptr};
    wuc::CompositionGraphicsDevice graphics{nullptr};
    wuc::CompositionDrawingSurface surface{nullptr};
    wuc::CompositionDrawingSurface mask{nullptr};  // rounded rectangle the shadow is cast from
    wuc::CompositionNineGridBrush maskBrush{nullptr};
    ComPtr<ID2D1Device> device;
    ComPtr<ID3D11DeviceContext> d3dContext;
    int surfaceW = 0, surfaceH = 0;
    bool surfaceFresh = true;  // not drawn since created or resized
    int maskPx = 0;
};

bool Flyout::CreateComposition() {
    try {
        auto c = new Composition();
        // Windows.UI.Composition needs a DispatcherQueue on this thread; it
        // runs on the app's normal message loop.
        DispatcherQueueOptions options = {sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
        winrt::check_hresult(CreateDispatcherQueueController(
            options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(c->queue))));
        c->compositor = wuc::Compositor();
        auto desktop = c->compositor.as<abi::Desktop::ICompositorDesktopInterop>();
        // Not topmost, so the hex colour EDIT (a child window) shows above it.
        winrt::check_hresult(desktop->CreateDesktopWindowTarget(
            hwnd_, FALSE, reinterpret_cast<abi::Desktop::IDesktopWindowTarget**>(winrt::put_abi(c->target))));

        auto& k = c->compositor;
        c->root = k.CreateContainerVisual();
        c->target.Root(c->root);
        c->shadow = k.CreateSpriteVisual();
        c->dropShadow = k.CreateDropShadow();
        c->shadow.Shadow(c->dropShadow);
        c->panel = k.CreateContainerVisual();
        c->clipShape = k.CreateRoundedRectangleGeometry();
        c->panel.Clip(k.CreateGeometricClip(c->clipShape));
        c->backdrop = k.CreateSpriteVisual();
        c->content = k.CreateSpriteVisual();
        c->panel.Children().InsertAtTop(c->backdrop);
        c->panel.Children().InsertAtTop(c->content);
        c->root.Children().InsertAtTop(c->shadow);
        c->root.Children().InsertAtTop(c->panel);
        comp_ = c;
    } catch (...) {
        return false;
    }
    return EnsureDevice();
}

// Direct3D/Direct2D device the composition surfaces are drawn with; created
// again (and handed to the compositor) if the GPU device is lost.
bool Flyout::EnsureDevice() {
    Composition* c = comp_;
    if (c->device) return true;
    ComPtr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    if (FAILED(hr) || FAILED(d3d.As(&dxgi)) || FAILED(d2d_->CreateDevice(dxgi.Get(), &device))) return false;
    try {
        if (!c->graphics) {
            auto interop = c->compositor.as<abi::ICompositorInterop>();
            winrt::check_hresult(interop->CreateGraphicsDevice(
                device.Get(), reinterpret_cast<abi::ICompositionGraphicsDevice**>(winrt::put_abi(c->graphics))));
        } else {
            winrt::check_hresult(c->graphics.as<abi::ICompositionGraphicsDeviceInterop>()->SetRenderingDevice(device.Get()));
        }
    } catch (...) {
        return false;
    }
    c->device = device;
    c->d3dContext.Reset();
    d3d->GetImmediateContext(&c->d3dContext);
    // Device resources: the drawing helpers' brush and the cached icons.
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
    brush_.Reset();
    dc->CreateSolidColorBrush(pal_.text, &brush_);
    icons_.clear();
    c->maskPx = 0;  // redraw the shadow mask
    c->surfaceFresh = true;
    return true;
}

// Begins drawing into a composition surface: returns the device context with
// the surface's offset applied (as drawBase_), or null if the device is lost.
static ComPtr<ID2D1DeviceContext> BeginSurfaceDraw(const wuc::CompositionDrawingSurface& surface, const RECT& update,
                                                   float scale, D2D1::Matrix3x2F* base, HRESULT* result) {
    ComPtr<ID2D1DeviceContext> dc;
    POINT offset{};
    auto interop = surface.as<abi::ICompositionDrawingSurfaceInterop>();
    *result = interop->BeginDraw(&update, __uuidof(ID2D1DeviceContext), (void**)dc.GetAddressOf(), &offset);
    if (FAILED(*result)) return nullptr;
    dc->SetDpi(96.0f * scale, 96.0f * scale);
    *base = D2D1::Matrix3x2F::Translation(offset.x / scale, offset.y / scale);
    dc->SetTransform(*base);
    // Only the update rectangle is ours (the surface can be part of an atlas).
    dc->PushAxisAlignedClip({0, 0, (update.right - update.left) / scale, (update.bottom - update.top) / scale},
                            D2D1_ANTIALIAS_MODE_ALIASED);
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    return dc;
}

// Ends drawing and submits it to the GPU right away. Without the flush the
// commands wait in Direct3D's queue and the compositor keeps showing the
// surface's previous content: animations didn't show and everything lagged.
static void EndSurfaceDraw(const wuc::CompositionDrawingSurface& surface, ID2D1DeviceContext* dc,
                           ID3D11DeviceContext* d3d) {
    dc->PopAxisAlignedClip();
    surface.as<abi::ICompositionDrawingSurfaceInterop>()->EndDraw();
    if (d3d) d3d->Flush();
}

static bool DeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET;
}

// Acrylic: the blurred desktop behind the window (our tint is drawn over it
// with the content). With transparency effects off, a solid colour.
void Flyout::UpdateBackdrop() {
    Composition* c = comp_;
    DWORD transparency = 1, size = sizeof(transparency);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &size);
    try {
        if (transparency) {
            c->backdrop.Brush(c->compositor.CreateHostBackdropBrush());
        } else {
            D2D1_COLOR_F t = pal_.tint;
            c->backdrop.Brush(c->compositor.CreateColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(255, (BYTE)(t.r * 255), (BYTE)(t.g * 255), (BYTE)(t.b * 255))));
        }
    } catch (...) {
    }
    c->dropShadow.Color(winrt::Windows::UI::ColorHelper::FromArgb(255, 0, 0, 0));
    c->dropShadow.Opacity(pal_.dark ? 0.45f : 0.22f);
}

// Puts the panel at its height (DIPs) against the edge next to the taskbar.
void Flyout::PlacePanel(int heightPx) {
    Composition* c = comp_;
    float w = (float)panelW_, h = (float)heightPx;
    panelH_ = heightPx;
    panelY_ = anchorTop_ ? panelEdge_ : panelEdge_ - heightPx;
    winrt::Windows::Foundation::Numerics::float3 offset{(float)panelX_, (float)panelY_, 0};
    winrt::Windows::Foundation::Numerics::float2 size{w, h};
    c->panel.Offset(offset);
    c->panel.Size(size);
    c->clipShape.Size(size);
    float r = kCornerRadius * scale_;
    c->clipShape.CornerRadius({r, r});
    c->backdrop.Size(size);
    c->content.Size(size);
    c->shadow.Offset(offset);
    c->shadow.Size(size);
    c->dropShadow.BlurRadius(32 * scale_);
    c->dropShadow.Offset({0, 8 * scale_, 0});
}

// The shadow is cast from a rounded rectangle, stretched with a nine-grid so
// the corners keep their radius at any size. The panel covers the rectangle
// itself, so it doubles as the shadow sprite's brush.
void Flyout::UpdateShadowMask() {
    Composition* c = comp_;
    int r = (int)std::ceil(kCornerRadius * scale_);
    int px = r * 2 + 2;
    if (c->maskPx == px && c->maskBrush) return;
    try {
        if (!c->mask) {
            c->mask = c->graphics.CreateDrawingSurface({(float)px, (float)px},
                                                       winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                       winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
            c->maskBrush = c->compositor.CreateNineGridBrush();
            c->maskBrush.Source(c->compositor.CreateSurfaceBrush(c->mask));
        } else {
            winrt::check_hresult(c->mask.as<abi::ICompositionDrawingSurfaceInterop>()->Resize({px, px}));
        }
    } catch (...) {
        return;
    }
    D2D1::Matrix3x2F base;
    HRESULT hr;
    auto dc = BeginSurfaceDraw(c->mask, {0, 0, px, px}, 1.0f, &base, &hr);
    if (!dc) {
        if (DeviceLost(hr)) c->device.Reset();
        return;
    }
    ComPtr<ID2D1SolidColorBrush> white;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    dc->FillRoundedRectangle(D2D1::RoundedRect({0, 0, (float)px, (float)px}, (float)r, (float)r), white.Get());
    EndSurfaceDraw(c->mask, dc.Get(), c->d3dContext.Get());
    c->maskBrush.SetInsets((float)r);
    c->shadow.Brush(c->maskBrush);
    c->dropShadow.Mask(c->maskBrush);
    c->maskPx = px;
}

// Clicks only reach the window over the panel (and its shadow). While the
// panel animates it covers the whole window so it can't be clipped; once it
// settles the region shrinks back, a moment later so the last frame is
// already on screen.
void Flyout::UpdateRegion(bool settleLater) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int m = (int)std::lround(kShadowMargin * scale_);
    RECT want = rc;
    if (!LayoutAnimating()) {
        want = {panelX_ - m, panelY_ - m, panelX_ + panelW_ + m, panelY_ + panelH_ + m};
        IntersectRect(&want, &want, &rc);
    }
    bool grows = want.left < region_.left || want.top < region_.top || want.right > region_.right ||
                 want.bottom > region_.bottom;
    if (EqualRect(&want, &region_)) return;
    if (grows || !settleLater) {
        region_ = want;
        SetWindowRgn(hwnd_, CreateRectRgnIndirect(&want), FALSE);
    } else {
        SetTimer(hwnd_, kRegionTimer, 150, nullptr);
    }
}

RECT Flyout::PanelRect() const {
    POINT origin = {0, 0};
    ClientToScreen(hwnd_, &origin);
    return {origin.x + panelX_, origin.y + panelY_, origin.x + panelX_ + panelW_, origin.y + panelY_ + panelH_};
}

void Flyout::LoadPalette() {
    DWORD light = 0, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    bool dark = light == 0;

    // The accent palette holds 8 RGBA shades: Light3, Light2, Light1, Base,
    // Dark1, Dark2, Dark3. WinUI uses Light2 on dark and Dark1 on light.
    D2D1_COLOR_F accent = Rgba(dark ? 0x99EBFF : 0x005FB8);
    BYTE shades[32];
    size = sizeof(shades);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, shades, &size) == ERROR_SUCCESS &&
        size >= 28) {
        int i = (dark ? 1 : 4) * 4;
        accent = D2D1::ColorF(shades[i] / 255.0f, shades[i + 1] / 255.0f, shades[i + 2] / 255.0f, 1.0f);
    }

    Palette p;
    p.dark = dark;
    p.accent = accent;
    if (dark) {
        p.text = Rgba(0xFFFFFF);
        p.textSecondary = Rgba(0xFFFFFF, 0.786f);
        p.textTertiary = Rgba(0xFFFFFF, 0.545f);
        p.subtleHover = Rgba(0xFFFFFF, 0.0605f);
        p.subtlePressed = Rgba(0xFFFFFF, 0.0419f);
        p.card = Rgba(0xFFFFFF, 0.0512f);
        p.cardStroke = Rgba(0x000000, 0.10f);
        p.divider = Rgba(0xFFFFFF, 0.0837f);
        p.footer = Rgba(0x000000, 0.2f);  // ~#212121 on the tinted backdrop, like Quick Settings
        p.trackOff = Rgba(0xFFFFFF, 0.545f);
        p.thumb = Rgba(0x454545);
        p.thumbStroke = Rgba(0xFFFFFF, 0.094f);
        p.onAccent = Rgba(0x000000);
        p.toggleOffFill = Rgba(0x000000, 0.10f);
        p.tint = Rgba(0x202020, 0.82f);
        p.border = Rgba(0xFFFFFF, 0.10f);
        p.textDisabled = Rgba(0xFFFFFF, 0.3f);
    } else {
        p.text = Rgba(0x000000, 0.894f);
        p.textSecondary = Rgba(0x000000, 0.606f);
        p.textTertiary = Rgba(0x000000, 0.447f);
        p.subtleHover = Rgba(0x000000, 0.0373f);
        p.subtlePressed = Rgba(0x000000, 0.0241f);
        p.card = Rgba(0xFFFFFF, 0.70f);
        p.cardStroke = Rgba(0x000000, 0.0578f);
        p.divider = Rgba(0x000000, 0.0803f);
        p.footer = Rgba(0x000000, 0.03f);
        p.trackOff = Rgba(0x000000, 0.447f);
        p.thumb = Rgba(0xFFFFFF);
        p.thumbStroke = Rgba(0x000000, 0.10f);
        p.onAccent = Rgba(0xFFFFFF);
        p.toggleOffFill = Rgba(0x000000, 0.0241f);
        p.tint = Rgba(0xF3F3F3, 0.82f);
        p.border = Rgba(0x000000, 0.10f);
        p.textDisabled = Rgba(0x000000, 0.25f);
    }
    // Level meter: the accent mixed towards white, solid, so it stands out
    // against the filled part of the slider in both themes.
    pal_ = p;
    pal_.meter = MeterColorFor((Settings::MeterColor)settings_->meterColor);
}

void Flyout::ApplyWindowStyle() {
    // The panel draws its own corners, border and shadow, and its backdrop is
    // the host backdrop brush; DWM adds nothing to the (bigger) window.
    BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_HOSTBACKDROPBRUSH, &on, sizeof(on));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd_, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    UpdateBackdrop();
}

static ComPtr<IDWriteTextFormat> MakeFormat(IDWriteFactory* f, const wchar_t* family, float size,
                                            DWRITE_FONT_WEIGHT weight, bool wrap) {
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(f->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                   L"", &fmt)))
        return nullptr;
    if (wrap) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    } else {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> sign;
        f->CreateEllipsisTrimmingSign(fmt.Get(), &sign);
        fmt->SetTrimming(&trimming, sign.Get());
    }
    return fmt;
}

bool Flyout::EnsureResources() {
    if (!fTitle_) {
        fTitle_ = MakeFormat(dwrite_.Get(), kTextFont, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
        fBody_ = MakeFormat(dwrite_.Get(), kTextFont, 14, DWRITE_FONT_WEIGHT_NORMAL, false);
        fCaption_ = MakeFormat(dwrite_.Get(), kTextFont, 12, DWRITE_FONT_WEIGHT_NORMAL, false);
        fDesc_ = MakeFormat(dwrite_.Get(), kTextFont, 12, DWRITE_FONT_WEIGHT_NORMAL, true);
        if (!fTitle_ || !fBody_ || !fCaption_ || !fDesc_) return false;
    }
    return EnsureDevice();
}

// ---------------------------------------------------------------------------
// Showing and positioning

void Flyout::Toggle() {
    if (visible_)
        Hide();
    else if (GetTickCount64() - lastHideTick_ > 300)  // the click that closed us
        Show();
}

void Flyout::Show(bool settingsView) {
    if (visible_) {
        SetForegroundWindow(hwnd_);
        GoToView(settingsView ? View::Settings : View::Mixer);
        return;
    }
    // The surface still holds whatever was drawn last; a different view needs
    // its frame on screen before the window shows, or the old one flashes.
    View want = settingsView ? View::Settings : View::Mixer;
    bool viewChanged = want != view_;
    view_ = want;
    scroll_ = 0;
    deviceMenuOpen_ = false;
    menuAnim_.Set(0);
    pending_ = false;
    gear_.Set(0);
    arrow_.Set(0);
    transition_.Set(1);
    hover_ = pressed_ = Hit{};
    startup_ = GetStartWithWindows();
    audio_->SetTrackSessions(true);

    Reposition(false);
    if (viewChanged) DwmFlush();
    RECT final;
    GetWindowRect(hwnd_, &final);
    int dist = (int)std::lround(24 * scale_);
    SetWindowPos(hwnd_, HWND_TOPMOST, final.left + slideDx_ * dist, final.top + slideDy_ * dist, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);

    visible_ = true;
    SetTimer(hwnd_, kMeterTimer, 10, nullptr);  // level meters (one ~15.6 ms timer tick)
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    UpdateWindow(hwnd_);

    // Slide in with an ease-out curve, one step per compositor frame.
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    const double duration = 0.20;
    for (;;) {
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart / duration;
        if (t >= 1.0 || !visible_) break;
        double e = 1.0 - std::pow(1.0 - t, 3.0);
        int off = (int)std::lround(dist * (1.0 - e));
        SetWindowPos(hwnd_, nullptr, final.left + slideDx_ * off, final.top + slideDy_ * off, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        DwmFlush();
    }
    if (visible_)
        SetWindowPos(hwnd_, nullptr, final.left, final.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (visible_ && !warmed_) PostMessageW(hwnd_, WM_WARMUP, 0, 0);
    if (visible_ && !g_mouseHook) {
        g_hookOwner = this;
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
    }
}

void Flyout::Hide() {
    if (!visible_) return;
    visible_ = false;
    deviceMenuOpen_ = false;
    menuAnim_.Set(0);
    KillTimer(hwnd_, kMeterTimer);
    HideTooltip();
    for (HWND e : hexEdit_)
        if (e) ShowWindow(e, SW_HIDE);
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
        g_hookOwner = nullptr;
    }
    dragging_ = false;
    lastHideTick_ = GetTickCount64();
    ShowWindow(hwnd_, SW_HIDE);
    if (GetCapture() == hwnd_) ReleaseCapture();
    audio_->SetTrackSessions(false);
    // Next open starts in the mixer: draw it now, while hidden, so the
    // Settings frame never shows when the window comes back.
    if (view_ != View::Mixer || transition_.Running()) {
        view_ = View::Mixer;
        scroll_ = 0;
        transition_.Set(1);
        Render();
    }
}

void Flyout::Reposition(bool) {
    PERF_BEGIN(repos);
    RECT anchor{};
    NOTIFYICONIDENTIFIER nii = {sizeof(nii)};
    nii.hWnd = owner_;
    nii.uID = 1;
    PERF_BEGIN(rect);
    HRESULT rectHr = Shell_NotifyIconGetRect(&nii, &anchor);
    PERF_END(rect, "IconGetRect");
    if (FAILED(rectHr)) {
        POINT pt;
        GetCursorPos(&pt);
        anchor = {pt.x, pt.y, pt.x + 1, pt.y + 1};
    }
    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    if (dpiX / 96.0f != scale_) {
        scale_ = dpiX / 96.0f;
        icons_.clear();
    }
    if (!EnsureResources()) return;

    const RECT& work = mi.rcWork;
    const RECT& full = mi.rcMonitor;
    maxH_ = (work.bottom - work.top) / scale_ - 24.0f;

    // The panel's fixed edges, 12 DIPs from the work area like Quick Settings.
    int w = (int)std::lround(kWidth * scale_);
    int m = (int)std::lround(12 * scale_);
    int shadow = (int)std::lround(kShadowMargin * scale_);
    int left = work.right - m - w;
    anchorTop_ = false;
    slideDx_ = 0;
    slideDy_ = 1;
    if (work.top > full.top) {  // taskbar at the top
        anchorTop_ = true;
        slideDy_ = -1;
    } else if (work.left > full.left) {  // taskbar on the left
        left = work.left + m;
        slideDx_ = -1;
        slideDy_ = 0;
    } else if (work.right < full.right) {  // taskbar on the right
        slideDx_ = 1;
        slideDy_ = 0;
    }
    // The window: the panel's column at its tallest, plus room for the shadow.
    RECT win = {std::max<LONG>(work.left, left - shadow), work.top, std::min<LONG>(work.right, left + w + shadow), work.bottom};
    panelX_ = left - win.left;
    panelW_ = w;
    panelEdge_ = anchorTop_ ? m : (win.bottom - win.top) - m;
    PERF_BEGIN(swp);
    SetWindowPos(hwnd_, HWND_TOPMOST, win.left, win.top, win.right - win.left, win.bottom - win.top, SWP_NOACTIVATE);
    PERF_END(swp, "SetWindowPos");
    Render();
    UpdateRegion(false);
    PERF_END(repos, "Reposition");
}

void Flyout::OnAudioStructureChanged() {
    if (!visible_) return;
    if (dragging_) {
        dragging_ = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
    hover_ = pressed_ = Hit{};
    Reposition(false);
}

void Flyout::OnVolumeChanged() {
    if (visible_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Flyout::OnThemeChanged() {
    LoadPalette();
    ApplyWindowStyle();
    if (brush_) brush_->SetColor(pal_.text);
    if (visible_) InvalidateRect(hwnd_, nullptr, FALSE);
}

std::vector<int> Flyout::VisibleDevices() {
    std::vector<int> result;
    auto& devices = audio_->Devices();
    for (int i = 0; i < (int)devices.size(); i++)
        if (devices[i].isDefault || !settings_->IsDeviceHidden(devices[i].id)) result.push_back(i);
    return result;
}

// ---------------------------------------------------------------------------
// Drawing helpers

void Flyout::Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius) {
    brush_->SetColor(c);
    if (radius > 0)
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
    else
        rt_->FillRectangle(r, brush_.Get());
}

void Flyout::Stroke(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius, float width) {
    brush_->SetColor(c);
    float i = width / 2;
    D2D1_RECT_F rr = {r.left + i, r.top + i, r.right - i, r.bottom - i};
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(rr, radius, radius), brush_.Get(), width);
}

void Flyout::Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c,
                  DWRITE_TEXT_ALIGNMENT align) {
    brush_->SetColor(c);
    f->SetTextAlignment(align);
    rt_->DrawText(s.c_str(), (UINT32)s.size(), f, r, brush_.Get());
}

float Flyout::TextHeight(const std::wstring& s, IDWriteTextFormat* f, float width) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, width, 10000, &layout))) return 16;
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    return std::ceil(m.height);
}

float Flyout::TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000, 100, &layout))) return 60;
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    return std::ceil(m.widthIncludingTrailingWhitespace);
}

void Flyout::Glyph(wchar_t g, float size, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    auto& fmt = iconFormats_[(int)size];
    if (!fmt) {
        dwrite_->CreateTextFormat(kIconFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
        if (!fmt) return;
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    brush_->SetColor(c);
    rt_->DrawText(&g, 1, fmt.Get(), r, brush_.Get());
}

D2D1_COLOR_F Flyout::SliderFill() const {
    if (settings_->meterColor == Settings::MeterGreen) return pal_.dark ? Rgba(0xFFFFFF) : Rgba(0x3A3A3A);
    if (settings_->meterColor == Settings::MeterCustom) return Rgba(settings_->meterCustomBar);
    return pal_.accent;
}

// The level glyph over the full set of bars drawn dim, like the Windows
// volume icon, so the inactive bars still show.
void Flyout::VolumeIcon(float volume, bool muted, const D2D1_RECT_F& r) {
    if (!muted) Glyph(kGlyphVolumeBars, 16, r, pal_.textDisabled);
    Glyph(VolumeGlyph(volume, muted), 16, r, pal_.text);
}

void Flyout::Button(const Hit& h, bool draw, float radius) {
    hits_.push_back(h);
    if (draw && hover_ == h) Fill(h.rect, pressed_ == h ? pal_.subtlePressed : pal_.subtleHover, radius);
}

void Flyout::Slider(const Hit& h, float cy, float value, bool muted, float peak, bool draw) {
    hits_.push_back(h);
    if (!draw) return;
    float r = 6;
    if (pressed_ == h && dragging_) r = 5;
    else if (hover_ == h) r = 7;
    SliderBar(h.trackX0, h.trackX1, cy, value, muted, peak, r);
}

// The slider without its hit: track, fill, level meter and thumb (dot = the
// radius of the thumb's inner dot).
void Flyout::SliderBar(float x0, float x1, float cy, float value, bool muted, float peak, float dot) {
    float tx = x0 + std::clamp(value, 0.0f, 1.0f) * (x1 - x0);
    // With the green meter the accent fill would clash, so it turns neutral.
    D2D1_COLOR_F fill = muted ? pal_.textTertiary : SliderFill();

    Fill({x0, cy - 2, x1, cy + 2}, pal_.trackOff, 2);
    if (tx > x0 + 1) Fill({x0, cy - 2, tx, cy + 2}, fill, 2);
    // Level meter: a lighter band, thicker than the track, over the filled
    // part (like EarTrumpet).
    float px = std::min(x0 + std::clamp(peak, 0.0f, 1.0f) * (x1 - x0), tx);
    if (px > x0 + 1) Fill({x0, cy - 3, px, cy + 3}, pal_.meter, 3);

    // Thumb: neutral outer disc with an accent dot that grows on hover and
    // shrinks while pressed, like the WinUI slider.
    brush_->SetColor(pal_.thumb);
    rt_->FillEllipse(D2D1::Ellipse({tx, cy}, 10, 10), brush_.Get());
    brush_->SetColor(pal_.thumbStroke);
    rt_->DrawEllipse(D2D1::Ellipse({tx, cy}, 9.5f, 9.5f), brush_.Get(), 1);
    brush_->SetColor(fill);
    rt_->FillEllipse(D2D1::Ellipse({tx, cy}, dot, dot), brush_.Get());
}

void Flyout::ToggleSwitch(const Hit& h, float x, float cy, bool on, bool draw) {
    hits_.push_back(h);
    if (!draw) return;
    if (hover_ == h) Fill({h.rect.left + 2, h.rect.top + 2, h.rect.right - 2, h.rect.bottom - 2},
                          pressed_ == h ? pal_.subtlePressed : pal_.subtleHover, 6);
    D2D1_RECT_F r = {x, cy - 10, x + 40, cy + 10};
    float kr = hover_ == h ? 7.0f : 6.0f;
    if (on) {
        Fill(r, pal_.accent, 10);
        brush_->SetColor(pal_.onAccent);
        rt_->FillEllipse(D2D1::Ellipse({x + 30, cy}, kr, kr), brush_.Get());
    } else {
        Fill(r, pal_.toggleOffFill, 10);
        Stroke(r, pal_.textSecondary, 10);
        brush_->SetColor(pal_.textSecondary);
        rt_->FillEllipse(D2D1::Ellipse({x + 10, cy}, kr - 1, kr - 1), brush_.Get());
    }
}

ComPtr<ID2D1Bitmap> Flyout::LoadIconBitmap(const std::wstring& path, int px) {
    if (!wic_ && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_))))
        return nullptr;

    // Icon paths may look like "%windir%\foo.dll,-123".
    std::wstring file = path;
    int index = 0;
    size_t comma = file.rfind(L',');
    size_t slash = file.find_last_of(L"\\/");
    if (comma != std::wstring::npos && (slash == std::wstring::npos || comma > slash)) {
        index = _wtoi(file.c_str() + comma + 1);
        file.resize(comma);
    }
    wchar_t expanded[MAX_PATH * 2];
    if (!ExpandEnvironmentStringsW(file.c_str(), expanded, ARRAYSIZE(expanded))) return nullptr;

    HICON icon = nullptr;
    if (SHDefExtractIconW(expanded, index, 0, &icon, nullptr, px) != S_OK || !icon) {
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(expanded, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON)) icon = sfi.hIcon;
    }
    if (!icon) return nullptr;

    ComPtr<IWICBitmap> wb;
    HRESULT hr = wic_->CreateBitmapFromHICON(icon, &wb);
    DestroyIcon(icon);
    if (FAILED(hr)) return nullptr;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic_->CreateFormatConverter(&conv))) return nullptr;
    if (FAILED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return nullptr;
    ComPtr<ID2D1Bitmap> bmp;
    rt_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
    return bmp;
}

void Flyout::AppIcon(const AppEntry& a, const D2D1_RECT_F& r, float opacity) {
    ComPtr<ID2D1Bitmap> bmp;
    if (!a.isSystem && !a.iconPath.empty()) {
        int px = (int)std::lround(24 * scale_);
        std::wstring key = a.iconPath + L"|" + std::to_wstring(px);
        auto it = icons_.find(key);
        if (it == icons_.end()) it = icons_.emplace(key, LoadIconBitmap(a.iconPath, px)).first;
        bmp = it->second;
    }
    if (bmp) {
        rt_->DrawBitmap(bmp.Get(), r, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        D2D1_COLOR_F c = pal_.text;
        c.a *= opacity;
        Glyph(a.isSystem ? kGlyphSpeakers : kGlyphApp, 20, r, c);
    }
}

ComPtr<ID2D1Bitmap> Flyout::WicToD2D(IWICBitmapSource* src) {
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic_->CreateFormatConverter(&conv))) return nullptr;
    if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return nullptr;
    ComPtr<ID2D1Bitmap> bmp;
    rt_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
    return bmp;
}

// Custom device icons: .ico (largest frame) or any image WIC can decode,
// scaled to px.
ComPtr<ID2D1Bitmap> Flyout::LoadImageBitmap(const std::wstring& path, int px) {
    if (!wic_ && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_))))
        return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                               &decoder)))
        return nullptr;
    UINT frames = 0, best = 0, bestW = 0;
    decoder->GetFrameCount(&frames);
    for (UINT i = 0; i < frames; i++) {
        ComPtr<IWICBitmapFrameDecode> f;
        UINT w = 0, h = 0;
        if (SUCCEEDED(decoder->GetFrame(i, &f)) && SUCCEEDED(f->GetSize(&w, &h)) && w > bestW) {
            bestW = w;
            best = i;
        }
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(best, &frame))) return nullptr;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic_->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), px, px, WICBitmapInterpolationModeHighQualityCubic)))
        return nullptr;
    return WicToD2D(scaler.Get());
}

void Flyout::DeviceIcon(const AudioDevice& d, const D2D1_RECT_F& r, float size, const D2D1_COLOR_F& c) {
    auto it = settings_->deviceIcons.find(d.id);
    std::wstring v = it == settings_->deviceIcons.end() ? L"" : it->second;
    wchar_t glyph = d.formFactor == FormFactor::Headphones ? kGlyphHeadphones : kGlyphSpeakers;
    if (v == L"speakers") {
        glyph = kGlyphSpeakers;
    } else if (v == L"headphones") {
        glyph = kGlyphHeadphones;
    } else if (!v.empty()) {
        int px = (int)std::lround(size * scale_ * 1.25f);
        std::wstring key = L"img|" + v + L"|" + std::to_wstring(px);
        auto ic = icons_.find(key);
        if (ic == icons_.end()) ic = icons_.emplace(key, LoadImageBitmap(v, px)).first;
        if (ic->second) {
            float s = size * 1.25f, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            rt_->DrawBitmap(ic->second.Get(), {cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2}, c.a,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            return;
        }
    }
    Glyph(glyph, size, r, c);
}

// ---------------------------------------------------------------------------
// Layout. The same code measures (draw == false) and paints, so hit testing
// always matches what is on screen.

float Flyout::Layout(bool draw) {
    hits_.clear();
    contentBottom_ = winH_ - kFooterHeight;
    if (draw) rt_->PushAxisAlignedClip({0, 0, kWidth, contentBottom_}, D2D1_ANTIALIAS_MODE_ALIASED);
    float y0 = -scroll_;
    // Entrance of a view after switching: slide 24 DIPs and fade in.
    bool entering = draw && transition_.Running();
    if (entering) {
        float v = transition_.Value();
        rt_->SetTransform(D2D1::Matrix3x2F::Translation(transitionDir_ * 24.0f * (1 - v), 0) * drawBase_);
        rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), v),
                       nullptr);
    }
    float end = view_ == View::Mixer ? LayoutMixer(y0, draw) : LayoutSettings(y0, draw);
    if (entering) {
        rt_->PopLayer();
        rt_->SetTransform(drawBase_);
    }
    if (draw) rt_->PopAxisAlignedClip();
    LayoutFooter(contentBottom_, draw);
    return (end - y0) + kFooterHeight;
}

float Flyout::LayoutMixer(float y, bool draw) {
    const float x0 = 12, x1 = kWidth - 12;
    auto& devices = audio_->Devices();
    auto visible = VisibleDevices();
    y += 12;

    // Warning when the exe lives somewhere that gets cleaned up (Downloads...),
    // in the style of a WinUI "caution" InfoBar.
    if (!locationName.empty()) {
        std::wstring text = Fmt(S().locationWarning, locationName);
        float th = TextHeight(text, fDesc_.Get(), x1 - x0 - 56);
        float bw = TextWidth(S().moveToDocuments, fBody_.Get()) + 28;
        float h = 12 + th + 8 + 32 + 12;
        Hit move{HitKind::MoveToDocuments, -1, -1, {x0 + 44, y + 12 + th + 8, x0 + 44 + bw, y + 12 + th + 40}};
        hits_.push_back(move);
        if (draw) {
            Fill({x0, y, x1, y + h}, pal_.dark ? Rgba(0x433519) : Rgba(0xFFF4CE), 8);
            Stroke({x0, y, x1, y + h}, pal_.cardStroke, 8);
            Glyph(kGlyphWarning, 16, {x0 + 12, y + 10, x0 + 36, y + 30}, pal_.dark ? Rgba(0xFCE100) : Rgba(0x9D5D00));
            Text(text, fDesc_.Get(), {x0 + 44, y + 12, x1 - 12, y + 12 + th}, pal_.text);
            Fill(move.rect, hover_ == move ? pal_.subtleHover : pal_.toggleOffFill, 4);
            Stroke(move.rect, pal_.cardStroke, 4);
            Text(S().moveToDocuments, fBody_.Get(), move.rect, pal_.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        y += h + 8;
    }

    // Update banner.
    if (update_ && (update_->status == UpdateState::Available || update_->status == UpdateState::Installing ||
                    update_->status == UpdateState::InstallFailed)) {
        const float h = 52;
        bool installing = update_->status == UpdateState::Installing;
        const wchar_t* label = installing ? S().updating
                               : update_->status == UpdateState::InstallFailed ? S().updateFailed
                                                                                : S().updateNow;
        float bw = TextWidth(label, fBody_.Get()) + 28;
        Hit btn{HitKind::InstallUpdate, -1, -1, {x1 - 10 - bw, y + 10, x1 - 10, y + 42}};
        if (!installing) hits_.push_back(btn);
        // The rest of the banner opens Settings, which lists what's new.
        Hit details{HitKind::Gear, -1, -1, {x0, y, btn.rect.left - 4, y + h}};
        hits_.push_back(details);
        if (draw) {
            Fill({x0, y, x1, y + h}, pal_.card, 8);
            Stroke({x0, y, x1, y + h}, pal_.cardStroke, 8);
            Glyph(kGlyphDownload, 16, {x0 + 12, y, x0 + 40, y + h}, pal_.accent);
            Text(Fmt(S().updateAvailable, update_->version), fBody_.Get(), {x0 + 46, y, btn.rect.left - 8, y + h},
                 pal_.text);
            D2D1_COLOR_F fill = pal_.accent;
            if (installing) fill.a = 0.5f;
            else if (hover_ == btn) fill.a = pressed_ == btn ? 0.8f : 0.9f;
            Fill(btn.rect, fill, 4);
            Text(label, fBody_.Get(), btn.rect, pal_.onAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        y += h + 8;
    }

    int activeDevice = -1;
    for (int di : visible)
        if (devices[di].isDefault) activeDevice = di;
    if (activeDevice < 0) {
        if (draw) Text(S().noDevices, fBody_.Get(), {x0, y, x1, y + 72}, pal_.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        return y + 72 + 12;
    }

    for (int di : visible) {
        if (di != activeDevice) continue;
        AudioDevice& d = devices[di];
        bool expanded = IsExpanded(d);
        auto animation = deviceAnim_.find(d.id);
        float open = animation == deviceAnim_.end() ? (expanded ? 1.0f : 0.0f) : animation->second.Value();
        float appsHeight = d.apps.empty() ? 0 : 8 + 48.0f * d.apps.size();
        float appsShown = PixelPart(appsHeight, open);
        // The menu lists the other outputs; the current one is the header.
        std::vector<int> others;
        for (int vi : visible)
            if (vi != di) others.push_back(vi);
        const float menuRowHeight = 40;
        float menuFull = menuRowHeight * others.size();
        float menuV = menuAnim_.Value();
        float menuHeight = PixelPart(menuFull, menuV);
        float top = y;
        float headerTop = top + menuHeight;
        float h = 44 + menuHeight + appsShown + 48;
        if (draw) {
            Fill({x0, top, x1, top + h}, pal_.card, 8);
            Stroke({x0, top, x1, top + h}, pal_.cardStroke, 8);
        }

        // The current output is the selector; its menu slides open above it.
        if (menuHeight > 0) {
            size_t menuHits = hits_.size();
            if (draw) rt_->PushAxisAlignedClip({x0 + 1, top, x1 - 1, top + menuHeight}, D2D1_ANTIALIAS_MODE_ALIASED);
            float menuTop = top - (menuFull - menuHeight);
            for (int i = 0; i < (int)others.size(); i++) {
                int optionDevice = others[i];
                const AudioDevice& option = devices[optionDevice];
                D2D1_RECT_F row = {x0 + 1, menuTop + menuRowHeight * i, x1 - 1, menuTop + menuRowHeight * (i + 1)};
                Hit item{HitKind::SelectDevice, optionDevice, -1, row};
                hits_.push_back(item);
                if (draw) {
                    if (i > 0) Fill({x0 + 12, row.top, x1 - 12, row.top + 1}, pal_.divider);
                    if (hover_ == item) Fill(row, pal_.subtleHover);
                    DeviceIcon(option, {x0 + 12, row.top + 8, x0 + 40, row.top + 32}, 16, pal_.textSecondary);
                    Text(DeviceName(option), fBody_.Get(), {x0 + 48, row.top, x1 - 12, row.bottom}, pal_.text);
                }
            }
            if (draw) {
                rt_->PopAxisAlignedClip();
                Fill({x0 + 12, headerTop, x1 - 12, headerTop + 1}, pal_.divider);
            }
            if (menuAnim_.Running()) hits_.resize(menuHits);  // not clickable while moving
        }

        // With a single output there is nothing to switch to: just its name.
        float cy = headerTop + 12;
        float nameX = x0 + 12;
        switchIconRect_ = {};
        if (!others.empty()) {
            Hit header{HitKind::DeviceMenu, di, -1, {x0 + 1, headerTop + 1, x1 - 1, headerTop + 43.0f}};
            Button(header, draw, 7);
            switchIconRect_ = {x0 + 4, headerTop + 4, x0 + 40, headerTop + 40};
            if (draw) Glyph(kGlyphSwitch, 16, switchIconRect_, menuV > 0.5f ? pal_.accent : pal_.textSecondary);
            nameX = x0 + 40;
        } else if (deviceMenuOpen_) {
            deviceMenuOpen_ = false;
            menuAnim_.Set(0);
        }
        if (draw) {
            DeviceIcon(d, {nameX, cy, nameX + 28, cy + 20}, 16, pal_.textSecondary);
            Text(DeviceName(d), fTitle_.Get(), {nameX + 34, cy, x1 - 12, cy + 20}, pal_.text);
        }

        // The app list wipes open under the header, sliding out from under it.
        if (appsShown > 0) {
            size_t hitCount = hits_.size();
            float appsTop = headerTop + 44;
            if (draw) rt_->PushAxisAlignedClip({x0 + 1, appsTop, x1 - 1, appsTop + appsShown}, D2D1_ANTIALIAS_MODE_ALIASED);
            cy = appsTop - (appsHeight - appsShown);
            if (draw) Fill({x0 + 12, cy + 4, x1 - 12, cy + 5}, pal_.divider);
            cy += 8;
            for (int ai = 0; ai < (int)d.apps.size(); ai++) {
                AppEntry& a = d.apps[ai];
                Hit amute{HitKind::Mute, di, ai, {x0 + 6, cy + 4, x0 + 46, cy + 44}};
                Button(amute, draw);
                if (draw) {
                    float icx = (amute.rect.left + amute.rect.right) / 2, icy = (amute.rect.top + amute.rect.bottom) / 2;
                    AppIcon(a, {icx - 12, icy - 12, icx + 12, icy + 12}, a.muted ? 0.3f : 1.0f);
                    if (a.muted) Glyph(kGlyphMute, 14, amute.rect, pal_.text);
                    Text(a.name, fCaption_.Get(), {x0 + 54, cy + 2, x1 - 14, cy + 20}, pal_.textSecondary);
                }
                Hit aslider{HitKind::Slider, di, ai, {x0 + 50, cy + 16, x1 - 48, cy + 48}, x0 + 62, x1 - 60};
                Slider(aslider, cy + 32, a.volume, a.muted, a.peak, draw);
                if (draw) Text(Percent(a.volume), fBody_.Get(), {x1 - 52, cy + 16, x1 - 14, cy + 48}, pal_.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
                cy += 48;
            }
            if (draw) rt_->PopAxisAlignedClip();
            if (animation != deviceAnim_.end() && animation->second.Running()) hits_.resize(hitCount);
        }

        // The device volume stays at the bottom, under the app list.
        cy = headerTop + 44 + appsShown;
        Hit mute{HitKind::Mute, di, -1, {x0 + 6, cy, x0 + 46, cy + 40}};
        Button(mute, draw);
        if (draw) VolumeIcon(d.volume, d.muted, mute.rect);
        Hit slider{HitKind::Slider, di, -1, {x0 + 50, cy, x1 - 84, cy + 40}, x0 + 62, x1 - 96};
        Slider(slider, cy + 20, d.volume, d.muted, d.peak, draw);
        if (draw) Text(Percent(d.volume), fBody_.Get(), {x1 - 82, cy, x1 - 48, cy + 40}, pal_.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (!d.apps.empty()) {
            Hit collapse{HitKind::Collapse, di, -1, {x1 - 44, cy, x1 - 1, cy + 40}};
            Button(collapse, draw);
            if (draw) DrawChevron(collapse.rect, pal_.textSecondary, open);
        }
        y = top + h + 8;
    }
    return y + 4;
}

float Flyout::LayoutSettings(float y, bool draw) {
    const float x0 = 12, x1 = kWidth - 12;
    hexVisible_ = false;
    previewVisible_ = false;

    y += 12;
    const float textX = x0 + 16, textR = x1 - 16 - 40 - 16;

    auto card = [&](float top, float h) {
        if (!draw || h <= 0) return;
        Fill({x0, top, x1, top + h}, pal_.card, 8);
        Stroke({x0, top, x1, top + h}, pal_.cardStroke, 8);
    };
    // A collapsible section: one card with a title row and a body that wipes
    // open/closed with the section's animation. `body` draws at the current y
    // and advances it; it is measured first, then drawn (shifted up while
    // animating so it slides out from under the header).
    auto section = [&](int idx, const wchar_t* title, auto&& body) {
        const float hdrH = 48;
        float v = sectionAnim_[idx].Value();
        float hy = y, top = y + hdrH, h = 0;
        Hit hdr{HitKind::Section, idx, -1, {x0, hy, x1, top}};
        if (v > 0) {
            size_t hitCount = hits_.size();
            bool d = draw;
            draw = false;
            y = top;
            body();
            h = y - top;
            draw = d;
            hits_.resize(hitCount);
        }
        float shown = PixelPart(h, v);
        card(hy, hdrH + shown);
        Button(hdr, draw, 7);
        if (draw) {
            Text(title, fTitle_.Get(), {x0 + 16, hy, x1 - 56, top}, pal_.text);
            DrawChevron({x1 - 36, hy, x1 - 12, top}, pal_.textSecondary, v);
            if (shown > 0) Fill({x0 + 1, top, x1 - 1, top + 1}, pal_.divider);
        }
        if (v > 0) {
            size_t hitCount = hits_.size();
            if (draw) rt_->PushAxisAlignedClip({x0, top, x1, top + shown}, D2D1_ANTIALIAS_MODE_ALIASED);
            y = top - (h - shown);
            body();
            if (draw) rt_->PopAxisAlignedClip();
            if (sectionAnim_[idx].Running()) hits_.resize(hitCount);  // not clickable while moving
        }
        y = top + shown + 12;
    };

    struct Row { HitKind kind; const wchar_t* label; const wchar_t* desc; bool on; bool enabled; };
    auto divider = [&] {
        if (draw) Fill({x0 + 1, y, x1 - 1, y + 1}, pal_.divider);
    };
    auto toggleRows = [&](const Row* rows, int rowCount) {
        for (int i = 0; i < rowCount; i++) {
            float rh = 12 + 20 + TextHeight(rows[i].desc, fDesc_.Get(), textR - textX) + 12;
            if (i > 0) divider();
            if (draw && !rows[i].enabled)
                rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::IdentityMatrix(), 0.4f),
                               nullptr);
            Hit h{rows[i].kind, -1, -1, {x0, y, x1, y + rh}};
            ToggleSwitch(h, x1 - 16 - 40, y + rh / 2, rows[i].on, draw);
            if (!rows[i].enabled) hits_.pop_back();
            if (draw) {
                Text(rows[i].label, fBody_.Get(), {textX, y + 12, textR, y + 32}, pal_.text);
                Text(rows[i].desc, fDesc_.Get(), {textX, y + 32, textR, y + rh}, pal_.textSecondary);
                if (!rows[i].enabled) rt_->PopLayer();
            }
            y += rh;
        }
    };

    // A labelled row with a button on the right.
    auto buttonRow = [&](const wchar_t* label, const std::wstring& desc, const wchar_t* button, HitKind kind, bool accent,
                         bool enabled) {
        float bw = TextWidth(button, fBody_.Get()) + 28;
        float dh = TextHeight(desc, fDesc_.Get(), x1 - 12 - bw - 8 - textX);
        float h = std::max(64.0f, 12 + 20 + dh + 12);
        Hit btn{kind, -1, -1, {x1 - 12 - bw, y + h / 2 - 16, x1 - 12, y + h / 2 + 16}};
        if (enabled) hits_.push_back(btn);
        if (draw) {
            Text(label, fBody_.Get(), {textX, y + 12, btn.rect.left - 8, y + 32}, pal_.text);
            Text(desc, fDesc_.Get(), {textX, y + 32, btn.rect.left - 8, y + 32 + dh}, pal_.textSecondary);
            D2D1_COLOR_F fill = accent ? pal_.accent : pal_.toggleOffFill;
            if (!enabled) fill.a *= 0.5f;
            else if (hover_ == btn) fill = accent ? D2D1::ColorF(fill.r, fill.g, fill.b, 0.9f) : pal_.subtleHover;
            Fill(btn.rect, fill, 4);
            if (!accent) Stroke(btn.rect, pal_.cardStroke, 4);
            Text(button, fBody_.Get(), btn.rect, accent ? pal_.onAccent : pal_.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        y += h;
    };

    auto personalBody = [&] {
    bool useSystem = settings_->useSystemVolumeIcon;
    Row rows[] = {
        {HitKind::ToggleUseSystemIcon, S().useSystemIcon, S().useSystemIconDesc, useSystem, true},
        // Hiding the Windows icon makes no sense while it is VolumeOne's icon.
        {HitKind::ToggleHideIcon, S().hideSystemIcon, S().hideSystemIconDesc, settings_->hideSystemVolumeIcon && !useSystem,
         !useSystem},
    };
    toggleRows(rows, ARRAYSIZE(rows));
    divider();

    // Level meter colour: four choices, plus hex fields for "custom" (the
    // meter and the slider bar behind it), and an animated preview.
    {
        bool custom = settings_->meterColor == Settings::MeterCustom;
        float dh = TextHeight(S().meterColorDesc, fDesc_.Get(), x1 - x0 - 32);
        float h = 12 + 20 + dh + 10 + 32 + (custom ? 8 + 18 + 32 : 0) + 8 + 40 + 12;
        if (draw) {
            Text(S().meterColor, fBody_.Get(), {textX, y + 12, x1 - 16, y + 32}, pal_.text);
            Text(S().meterColorDesc, fDesc_.Get(), {textX, y + 32, x1 - 16, y + 32 + dh}, pal_.textSecondary);
        }
        const wchar_t* labels[] = {S().meterAccent, S().meterWhite, S().meterGreen, S().meterCustom};
        float cy = y + 32 + dh + 10;
        float gap = 6, cw = (x1 - x0 - 32 - gap * 3) / 4;
        for (int i = 0; i < 4; i++) {
            float cx = textX + i * (cw + gap);
            Hit chip{HitKind::MeterOption, i, -1, {cx, cy, cx + cw, cy + 32}};
            hits_.push_back(chip);
            if (!draw) continue;
            bool selected = (int)settings_->meterColor == i;
            Fill(chip.rect, hover_ == chip ? pal_.subtleHover : pal_.toggleOffFill, 4);
            Stroke(chip.rect, selected ? pal_.accent : pal_.cardStroke, 4, selected ? 2.0f : 1.0f);
            brush_->SetColor(MeterColorFor((Settings::MeterColor)i));
            rt_->FillEllipse(D2D1::Ellipse({cx + 14, cy + 16}, 5, 5), brush_.Get());
            Text(labels[i], fCaption_.Get(), {cx + 24, cy, cx + cw - 4, cy + 32}, pal_.text);
        }
        float py = cy + 32 + 8;
        if (custom) {
            const wchar_t* fieldLabels[] = {S().meterCustomLevel, S().meterCustomBar};
            DWORD colors[] = {settings_->meterCustomColor, settings_->meterCustomBar};
            float fw = (x1 - x0 - 32 - gap) / 2;
            for (int i = 0; i < 2; i++) {
                float fx = textX + i * (fw + gap), fy = py + 18;
                hexRect_[i] = {fx, fy, fx + fw, fy + 32};
                if (!draw) continue;
                const D2D1_RECT_F& r = hexRect_[i];
                Text(fieldLabels[i], fCaption_.Get(), {fx, py, fx + fw, py + 16}, pal_.textSecondary);
                Fill(r, pal_.toggleOffFill, 4);
                Stroke(r, pal_.cardStroke, 4);
                Fill({r.left + 4, r.bottom - 2, r.right - 4, r.bottom - 1}, pal_.accent);
                Text(L"#", fBody_.Get(), {r.left + 10, fy, r.left + 24, fy + 32}, pal_.textSecondary);
                // Swatch of the colour, right of the text.
                D2D1_RECT_F sw = {r.right - 26, fy + 8, r.right - 10, fy + 24};
                Fill(sw, Rgba(colors[i]), 3);
                Stroke(sw, pal_.cardStroke, 3);
            }
            hexVisible_ = !sectionAnim_[1].Running();
            py += 18 + 32 + 8;
        }
        // Preview: a slider at 75 % with a made-up level bouncing over it.
        previewVisible_ = true;
        if (draw) {
            D2D1_RECT_F pr = {textX, py, x1 - 16, py + 40};
            Fill(pr, pal_.toggleOffFill, 4);
            Stroke(pr, pal_.cardStroke, 4);
            float t = (float)(NowMs() / 1000.0);
            float level = 0.55f + 0.25f * std::sin(t * 2.1f) + 0.12f * std::sin(t * 7.3f + 1.0f) +
                          0.06f * std::sin(t * 17.0f + 2.0f);
            SliderBar(pr.left + 16, pr.right - 16, py + 20, 0.75f, false, std::clamp(level, 0.0f, 1.0f) * 0.75f, 6);
        }
        y += h;
    }
    divider();

    // Device names: the name set in Windows Sound settings ("nickname"), the
    // driver's name ("technical"), or both, as Windows shows them.
    {
        float dh = TextHeight(S().deviceNameDesc, fDesc_.Get(), x1 - x0 - 32);
        if (draw) {
            Text(S().deviceName, fBody_.Get(), {textX, y + 12, x1 - 16, y + 32}, pal_.text);
            Text(S().deviceNameDesc, fDesc_.Get(), {textX, y + 32, x1 - 16, y + 32 + dh}, pal_.textSecondary);
        }
        const wchar_t* labels[] = {S().nameBoth, S().nameNickname, S().nameTechnical};
        float cy = y + 32 + dh + 10;
        float gap = 6, cw = (x1 - x0 - 32 - gap * 2) / 3;
        for (int i = 0; i < 3; i++) {
            float cx = textX + i * (cw + gap);
            Hit chip{HitKind::NameOption, i, -1, {cx, cy, cx + cw, cy + 32}};
            hits_.push_back(chip);
            if (!draw) continue;
            bool selected = (int)settings_->deviceNameMode == i;
            Fill(chip.rect, hover_ == chip ? pal_.subtleHover : pal_.toggleOffFill, 4);
            Stroke(chip.rect, selected ? pal_.accent : pal_.cardStroke, 4, selected ? 2.0f : 1.0f);
            Text(labels[i], fCaption_.Get(), chip.rect, pal_.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        y = cy + 32 + 12;
    }
    divider();
    buttonRow(S().renameInWindows, S().renameInWindowsDesc, S().open, HitKind::SoundSettings, false, true);
    };

    // Output devices: icon button + visibility toggle.
    auto outputsBody = [&] {
    auto& devices = audio_->Devices();
    for (int i = 0; i < (int)devices.size(); i++) {
        const AudioDevice& d = devices[i];
        float top = y + 48.0f * i;
        bool shown = !settings_->IsDeviceHidden(d.id);
        D2D1_COLOR_F c = shown ? pal_.text : pal_.textTertiary;
        Hit icon{HitKind::IconPick, i, -1, {x0 + 4, top + 4, x0 + 44, top + 44}};
        Button(icon, draw);
        Hit h{HitKind::ToggleDevice, i, -1, {x0 + 46, top, x1, top + 48}};
        ToggleSwitch(h, x1 - 16 - 40, top + 24, shown, draw);
        if (draw) {
            if (i > 0) Fill({x0 + 1, top, x1 - 1, top + 1}, pal_.divider);
            DeviceIcon(d, icon.rect, 16, c);
            Text(DeviceName(d), fBody_.Get(), {x0 + 52, top, x1 - 72, top + 48}, c);
        }
    }
    y += 48.0f * devices.size();
    };

    // Input devices: visibility in the right-click menu.
    auto inputsBody = [&] {
    auto& inputs = audio_->Inputs();
    for (int i = 0; i < (int)inputs.size(); i++) {
        const InputDevice& in = inputs[i];
        float top = y + 48.0f * i;
        bool shown = !settings_->IsDeviceHidden(in.id);
        D2D1_COLOR_F c = shown ? pal_.text : pal_.textTertiary;
        Hit h{HitKind::ToggleInput, i, -1, {x0, top, x1, top + 48}};
        ToggleSwitch(h, x1 - 16 - 40, top + 24, shown, draw);
        if (draw) {
            if (i > 0) Fill({x0 + 1, top, x1 - 1, top + 1}, pal_.divider);
            Glyph(kGlyphMicrophone, 16, {x0 + 4, top + 4, x0 + 44, top + 44}, c);
            Text(DeviceName(in), fBody_.Get(), {x0 + 52, top, x1 - 72, top + 48}, c);
        }
    }
    y += 48.0f * inputs.size();
    };

    auto generalBody = [&] {
    Row rows[] = {
        {HitKind::ToggleHideQs, S().hideQsVolume, S().hideQsVolumeDesc, settings_->hideQuickSettingsVolume, true},
        {HitKind::ToggleStartup, S().startWithWindows, S().startWithWindowsDesc, startup_, true},
    };
    toggleRows(rows, ARRAYSIZE(rows));
    divider();

    // Leave: close and put Windows back as it was.
    buttonRow(S().closeAndRestore, S().closeAndRestoreDesc, S().closeAndRestoreButton, HitKind::CloseRestore, false, true);
    };

    // Updates: its own card at the bottom, always open.
    auto updatesBody = [&] {
        std::wstring status = L"VolumeOne " APP_VERSION;
        const wchar_t* button = S().checkUpdates;
        HitKind kind = HitKind::CheckUpdates;
        bool enabled = true;
        switch (update_ ? update_->status : UpdateState::Idle) {
            case UpdateState::Checking: status += std::wstring(L" · ") + S().checking; enabled = false; break;
            case UpdateState::UpToDate: status += std::wstring(L" · ") + S().upToDate; break;
            case UpdateState::Failed: status += std::wstring(L" · ") + S().checkFailed; break;
            case UpdateState::Available:
            case UpdateState::InstallFailed:
                status += L" · " + Fmt(S().updateAvailable, update_->version);
                button = S().updateNow;
                kind = HitKind::InstallUpdate;
                break;
            case UpdateState::Installing:
                status += std::wstring(L" · ") + S().updating;
                enabled = false;
                break;
            default: break;
        }
        buttonRow(S().updates, status, button, kind, kind == HitKind::InstallUpdate, enabled);

        bool hasUpdate = update_ && (update_->status == UpdateState::Available ||
                                     update_->status == UpdateState::InstallFailed ||
                                     update_->status == UpdateState::Installing);
        if (hasUpdate && !update_->notes.empty()) {
            divider();
            std::wstring title = Fmt(S().whatsNew, update_->version);
            float nh = TextHeight(update_->notes, fDesc_.Get(), x1 - x0 - 32);
            if (draw) Text(title, fTitle_.Get(), {x0 + 16, y + 12, x1, y + 32}, pal_.text);
            y += 40;
            if (draw) Text(update_->notes, fDesc_.Get(), {x0 + 16, y, x1 - 16, y + nh}, pal_.textSecondary);
            y += nh + 12;
        }
    };

    // A plain card around `body`: measured first, then drawn.
    auto plainCard = [&](auto&& body) {
        float top = y;
        size_t hitCount = hits_.size();
        bool d = draw;
        draw = false;
        body();
        float h = y - top;
        draw = d;
        hits_.resize(hitCount);
        y = top;
        card(top, h);
        body();
        y = top + h + 12;
    };

    section(0, S().settings, generalBody);
    section(1, S().personalization, personalBody);
    section(2, S().inputs, inputsBody);
    section(3, S().outputs, outputsBody);
    plainCard(updatesBody);
    return y;
}

void Flyout::LayoutFooter(float top, bool draw) {
    // Matches the Quick Settings footer: 48 tall, darker band, no divider,
    // a 16 px icon button centred 32 px from the right edge. The same spot
    // holds the gear in the mixer and the back arrow in Settings.
    if (draw) Fill({0, top, kWidth, top + kFooterHeight}, pal_.footer);
    Hit corner{view_ == View::Mixer ? HitKind::Gear : HitKind::Back, -1, -1,
               {kWidth - 50, top + 6, kWidth - 14, top + 42}};
    corner.footer = true;
    Button(corner, draw);
    if (draw) {
        if (view_ == View::Mixer)
            DrawGear(corner.rect, pal_.text);
        else
            DrawRotatedGlyph(kGlyphBack, 16, corner.rect, pal_.text, arrow_.Value());
    }

    if (view_ == View::Mixer) {
        float tw = TextWidth(S().soundSettings, fBody_.Get());
        Hit sound{HitKind::SoundSettings, -1, -1, {12, top + 8, 12 + tw + 24, top + 40}};
        sound.footer = true;
        Button(sound, draw);
        if (draw) Text(S().soundSettings, fBody_.Get(), sound.rect, pal_.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (draw) {
        Text(APP_NAME L" " APP_VERSION, fCaption_.Get(), {24, top, 220, top + kFooterHeight}, pal_.textTertiary);
    }
}

void Flyout::Render() {
    PERF_BEGIN(render);
    if (!EnsureResources() || panelW_ <= 0) return;
    Composition* c = comp_;
    UpdateShadowMask();

    // The panel is as tall as the content (sections mid-animation included),
    // up to the screen; beyond that the content scrolls. Whole pixels, so the
    // footer sits exactly on the bottom edge.
    winH_ = 100000.0f;
    float natural = Layout(false);
    int heightPx = std::max(1, (int)std::lround(std::min(natural, maxH_) * scale_));
    winH_ = heightPx / scale_;
    maxScroll_ = std::max(0.0f, natural - winH_);
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll_);

    // Content surface: as big as the panel can get, drawn only where it shows.
    int surfaceH = (int)std::lround(maxH_ * scale_) + 1;
    try {
        if (!c->surface) {
            c->surface = c->graphics.CreateDrawingSurface({(float)panelW_, (float)surfaceH},
                                                          winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                          winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
            auto brush = c->compositor.CreateSurfaceBrush(c->surface);
            brush.Stretch(wuc::CompositionStretch::None);
            brush.HorizontalAlignmentRatio(0);
            brush.VerticalAlignmentRatio(0);
            c->content.Brush(brush);
        } else if (c->surfaceW != panelW_ || c->surfaceH != surfaceH) {
            winrt::check_hresult(c->surface.as<abi::ICompositionDrawingSurfaceInterop>()->Resize({panelW_, surfaceH}));
        }
        if (c->surfaceW != panelW_ || c->surfaceH != surfaceH) c->surfaceFresh = true;
        c->surfaceW = panelW_;
        c->surfaceH = surfaceH;
    } catch (...) {
        return;
    }

    // A taller panel needs the input region first, so it is never clipped.
    panelH_ = heightPx;
    panelY_ = anchorTop_ ? panelEdge_ : panelEdge_ - heightPx;
    UpdateRegion(true);

    HRESULT hr;
    // The first draw after creating or resizing the surface must cover all of
    // it; after that, only the part the panel shows.
    RECT update = {0, 0, panelW_, c->surfaceFresh ? surfaceH : heightPx};
    auto dc = BeginSurfaceDraw(c->surface, update, scale_, &drawBase_, &hr);
    if (!dc) {
        if (DeviceLost(hr)) {
            c->device.Reset();
            InvalidateRect(hwnd_, nullptr, FALSE);  // draw again with a new device
        }
        return;
    }
    rt_ = dc;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // transparent target: no ClearType
    // Tint over the blurred backdrop, like WinUI's acrylic tint layer.
    Fill({0, 0, kWidth, winH_}, pal_.tint);
    Layout(true);
    DrawTooltip();
    Stroke({0, 0, kWidth, winH_}, pal_.border, kCornerRadius);
    rt_.Reset();
    EndSurfaceDraw(c->surface, dc.Get(), c->d3dContext.Get());
    c->surfaceFresh = false;
    // Same commit as the content just drawn.
    PlacePanel(heightPx);
    UpdateHexEdit();
    PERF_END(render, "Render");
}

D2D1_POINT_2F Flyout::ToPanel(int x, int y) const {
    return {(x - panelX_) / scale_, (y - panelY_) / scale_};
}

// ---------------------------------------------------------------------------
// Input

Flyout::Hit Flyout::HitTest(float x, float y) const {
    for (const Hit& h : hits_) {
        if (!h.footer && y >= contentBottom_) continue;  // content scrolled under the footer
        if (x >= h.rect.left && x < h.rect.right && y >= h.rect.top && y < h.rect.bottom) return h;
    }
    return Hit{};
}

void Flyout::SetSliderValue(const Hit& h, float x) {
    float v = std::clamp((x - h.trackX0) / (h.trackX1 - h.trackX0), 0.0f, 1.0f);
    v = std::round(v * 100.0f) / 100.0f;
    auto& devices = audio_->Devices();
    if (h.device < 0 || h.device >= (int)devices.size()) return;
    AudioDevice& d = devices[h.device];
    if (h.app < 0)
        audio_->SetDeviceVolume(d, v);
    else if (h.app < (int)d.apps.size())
        audio_->SetAppVolume(d.apps[h.app], v);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Flyout::AdjustSlider(const Hit& h, float delta) {
    auto& devices = audio_->Devices();
    if (h.device < 0 || h.device >= (int)devices.size()) return;
    AudioDevice& d = devices[h.device];
    if (h.app < 0) {
        audio_->SetDeviceVolume(d, std::clamp(std::round((d.volume + delta) * 100.0f) / 100.0f, 0.0f, 1.0f));
    } else if (h.app < (int)d.apps.size()) {
        AppEntry& a = d.apps[h.app];
        audio_->SetAppVolume(a, std::clamp(std::round((a.volume + delta) * 100.0f) / 100.0f, 0.0f, 1.0f));
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Flyout::IsExpanded(const AudioDevice& d) const {
    auto it = expanded_.find(d.id);
    if (it != expanded_.end()) return it->second;
    return d.isDefault || d.playing;  // inactive devices start collapsed
}

void Flyout::PickIcon(int device) {
    auto& devices = audio_->Devices();
    if (device < 0 || device >= (int)devices.size()) return;
    const std::wstring id = devices[device].id;
    auto it = settings_->deviceIcons.find(id);
    std::wstring current = it == settings_->deviceIcons.end() ? L"" : it->second;
    bool custom = !current.empty() && current != L"speakers" && current != L"headphones";

    enum { IdAuto = 1, IdSpeakers, IdHeadphones, IdCustom };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (current.empty() ? MF_CHECKED : 0), IdAuto, S().iconAuto);
    AppendMenuW(menu, MF_STRING | (current == L"speakers" ? MF_CHECKED : 0), IdSpeakers, S().iconSpeakers);
    AppendMenuW(menu, MF_STRING | (current == L"headphones" ? MF_CHECKED : 0), IdHeadphones, S().iconHeadphones);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (custom ? MF_CHECKED : 0), IdCustom, S().iconCustom);

    modal_ = true;
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(menu);

    std::wstring value = current;
    if (cmd == IdAuto) value.clear();
    if (cmd == IdSpeakers) value = L"speakers";
    if (cmd == IdHeadphones) value = L"headphones";
    if (cmd == IdCustom) {
        ComPtr<IFileOpenDialog> dlg;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
            COMDLG_FILTERSPEC filter = {S().iconFiles, L"*.ico;*.png;*.jpg;*.jpeg;*.bmp"};
            dlg->SetFileTypes(1, &filter);
            ComPtr<IShellItem> item;
            PWSTR path = nullptr;
            if (SUCCEEDED(dlg->Show(hwnd_)) && SUCCEEDED(dlg->GetResult(&item)) &&
                SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                // Keep our own copy so the icon survives the original being
                // moved or deleted.
                UINT32 hash = 2166136261u;
                for (wchar_t ch : id) hash = (hash ^ ch) * 16777619u;
                wchar_t name[64];
                swprintf_s(name, L"%08x-%llx", hash, (unsigned long long)GetTickCount64());
                std::wstring dest = AppDataDir(L"icons") + L"\\" + name + PathFindExtensionW(path);
                if (CopyFileW(path, dest.c_str(), FALSE)) value = dest;
                CoTaskMemFree(path);
            }
        }
        SetForegroundWindow(hwnd_);
    }
    modal_ = false;

    if (value != current) {
        if (custom) DeleteFileW(current.c_str());  // our copy of the previous custom icon
        if (value.empty())
            settings_->deviceIcons.erase(id);
        else
            settings_->deviceIcons[id] = value;
        settings_->Save();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Flyout::OnUpdateStateChanged() {
    if (visible_) Reposition(false);
}

void Flyout::Activate(const Hit& h) {
    auto& devices = audio_->Devices();
    switch (h.kind) {
        case HitKind::Mute:
            if (h.device >= 0 && h.device < (int)devices.size()) {
                AudioDevice& d = devices[h.device];
                if (h.app < 0)
                    audio_->SetDeviceMute(d, !d.muted);
                else if (h.app < (int)d.apps.size())
                    audio_->SetAppMute(d.apps[h.app], !d.apps[h.app].muted);
            }
            break;
        case HitKind::Collapse:
            if (LayoutAnimating()) break;  // one at a time: clicks meanwhile don't queue up
            if (h.device >= 0 && h.device < (int)devices.size()) {
                const AudioDevice& d = devices[h.device];
                bool wasExpanded = IsExpanded(d);
                bool willExpand = !wasExpanded;
                auto [it, inserted] = deviceAnim_.try_emplace(d.id);
                if (inserted) it->second.Set(wasExpanded ? 1.0f : 0.0f);
                it->second.Go(willExpand ? 1.0f : 0.0f, 250);
                expanded_[d.id] = willExpand;
                hover_ = Hit{};
                StartFramePump();
            }
            break;
        case HitKind::DeviceMenu:
            SetDeviceMenu(!deviceMenuOpen_);
            hover_ = Hit{};
            break;
        case HitKind::SelectDevice:
            if (h.device >= 0 && h.device < (int)devices.size() &&
                (devices[h.device].isDefault || audio_->SetDefaultDevice(devices[h.device].id))) {
                SetDeviceMenu(false);
                hover_ = Hit{};
            }
            break;
        case HitKind::Gear:
            GoToView(View::Settings);
            break;
        case HitKind::Back:
            GoToView(View::Mixer);
            break;
        case HitKind::Section:
            if (LayoutAnimating()) break;
            if (h.device >= 0 && h.device < kSections) {
                sectionOpen_[h.device] = !sectionOpen_[h.device];
                sectionAnim_[h.device].Go(sectionOpen_[h.device] ? 1.0f : 0.0f, 250);
                StartFramePump();
            }
            break;
        case HitKind::NameOption:
            settings_->deviceNameMode = (DWORD)h.device;
            settings_->Save();
            break;
        case HitKind::MeterOption:
            settings_->meterColor = (DWORD)h.device;
            settings_->Save();
            pal_.meter = MeterColorFor((Settings::MeterColor)settings_->meterColor);
            Reposition(false);  // the custom colour field adds a row
            if (h.device == Settings::MeterCustom && hexEdit_[0]) SetFocus(hexEdit_[0]);
            break;
        case HitKind::CloseRestore: {
            modal_ = true;
            int answer = MessageBoxW(hwnd_, S().closeConfirm, S().closeConfirmTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            modal_ = false;
            if (answer == IDYES && onCloseAndRestore) {
                Hide();
                onCloseAndRestore();
                return;
            }
            break;
        }
        case HitKind::MoveToDocuments:
            if (onMoveToDocuments) onMoveToDocuments();
            return;
        case HitKind::SoundSettings:
            Hide();
            ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            return;
        case HitKind::ToggleHideIcon:
            settings_->hideSystemVolumeIcon = !settings_->hideSystemVolumeIcon;
            settings_->Save();
            if (onHideIconChanged) onHideIconChanged();
            break;
        case HitKind::ToggleHideQs:
            settings_->hideQuickSettingsVolume = !settings_->hideQuickSettingsVolume;
            settings_->Save();
            if (onHideIconChanged) onHideIconChanged();
            break;
        case HitKind::ToggleUseSystemIcon:
            settings_->useSystemVolumeIcon = !settings_->useSystemVolumeIcon;
            settings_->Save();
            if (onHideIconChanged) onHideIconChanged();
            break;
        case HitKind::ToggleStartup:
            startup_ = !startup_;
            SetStartWithWindows(startup_);
            break;
        case HitKind::ToggleDevice:
            if (h.device >= 0 && h.device < (int)devices.size()) {
                const std::wstring& id = devices[h.device].id;
                settings_->SetDeviceHidden(id, !settings_->IsDeviceHidden(id));
                settings_->Save();
            }
            break;
        case HitKind::ToggleInput: {
            auto& inputs = audio_->Inputs();
            if (h.device >= 0 && h.device < (int)inputs.size()) {
                const std::wstring& id = inputs[h.device].id;
                settings_->SetDeviceHidden(id, !settings_->IsDeviceHidden(id));
                settings_->Save();
            }
            break;
        }
        case HitKind::IconPick:
            PickIcon(h.device);
            return;
        case HitKind::CheckUpdates:
            if (onCheckUpdates) onCheckUpdates();
            break;
        case HitKind::InstallUpdate:
            if (onInstallUpdate) onInstallUpdate();
            break;
        default:
            break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Settings gear: six pointed teeth and a small hole, outlined, like the
// animated gear in the Quick Settings footer (Segoe's E713 is chunkier). It
// turns a bit on hover and further while pressed.

static ComPtr<ID2D1PathGeometry> MakeGearGeometry(ID2D1Factory* f) {
    const int teeth = 6;
    const float outer = 7.2f, root = 4.9f;      // radii in DIPs, for a 16 DIP icon
    const float tipHalf = 10.0f, rootHalf = 21.0f;  // half-widths in degrees
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return nullptr;
    auto at = [](float r, float deg) {
        float a = deg * 3.14159265f / 180.0f;
        return D2D1::Point2F(r * std::cos(a), r * std::sin(a));
    };
    for (int i = 0; i < teeth; i++) {
        float c = -90.0f + i * 360.0f / teeth;  // first tooth points up
        D2D1_POINT_2F pts[] = {at(root, c - rootHalf), at(outer, c - tipHalf), at(outer, c + tipHalf), at(root, c + rootHalf)};
        if (i == 0)
            sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
        else
            sink->AddLine(pts[0]);
        sink->AddLines(pts + 1, 3);
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return geo;
}

// ---------------------------------------------------------------------------
// View changes: the footer icon turns first (gear: half a turn; back arrow:
// from pointing left to pointing down), then the new view slides in.

static const float kIconTurnMs = 260, kSwitchDelayMs = 90, kTransitionMs = 200;

void Flyout::GoToView(View v) {
    if (pending_ || v == view_) return;
    if (v == View::Settings)
        gear_.Go(180, kIconTurnMs);
    else
        arrow_.Go(-90, kIconTurnMs);
    pending_ = true;
    pendingView_ = v;
    pendingAt_ = NowMs() + kSwitchDelayMs;
    StartFramePump();
}

// Animations render frame after frame, one per DWM composition (DwmFlush in
// AnimFrame), so this runs at the monitor's refresh rate. (A WM_TIMER can't: its
// 15.6 ms granularity and low priority gave 30 fps with hitches.)
void Flyout::StartFramePump() {
    if (pumping_) return;
    pumping_ = true;
    QueueFrame();
}

// The next frame goes through the composition DispatcherQueue at low
// priority: the compositor commits each frame's changes from that queue, so
// frames posted back to back as window messages starved it and nothing
// reached the screen until the animation ended.
void Flyout::QueueFrame() {
    try {
        if (comp_->queue.DispatcherQueue().TryEnqueue(winrt::Windows::System::DispatcherQueuePriority::Low,
                                                      [this] { AnimFrame(); }))
            return;
    } catch (...) {
    }
    PostMessageW(hwnd_, WM_ANIM_FRAME, 0, 0);
}

void Flyout::AnimFrame() {
    if (!visible_) {
        pumping_ = false;
        return;
    }
    DwmFlush();  // one frame per composition
    if (pending_ && NowMs() >= pendingAt_) SwitchView(pendingView_);
    if (view_ == View::Mixer) audio_->UpdatePeaks();
    Render();
    ValidateRect(hwnd_, nullptr);
    if (Animating())
        QueueFrame();
    else
        pumping_ = false;  // UpdateHexEdit ran in Render and shows the field now
}

void Flyout::SwitchView(View v) {
    pending_ = false;
    view_ = v;
    scroll_ = 0;
    hover_ = pressed_ = Hit{};
    if (v == View::Settings) startup_ = GetStartWithWindows();
    gear_.Set(0);
    arrow_.Set(0);
    transitionDir_ = v == View::Settings ? 1 : -1;
    transition_.Set(0);
    transition_.Go(1, kTransitionMs);
    Reposition(false);
    StartFramePump();
}

bool Flyout::LayoutAnimating() const {
    for (const Anim& a : sectionAnim_)
        if (a.Running()) return true;
    for (const auto& [id, a] : deviceAnim_)
        if (a.Running()) return true;
    return menuAnim_.Running();
}

void Flyout::SetDeviceMenu(bool open) {
    if (deviceMenuOpen_ == open || LayoutAnimating()) return;
    deviceMenuOpen_ = open;
    menuAnim_.Go(open ? 1.0f : 0.0f, 200);
    hover_ = Hit{};
    StartFramePump();
}

bool Flyout::Animating() const {
    return pending_ || gear_.Running() || arrow_.Running() || transition_.Running() || LayoutAnimating();
}

void Flyout::DrawRotatedGlyph(wchar_t g, float size, const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float degrees) {
    D2D1_MATRIX_3X2_F old;
    rt_->GetTransform(&old);
    D2D1_POINT_2F center = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    rt_->SetTransform(D2D1::Matrix3x2F::Rotation(degrees, center) * old);
    Glyph(g, size, r, c);
    rt_->SetTransform(old);
}

// ---------------------------------------------------------------------------
// Level meter colour

D2D1_COLOR_F Flyout::MeterColorFor(Settings::MeterColor c) const {
    switch (c) {
        case Settings::MeterWhite:
            return Rgba(0xFFFFFF);
        case Settings::MeterGreen:
            return Rgba(0x3CC83C);  // the Windows 7 peak meter green
        case Settings::MeterCustom:
            return Rgba(settings_->meterCustomColor);
        default: {
            // The accent mixed towards white, so it stands out against the
            // filled part of the slider.
            float mix = pal_.dark ? 0.6f : 0.5f;
            const D2D1_COLOR_F& a = pal_.accent;
            return D2D1::ColorF(a.r + (1 - a.r) * mix, a.g + (1 - a.g) * mix, a.b + (1 - a.b) * mix, 1.0f);
        }
    }
}

// The custom colours are typed into real EDIT controls laid over the fields
// the settings view draws. Called after each paint.
void Flyout::UpdateHexEdit() {
    bool show = visible_ && view_ == View::Settings && hexVisible_ && !transition_.Running();
    int fontPx = (int)std::lround(14 * scale_);
    static int lastFontPx = 0;
    if (show && (!hexFont_ || lastFontPx != fontPx)) {
        if (hexFont_) DeleteObject(hexFont_);
        hexFont_ = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
        for (HWND e : hexEdit_)
            if (e) SendMessageW(e, WM_SETFONT, (WPARAM)hexFont_, TRUE);
        lastFontPx = fontPx;
    }
    for (int i = 0; i < 2; i++) {
        const D2D1_RECT_F& r = hexRect_[i];
        HWND& edit = hexEdit_[i];
        if (!show || r.top < 0 || r.bottom > contentBottom_) {
            if (edit && IsWindowVisible(edit)) ShowWindow(edit, SW_HIDE);
            continue;
        }
        if (!edit) {
            // Layered: the flyout has no redirection bitmap for a plain child to
            // draw into; a layered child gets its own surface.
            edit = CreateWindowExW(WS_EX_LAYERED, L"EDIT", nullptr, WS_CHILD | ES_AUTOHSCROLL | ES_UPPERCASE, 0, 0, 10, 10,
                                   hwnd_, (HMENU)(INT_PTR)(1 + i), GetModuleHandleW(nullptr), nullptr);
            SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA);
            SendMessageW(edit, EM_LIMITTEXT, 6, 0);
            SendMessageW(edit, WM_SETFONT, (WPARAM)hexFont_, FALSE);
            SetWindowLongPtrW(edit, GWLP_USERDATA, SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)HexEditProc));
            wchar_t text[8];
            swprintf_s(text, L"%06X", (i == 0 ? settings_->meterCustomColor : settings_->meterCustomBar) & 0xFFFFFF);
            SetWindowTextW(edit, text);
        }
        // Inside the drawn field, between the "#" and the swatch.
        int x = panelX_ + (int)std::lround((r.left + 24) * scale_);
        int h = (int)std::lround(fontPx * 1.4f);
        int y = panelY_ + (int)std::lround(((r.top + r.bottom) / 2) * scale_) - h / 2;
        int w = panelX_ + (int)std::lround((r.right - 32) * scale_) - x;
        SetWindowPos(edit, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// Hex fields: only hex digits get in. A pasted colour drops its "#" (or
// "0x") and, when it is a whole colour, replaces the field.
LRESULT CALLBACK Flyout::HexEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto base = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CHAR && wp >= 0x20 && !iswxdigit((wint_t)wp)) return 0;
    if (msg == WM_PASTE) {
        std::wstring text;
        if (OpenClipboard(hwnd)) {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                if (auto p = (const wchar_t*)GlobalLock(data)) {
                    text = p;
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }
        size_t i = text.find_first_not_of(L" \t\r\n");
        if (i == std::wstring::npos) return 0;
        if (text[i] == L'#') i++;
        else if (text.compare(i, 2, L"0x") == 0 || text.compare(i, 2, L"0X") == 0) i += 2;
        std::wstring hex;
        for (; i < text.size() && iswxdigit(text[i]) && hex.size() < 6; i++) hex += text[i];
        if (hex.empty()) return 0;
        if (hex.size() == 6) SendMessageW(hwnd, EM_SETSEL, 0, -1);
        SendMessageW(hwnd, EM_REPLACESEL, TRUE, (LPARAM)hex.c_str());
        return 0;
    }
    return CallWindowProcW(base, hwnd, msg, wp, lp);
}

ID2D1StrokeStyle* Flyout::RoundStroke() {
    if (!roundStroke_) {
        auto props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                 D2D1_LINE_JOIN_ROUND);
        d2d_->CreateStrokeStyle(props, nullptr, 0, &roundStroke_);
    }
    return roundStroke_.Get();
}

// "Change output" label under the switch icon of the output selector, shown
// after hovering the icon for a moment.
void Flyout::DrawTooltip() {
    if (!tipShown_ || view_ != View::Mixer) return;
    const wchar_t* label = S().changeOutput;
    float w = 0;
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(dwrite_->CreateTextLayout(label, (UINT32)wcslen(label), fCaption_.Get(), kWidth, 32, &layout))) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(layout->GetMetrics(&m))) w = std::ceil(m.widthIncludingTrailingWhitespace);
    }
    float left = std::clamp(switchIconRect_.left, 4.0f, kWidth - 4 - (w + 16));
    D2D1_RECT_F r = {left, switchIconRect_.bottom + 4, left + w + 16, switchIconRect_.bottom + 30};
    Fill(r, pal_.dark ? Rgba(0x2C2C2C) : Rgba(0xF9F9F9), 4);
    Stroke(r, pal_.dark ? Rgba(0x000000, 0.3f) : Rgba(0x000000, 0.12f), 4);
    Text(label, fCaption_.Get(), r, pal_.text, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Flyout::HideTooltip() {
    if (tipArmed_) KillTimer(hwnd_, kTipTimer);
    tipArmed_ = false;
    if (tipShown_) {
        tipShown_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// Expand/collapse chevron, centred in r: pointing down at open = 0 and up at
// open = 1. Instead of rotating (halfway it reads as an "L" or "<"), its tip
// moves through the middle, so in between it is a flatter chevron or a line.
void Flyout::DrawChevron(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float open) {
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    float s = 2.5f * (1 - 2 * std::clamp(open, 0.0f, 1.0f));  // tip offset: +down, -up
    brush_->SetColor(c);
    rt_->DrawLine({cx - 5, cy - s}, {cx, cy + s}, brush_.Get(), 1.0f, RoundStroke());
    rt_->DrawLine({cx, cy + s}, {cx + 5, cy - s}, brush_.Get(), 1.0f, RoundStroke());
}

void Flyout::DrawGear(const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    if (!gearGeo_) gearGeo_ = MakeGearGeometry(d2d_.Get());
    RoundStroke();
    if (!gearGeo_) return;
    D2D1_MATRIX_3X2_F old;
    rt_->GetTransform(&old);
    D2D1_POINT_2F center = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    rt_->SetTransform(D2D1::Matrix3x2F::Rotation(gear_.Value()) * D2D1::Matrix3x2F::Translation(center.x, center.y) * old);
    brush_->SetColor(c);
    rt_->DrawGeometry(gearGeo_.Get(), brush_.Get(), 1.0f, roundStroke_.Get());
    rt_->DrawEllipse(D2D1::Ellipse({0, 0}, 1.9f, 1.9f), brush_.Get(), 1.0f);
    rt_->SetTransform(old);
}

// ---------------------------------------------------------------------------
// Volume OSD

static const float kOsdWidth = 240.0f, kOsdHeight = 60.0f;
static const UINT_PTR kOsdHideTimer = 1;

void Flyout::ShowVolumeOsd(const AudioDevice& d) {
    osdName_ = DeviceName(d);
    osdVolume_ = d.volume;
    osdMuted_ = d.muted;

    if (!osd_) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = OsdProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"VolumeOne.Osd";
        RegisterClassExW(&wc);
        osd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, APP_NAME,
                               WS_POPUP, 0, 0, 10, 10, owner_, nullptr, wc.hInstance, this);
        if (!osd_) return;
        SetWindowLongPtrW(osd_, GWLP_USERDATA, (LONG_PTR)this);
    }
    // Same look as the flyout.
    BOOL dark = pal_.dark;
    DwmSetWindowAttribute(osd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(osd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_TRANSIENTWINDOW;
    DwmSetWindowAttribute(osd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(osd_, &margins);

    // Centred over the cursor (it's on the taskbar icon), above the taskbar.
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    osdScale_ = dpiX / 96.0f;
    int w = (int)std::lround(kOsdWidth * osdScale_), h = (int)std::lround(kOsdHeight * osdScale_);
    int m = (int)std::lround(12 * osdScale_);
    const RECT& work = mi.rcWork;
    int x = std::clamp((int)pt.x - w / 2, (int)work.left + m, (int)work.right - m - w);
    int y = work.bottom - m - h;
    if (work.top > mi.rcMonitor.top) y = work.top + m;  // taskbar at the top
    SetWindowPos(osd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    if (osdRt_) {
        osdRt_->SetDpi(96.0f * osdScale_, 96.0f * osdScale_);
        osdRt_->Resize(D2D1::SizeU(w, h));
    }
    RenderOsd();
    SetTimer(osd_, kOsdHideTimer, 1500, nullptr);
}

void Flyout::RenderOsd() {
    if (!osd_ || !EnsureResources()) return;
    if (!osdRt_) {
        RECT rc;
        GetClientRect(osd_, &rc);
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f * osdScale_, 96.0f * osdScale_);
        if (FAILED(d2d_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(osd_, D2D1::SizeU(rc.right, rc.bottom)),
                                                &osdRt_)))
            return;
        osdRt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        osdRt_->CreateSolidColorBrush(pal_.text, &osdBrush_);
    }
    // The drawing helpers use rt_/brush_: point them at the OSD meanwhile.
    ComPtr<ID2D1RenderTarget> flyoutTarget = rt_;
    rt_ = osdRt_;
    std::swap(brush_, osdBrush_);
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    Fill({0, 0, kOsdWidth, kOsdHeight}, pal_.tint);
    Text(osdName_, fCaption_.Get(), {16, 8, kOsdWidth - 16, 26}, pal_.textSecondary);
    VolumeIcon(osdVolume_, osdMuted_, {8, 28, 40, 52});
    const float x0 = 48, x1 = kOsdWidth - 52, cy = 40;
    Fill({x0, cy - 2, x1, cy + 2}, pal_.trackOff, 2);
    float tx = x0 + std::clamp(osdVolume_, 0.0f, 1.0f) * (x1 - x0);
    if (tx > x0 + 1) Fill({x0, cy - 2, tx, cy + 2}, osdMuted_ ? pal_.textTertiary : pal_.accent, 2);
    Text(std::to_wstring((int)std::lround(osdVolume_ * 100.0f)), fBody_.Get(), {x1 + 8, 28, kOsdWidth - 16, 52}, pal_.text,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
    HRESULT hr = rt_->EndDraw();
    rt_ = flyoutTarget;
    std::swap(brush_, osdBrush_);
    if (hr == D2DERR_RECREATE_TARGET) {
        osdBrush_.Reset();
        osdRt_.Reset();
    }
}

LRESULT CALLBACK Flyout::OsdProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto self = (Flyout*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;  // clicks go through to the taskbar
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (self) self->RenderOsd();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kOsdHideTimer) {
                KillTimer(hwnd, wp);
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Flyout::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto self = (Flyout*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    auto self = (Flyout*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    return self ? self->WndProc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Flyout::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && !modal_) Hide();
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            Render();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_COMMAND:  // custom colour typed in a hex field
            if (HIWORD(wp) == EN_CHANGE && lp && ((HWND)lp == hexEdit_[0] || (HWND)lp == hexEdit_[1])) {
                wchar_t text[16] = {};
                GetWindowTextW((HWND)lp, text, ARRAYSIZE(text));
                wchar_t* end = nullptr;
                unsigned long v = wcstoul(text, &end, 16);
                if (wcslen(text) == 6 && end && *end == 0) {
                    ((HWND)lp == hexEdit_[0] ? settings_->meterCustomColor : settings_->meterCustomBar) = v;
                    settings_->Save();
                    pal_.meter = MeterColorFor((Settings::MeterColor)settings_->meterColor);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            return 0;

        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            COLORREF bg = pal_.dark ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xF9, 0xF9, 0xF9);
            SetTextColor(dc, pal_.dark ? RGB(255, 255, 255) : RGB(0x1A, 0x1A, 0x1A));
            SetBkColor(dc, bg);
            if (hexBrush_) DeleteObject(hexBrush_);
            hexBrush_ = CreateSolidBrush(bg);
            return (LRESULT)hexBrush_;
        }

        case WM_ANIM_FRAME:  // fallback when the DispatcherQueue can't take the frame
            AnimFrame();
            return 0;

        case WM_WARMUP:
            // The first layout of a view is slow (DirectWrite loads line
            // breaking, fonts...): do the other view now, while idle, instead
            // of when the user switches to it.
            if (!warmed_ && visible_ && !pumping_) {
                warmed_ = true;
                View current = view_;
                float scroll = scroll_;
                view_ = current == View::Mixer ? View::Settings : View::Mixer;
                Layout(false);
                view_ = current;
                scroll_ = scroll;
                Layout(false);  // hit areas back to the current view
            }
            return 0;

        case WM_OUTSIDE_CLICK:
            if (!modal_) Hide();  // menus and the file dialog are outside the flyout too
            return 0;

        case WM_TIMER:
            if (wp == kTipTimer) {
                KillTimer(hwnd_, kTipTimer);
                tipShown_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wp == kRegionTimer) {
                KillTimer(hwnd_, kRegionTimer);
                UpdateRegion(false);
                return 0;
            }
            // Level meters; animations have their own frame pump.
            if (wp == kMeterTimer && visible_ && !pumping_) {
                // The mixer's meters, or the meter preview in Personalization.
                if (view_ == View::Mixer ? audio_->UpdatePeaks() : previewVisible_ && sectionOpen_[1])
                    InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;

        case WM_DPICHANGED:
            scale_ = HIWORD(wp) / 96.0f;
            icons_.clear();
            if (visible_) Reposition(false);
            return 0;

        case WM_MOUSEMOVE: {
            auto [x, y] = ToPanel(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (!tracking_) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            if (dragging_) {
                SetSliderValue(pressed_, x);
                return 0;
            }
            Hit h = HitTest(x, y);
            if (!(h == hover_)) {
                hover_ = h;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            bool overSwitch = h.kind == HitKind::DeviceMenu && x >= switchIconRect_.left && x < switchIconRect_.right &&
                              y >= switchIconRect_.top && y < switchIconRect_.bottom;
            if (!overSwitch) {
                HideTooltip();
                tipBlocked_ = false;
            } else if (!tipArmed_ && !tipBlocked_) {
                tipArmed_ = true;
                SetTimer(hwnd_, kTipTimer, 600, nullptr);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            HideTooltip();
            if (!dragging_) {
                hover_ = Hit{};
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            auto [x, y] = ToPanel(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            SetCapture(hwnd_);
            pressed_ = HitTest(x, y);
            HideTooltip();
            tipBlocked_ = true;  // not again until the mouse leaves the icon
            if (deviceMenuOpen_ && pressed_.kind != HitKind::DeviceMenu && pressed_.kind != HitKind::SelectDevice) {
                SetDeviceMenu(false);
                pressed_ = Hit{};
            }
            if (pressed_.kind == HitKind::Slider) {
                dragging_ = true;
                SetSliderValue(pressed_, x);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            auto [x, y] = ToPanel(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            Hit h = HitTest(x, y);
            Hit p = pressed_;
            bool wasDragging = dragging_;
            dragging_ = false;
            pressed_ = Hit{};
            if (GetCapture() == hwnd_) ReleaseCapture();
            hover_ = h;
            if (!wasDragging && p.kind != HitKind::None && h == p) Activate(h);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            dragging_ = false;
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            float delta = GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
            auto [px, py] = ToPanel(pt.x, pt.y);
            Hit h = HitTest(px, py);
            if (h.kind == HitKind::Slider || (h.kind == HitKind::Mute && view_ == View::Mixer)) {
                AdjustSlider(h, delta * 0.02f);
            } else if (maxScroll_ > 0) {
                scroll_ = std::clamp(scroll_ - delta * 48.0f, 0.0f, maxScroll_);
                hover_ = Hit{};
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                if (view_ == View::Settings)
                    GoToView(View::Mixer);
                else
                    Hide();
            }
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
