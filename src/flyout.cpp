#include "flyout.h"
#include "strings.h"

#include <shellscalingapi.h>
#include <shlobj.h>
#include <cmath>

static const float kWidth = 360.0f;
static const float kFooterHeight = 52.0f;
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
};

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

static bool IsFooterHit(int kind, int gear, int sound, int exit) { return kind == gear || kind == sound || kind == exit; }

// ---------------------------------------------------------------------------

bool Flyout::Create(HINSTANCE inst, HWND owner, AudioManager* audio, Settings* settings) {
    owner_ = owner;
    audio_ = audio;
    settings_ = settings;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)dwrite_.GetAddressOf())))
        return false;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VolumeOne.Flyout";
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, APP_NAME, WS_POPUP, 0, 0, 10, 10,
                            owner, nullptr, inst, this);
    if (!hwnd_) return false;

    LoadPalette();
    ApplyWindowStyle();
    return true;
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
        p.footer = Rgba(0x000000, 0.12f);
        p.trackOff = Rgba(0xFFFFFF, 0.545f);
        p.thumb = Rgba(0x454545);
        p.thumbStroke = Rgba(0xFFFFFF, 0.094f);
        p.onAccent = Rgba(0x000000);
        p.toggleOffFill = Rgba(0x000000, 0.10f);
        p.tint = Rgba(0x202020, 0.82f);
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
    }
    pal_ = p;
}

void Flyout::ApplyWindowStyle() {
    BOOL dark = pal_.dark;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_TRANSIENTWINDOW;  // acrylic, like the Quick Settings flyout
    DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
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
    if (!rt_) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f * scale_,
            96.0f * scale_);
        auto hprops = D2D1::HwndRenderTargetProperties(
            hwnd_, D2D1::SizeU(std::max<LONG>(rc.right, 1), std::max<LONG>(rc.bottom, 1)));
        if (FAILED(d2d_->CreateHwndRenderTarget(props, hprops, &rt_))) return false;
        // ClearType needs an opaque background; we draw on the acrylic backdrop.
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt_->CreateSolidColorBrush(pal_.text, &brush_);
        icons_.clear();
    }
    return true;
}

void Flyout::DiscardTarget() {
    icons_.clear();
    brush_.Reset();
    rt_.Reset();
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
        return;
    }
    view_ = settingsView ? View::Settings : View::Mixer;
    scroll_ = 0;
    hover_ = pressed_ = Hit{};
    startup_ = GetStartWithWindows();
    audio_->SetTrackSessions(true);

    Reposition(false);
    RECT final;
    GetWindowRect(hwnd_, &final);
    int dist = (int)std::lround(24 * scale_);
    SetWindowPos(hwnd_, HWND_TOPMOST, final.left + slideDx_ * dist, final.top + slideDy_ * dist, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);

    visible_ = true;
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
}

void Flyout::Hide() {
    if (!visible_) return;
    visible_ = false;
    dragging_ = false;
    lastHideTick_ = GetTickCount64();
    ShowWindow(hwnd_, SW_HIDE);
    if (GetCapture() == hwnd_) ReleaseCapture();
    audio_->SetTrackSessions(false);
}

void Flyout::Reposition(bool) {
    RECT anchor{};
    NOTIFYICONIDENTIFIER nii = {sizeof(nii)};
    nii.hWnd = owner_;
    nii.uID = 1;
    if (FAILED(Shell_NotifyIconGetRect(&nii, &anchor))) {
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
        if (rt_) rt_->SetDpi((float)dpiX, (float)dpiX);
        icons_.clear();
    }
    if (!EnsureResources()) return;

    const RECT& work = mi.rcWork;
    const RECT& full = mi.rcMonitor;
    winH_ = 100000.0f;
    float natural = Layout(false);
    float maxH = (work.bottom - work.top) / scale_ - 24.0f;
    winH_ = std::min(natural, maxH);
    maxScroll_ = std::max(0.0f, natural - winH_);
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll_);

    int w = (int)std::lround(kWidth * scale_);
    int h = (int)std::lround(winH_ * scale_);
    int m = (int)std::lround(12 * scale_);
    int x = work.right - m - w;
    int y = work.bottom - m - h;
    slideDx_ = 0;
    slideDy_ = 1;
    if (work.top > full.top) {  // taskbar at the top
        y = work.top + m;
        slideDy_ = -1;
    } else if (work.left > full.left) {  // taskbar on the left
        x = work.left + m;
        slideDx_ = -1;
        slideDy_ = 0;
    } else if (work.right < full.right) {  // taskbar on the right
        slideDx_ = 1;
        slideDy_ = 0;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    if (rt_) rt_->Resize(D2D1::SizeU(w, h));
    InvalidateRect(hwnd_, nullptr, FALSE);
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
        if (!settings_->IsDeviceHidden(devices[i].id)) result.push_back(i);
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

void Flyout::Button(const Hit& h, bool draw, float radius) {
    hits_.push_back(h);
    if (draw && hover_ == h) Fill(h.rect, pressed_ == h ? pal_.subtlePressed : pal_.subtleHover, radius);
}

void Flyout::Slider(const Hit& h, float cy, float value, bool muted, bool draw) {
    hits_.push_back(h);
    if (!draw) return;
    float x0 = h.trackX0, x1 = h.trackX1;
    float tx = x0 + std::clamp(value, 0.0f, 1.0f) * (x1 - x0);
    D2D1_COLOR_F fill = muted ? pal_.textTertiary : pal_.accent;

    Fill({x0, cy - 2, x1, cy + 2}, pal_.trackOff, 2);
    if (tx > x0 + 1) Fill({x0, cy - 2, tx, cy + 2}, fill, 2);

    // Thumb: neutral outer disc with an accent dot that grows on hover and
    // shrinks while pressed, like the WinUI slider.
    brush_->SetColor(pal_.thumb);
    rt_->FillEllipse(D2D1::Ellipse({tx, cy}, 10, 10), brush_.Get());
    brush_->SetColor(pal_.thumbStroke);
    rt_->DrawEllipse(D2D1::Ellipse({tx, cy}, 9.5f, 9.5f), brush_.Get(), 1);
    float r = 6;
    if (pressed_ == h && dragging_) r = 5;
    else if (hover_ == h) r = 7;
    brush_->SetColor(fill);
    rt_->FillEllipse(D2D1::Ellipse({tx, cy}, r, r), brush_.Get());
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

// ---------------------------------------------------------------------------
// Layout. The same code measures (draw == false) and paints, so hit testing
// always matches what is on screen.

float Flyout::Layout(bool draw) {
    hits_.clear();
    contentBottom_ = winH_ - kFooterHeight;
    if (draw) rt_->PushAxisAlignedClip({0, 0, kWidth, contentBottom_}, D2D1_ANTIALIAS_MODE_ALIASED);
    float y0 = -scroll_;
    float end = view_ == View::Mixer ? LayoutMixer(y0, draw) : LayoutSettings(y0, draw);
    if (draw) rt_->PopAxisAlignedClip();
    LayoutFooter(contentBottom_, draw);
    return (end - y0) + kFooterHeight;
}

float Flyout::LayoutMixer(float y, bool draw) {
    const float x0 = 12, x1 = kWidth - 12;
    auto& devices = audio_->Devices();
    auto visible = VisibleDevices();
    y += 12;

    if (visible.empty()) {
        if (draw) Text(S().noDevices, fBody_.Get(), {x0, y, x1, y + 72}, pal_.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        return y + 72 + 12;
    }

    for (int di : visible) {
        AudioDevice& d = devices[di];
        float top = y;
        float h = 12 + 24 + 40 + (d.apps.empty() ? 0 : 8 + 48.0f * d.apps.size()) + 8;
        if (draw) {
            Fill({x0, top, x1, top + h}, pal_.card, 8);
            Stroke({x0, top, x1, top + h}, pal_.cardStroke, 8);
        }

        float cy = top + 12;
        if (draw) {
            Glyph(d.formFactor == FormFactor::Headphones ? kGlyphHeadphones : kGlyphSpeakers, 16,
                  {x0 + 12, cy, x0 + 40, cy + 20}, pal_.textSecondary);
            Text(d.name, fTitle_.Get(), {x0 + 46, cy, x1 - 14, cy + 20}, pal_.text);
        }
        cy += 24;

        // Device master volume.
        Hit mute{HitKind::Mute, di, -1, {x0 + 6, cy, x0 + 46, cy + 40}};
        Button(mute, draw);
        if (draw) Glyph(VolumeGlyph(d.volume, d.muted), 16, mute.rect, pal_.text);
        Hit slider{HitKind::Slider, di, -1, {x0 + 50, cy, x1 - 48, cy + 40}, x0 + 62, x1 - 60};
        Slider(slider, cy + 20, d.volume, d.muted, draw);
        if (draw) Text(Percent(d.volume), fBody_.Get(), {x1 - 52, cy, x1 - 14, cy + 40}, pal_.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
        cy += 40;

        if (!d.apps.empty()) {
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
                Slider(aslider, cy + 32, a.volume, a.muted, draw);
                if (draw) Text(Percent(a.volume), fBody_.Get(), {x1 - 52, cy + 16, x1 - 14, cy + 48}, pal_.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
                cy += 48;
            }
        }
        y = top + h + 8;
    }
    return y + 4;
}

float Flyout::LayoutSettings(float y, bool draw) {
    const float x0 = 12, x1 = kWidth - 12;

    Hit back{HitKind::Back, -1, -1, {x0, y + 8, x0 + 40, y + 44}};
    Button(back, draw);
    if (draw) {
        Glyph(kGlyphBack, 16, back.rect, pal_.text);
        Text(S().settings, fTitle_.Get(), {x0 + 50, y + 8, x1, y + 44}, pal_.text);
    }
    y += 52;

    // General toggles.
    struct Row { HitKind kind; const wchar_t* label; const wchar_t* desc; bool on; };
    Row rows[] = {
        {HitKind::ToggleHideIcon, S().hideSystemIcon, S().hideSystemIconDesc, settings_->hideSystemVolumeIcon},
        {HitKind::ToggleStartup, S().startWithWindows, S().startWithWindowsDesc, startup_},
    };
    const float textX = x0 + 16, textR = x1 - 16 - 40 - 16;
    float heights[2], cardH = 0;
    for (int i = 0; i < 2; i++) {
        heights[i] = 12 + 20 + TextHeight(rows[i].desc, fDesc_.Get(), textR - textX) + 12;
        cardH += heights[i];
    }
    if (draw) {
        Fill({x0, y, x1, y + cardH}, pal_.card, 8);
        Stroke({x0, y, x1, y + cardH}, pal_.cardStroke, 8);
    }
    float ry = y;
    for (int i = 0; i < 2; i++) {
        Hit h{rows[i].kind, -1, -1, {x0, ry, x1, ry + heights[i]}};
        ToggleSwitch(h, x1 - 16 - 40, ry + heights[i] / 2, rows[i].on, draw);
        if (draw) {
            Text(rows[i].label, fBody_.Get(), {textX, ry + 12, textR, ry + 32}, pal_.text);
            Text(rows[i].desc, fDesc_.Get(), {textX, ry + 32, textR, ry + heights[i]}, pal_.textSecondary);
            if (i > 0) Fill({x0 + 1, ry, x1 - 1, ry + 1}, pal_.divider);
        }
        ry += heights[i];
    }
    y += cardH + 20;

    // Output devices.
    if (draw) Text(S().outputDevices, fTitle_.Get(), {x0 + 4, y, x1, y + 20}, pal_.text);
    y += 22;
    float descH = TextHeight(S().outputDevicesDesc, fDesc_.Get(), x1 - x0 - 8);
    if (draw) Text(S().outputDevicesDesc, fDesc_.Get(), {x0 + 4, y, x1 - 4, y + descH}, pal_.textSecondary);
    y += descH + 8;

    auto& devices = audio_->Devices();
    float devH = 48.0f * devices.size();
    if (draw && !devices.empty()) {
        Fill({x0, y, x1, y + devH}, pal_.card, 8);
        Stroke({x0, y, x1, y + devH}, pal_.cardStroke, 8);
    }
    for (int i = 0; i < (int)devices.size(); i++) {
        const AudioDevice& d = devices[i];
        float top = y + 48.0f * i;
        bool shown = !settings_->IsDeviceHidden(d.id);
        Hit h{HitKind::ToggleDevice, i, -1, {x0, top, x1, top + 48}};
        ToggleSwitch(h, x1 - 16 - 40, top + 24, shown, draw);
        if (draw) {
            if (i > 0) Fill({x0 + 1, top, x1 - 1, top + 1}, pal_.divider);
            D2D1_COLOR_F c = shown ? pal_.text : pal_.textTertiary;
            Glyph(d.formFactor == FormFactor::Headphones ? kGlyphHeadphones : kGlyphSpeakers, 16,
                  {x0 + 10, top, x0 + 42, top + 48}, c);
            Text(d.name, fBody_.Get(), {x0 + 48, top, x1 - 72, top + 48}, c);
        }
    }
    y += devH + 12;
    return y;
}

void Flyout::LayoutFooter(float top, bool draw) {
    if (draw) {
        Fill({0, top, kWidth, top + kFooterHeight}, pal_.footer);
        Fill({0, top, kWidth, top + 1}, pal_.divider);
    }
    if (view_ == View::Mixer) {
        float tw = TextWidth(S().soundSettings, fBody_.Get());
        Hit sound{HitKind::SoundSettings, -1, -1, {12, top + 10, 12 + tw + 24, top + 42}};
        Button(sound, draw);
        if (draw) Text(S().soundSettings, fBody_.Get(), sound.rect, pal_.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        Hit gear{HitKind::Gear, -1, -1, {kWidth - 12 - 40, top + 8, kWidth - 12, top + 44}};
        Button(gear, draw);
        if (draw) Glyph(kGlyphSettings, 16, gear.rect, pal_.text);
    } else {
        if (draw) Text(APP_NAME L" " APP_VERSION, fCaption_.Get(), {20, top, 220, top + kFooterHeight}, pal_.textTertiary);
        float tw = TextWidth(S().exit, fBody_.Get());
        Hit exit{HitKind::Exit, -1, -1, {kWidth - 12 - tw - 32, top + 10, kWidth - 12, top + 42}};
        Button(exit, draw);
        if (draw) Text(S().exit, fBody_.Get(), exit.rect, pal_.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void Flyout::Render() {
    if (!EnsureResources()) return;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));  // transparent: the DWM backdrop shows through
    // The raw DWM acrylic lets a lot of what is behind show through; tint it
    // to match Start and Quick Settings.
    Fill({0, 0, kWidth, winH_}, pal_.tint);
    Layout(true);
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) DiscardTarget();
}

// ---------------------------------------------------------------------------
// Input

Flyout::Hit Flyout::HitTest(float x, float y) const {
    for (const Hit& h : hits_) {
        bool footer = IsFooterHit((int)h.kind, (int)HitKind::Gear, (int)HitKind::SoundSettings, (int)HitKind::Exit);
        if (!footer && y >= contentBottom_) continue;
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
        case HitKind::Gear:
            view_ = View::Settings;
            scroll_ = 0;
            startup_ = GetStartWithWindows();
            Reposition(false);
            break;
        case HitKind::Back:
            view_ = View::Mixer;
            scroll_ = 0;
            Reposition(false);
            break;
        case HitKind::SoundSettings:
            Hide();
            ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            return;
        case HitKind::ToggleHideIcon:
            settings_->hideSystemVolumeIcon = !settings_->hideSystemVolumeIcon;
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
        case HitKind::Exit:
            if (onExit) onExit();
            return;
        default:
            break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
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
            if (LOWORD(wp) == WA_INACTIVE) Hide();
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

        case WM_SIZE:
            if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            return 0;

        case WM_DPICHANGED:
            scale_ = HIWORD(wp) / 96.0f;
            if (rt_) rt_->SetDpi((float)HIWORD(wp), (float)HIWORD(wp));
            icons_.clear();
            if (visible_) Reposition(false);
            return 0;

        case WM_MOUSEMOVE: {
            float x = GET_X_LPARAM(lp) / scale_, y = GET_Y_LPARAM(lp) / scale_;
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
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (!dragging_) {
                hover_ = Hit{};
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            float x = GET_X_LPARAM(lp) / scale_, y = GET_Y_LPARAM(lp) / scale_;
            SetCapture(hwnd_);
            pressed_ = HitTest(x, y);
            if (pressed_.kind == HitKind::Slider) {
                dragging_ = true;
                SetSliderValue(pressed_, x);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            float x = GET_X_LPARAM(lp) / scale_, y = GET_Y_LPARAM(lp) / scale_;
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
            Hit h = HitTest(pt.x / scale_, pt.y / scale_);
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
                    Activate(Hit{HitKind::Back});
                else
                    Hide();
            }
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
