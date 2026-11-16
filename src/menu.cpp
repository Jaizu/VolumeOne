#include "menu.h"

#include <d2d1.h>
#include <dwrite.h>
#include <shellscalingapi.h>

#include <cmath>
#include <memory>

namespace {

// Layout in DIPs.
const float kPad = 4;        // above the first row and below the last
const float kInset = 4;      // row highlight inset from the sides
const float kItemH = 26;
const float kSepH = 9;
const float kIconX = 12;
const float kTextX = 40;
const float kTextRight = 16;
const float kArrowW = 24;    // room for the submenu chevron
const float kMinWidth = 160;
const float kRadius = 4;

const wchar_t kClass[] = L"VolumeOne.Menu";
const UINT_PTR kSubTimer = 1;

D2D1_COLOR_F Rgba(UINT rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

struct Level {
    HWND hwnd = nullptr;
    const std::vector<MenuItem>* items = nullptr;
    std::vector<float> tops;  // each row's top
    float width = 0, height = 0;
    int hot = -1;   // row under the mouse or picked with the keyboard
    int open = -1;  // row whose submenu is open
    bool tracking = false;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::vector<ComPtr<ID2D1Bitmap>> icons;
};

struct Session {
    std::vector<std::unique_ptr<Level>> levels;  // [0] is the menu, then open submenus
    HMONITOR monitor = nullptr;
    UINT dpi = 96;
    float scale = 1;
    bool light = false;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> text, current, glyph;
    // Colors, as in the flyout's palette.
    D2D1_COLOR_F tint, fg, arrow, hover, highlight, divider;
    UINT result = 0;
    bool done = false;
};
Session* g_s = nullptr;

bool Selectable(const MenuItem& it) { return !it.separator; }

int FindLevel(HWND hwnd) {
    for (size_t i = 0; i < g_s->levels.size(); i++)
        if (g_s->levels[i]->hwnd == hwnd) return (int)i;
    return -1;
}

int HitTest(const Level& l, float y) {
    for (size_t i = 0; i < l.items->size(); i++) {
        const MenuItem& it = (*l.items)[i];
        float h = it.separator ? kSepH : kItemH;
        if (y >= l.tops[i] && y < l.tops[i] + h) return Selectable(it) ? (int)i : -1;
    }
    return -1;
}

void Redraw(Level& l) { InvalidateRect(l.hwnd, nullptr, FALSE); }

// Closes submenus from level n on.
void CloseFrom(size_t n) {
    while (g_s->levels.size() > n) {
        DestroyWindow(g_s->levels.back()->hwnd);
        g_s->levels.pop_back();
    }
    if (n > 0 && n <= g_s->levels.size()) {
        Level& parent = *g_s->levels[n - 1];
        parent.open = -1;
        Redraw(parent);
    }
}

void Render(Level& l) {
    Session& s = *g_s;
    if (!l.rt) {
        RECT rc;
        GetClientRect(l.hwnd, &rc);
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            (float)s.dpi, (float)s.dpi);
        if (FAILED(s.d2d->CreateHwndRenderTarget(
                props, D2D1::HwndRenderTargetProperties(l.hwnd, D2D1::SizeU(rc.right, rc.bottom)), &l.rt)))
            return;
        l.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // over acrylic: no ClearType
        l.rt->CreateSolidColorBrush(s.fg, &l.brush);
        l.icons.assign(l.items->size(), nullptr);
        for (size_t i = 0; i < l.items->size(); i++) {
            DIBSECTION ds;
            HBITMAP bmp = (*l.items)[i].icon;
            if (!bmp || GetObjectW(bmp, sizeof(ds), &ds) != sizeof(ds) || !ds.dsBm.bmBits) continue;
            auto bp = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)s.dpi, (float)s.dpi);
            l.rt->CreateBitmap(D2D1::SizeU(ds.dsBm.bmWidth, std::abs(ds.dsBm.bmHeight)), ds.dsBm.bmBits,
                               ds.dsBm.bmWidthBytes, bp, &l.icons[i]);
        }
    }

    auto fill = [&](const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius) {
        l.brush->SetColor(c);
        if (radius > 0)
            l.rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), l.brush.Get());
        else
            l.rt->FillRectangle(r, l.brush.Get());
    };
    auto snap = [&](float v) { return std::round(v * s.scale) / s.scale; };

    l.rt->BeginDraw();
    l.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    fill({0, 0, l.width, l.height}, s.tint, 0);
    for (size_t i = 0; i < l.items->size(); i++) {
        const MenuItem& it = (*l.items)[i];
        float top = l.tops[i];
        if (it.separator) {
            float y = snap(top + kSepH / 2);
            fill({0, y, l.width, y + 1 / s.scale}, s.divider, 0);
            continue;
        }
        D2D1_RECT_F row = {kInset, top + 1, l.width - kInset, top + kItemH - 1};
        if (it.current) fill(row, s.highlight, kRadius);
        if ((int)i == l.hot || (int)i == l.open) fill(row, s.hover, kRadius);

        if (l.icons[i]) {
            D2D1_SIZE_F sz = l.icons[i]->GetSize();
            float y = snap(top + (kItemH - sz.height) / 2);
            l.rt->DrawBitmap(l.icons[i].Get(), {kIconX, y, kIconX + sz.width, y + sz.height});
        }
        float right = l.width - kTextRight - (it.sub.empty() ? 0 : kArrowW);
        l.brush->SetColor(s.fg);
        l.rt->DrawTextW(it.text.c_str(), (UINT32)it.text.size(), it.current ? s.current.Get() : s.text.Get(),
                        {kTextX, top, right, top + kItemH}, l.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (!it.sub.empty()) {
            const wchar_t chevron = 0xE76C;
            l.brush->SetColor(s.arrow);
            l.rt->DrawTextW(&chevron, 1, s.glyph.Get(), {l.width - kTextRight - kArrowW, top, l.width - kTextRight, top + kItemH},
                            l.brush.Get());
        }
    }
    if (l.rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        l.icons.clear();
        l.brush.Reset();
        l.rt.Reset();
    }
}

float TextWidth(const std::wstring& text, IDWriteTextFormat* fmt) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(g_s->dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 10000, 100, &layout))) return 0;
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

LRESULT CALLBACK MenuProc(HWND, UINT, WPARAM, LPARAM);

// Opens `items` as level `parent + 1`: the menu itself (parent -1), at
// x, y, or the submenu of the parent's row `row`, beside it.
void OpenLevel(const std::vector<MenuItem>& items, int parent, int row, int x, int y, bool rightAlign) {
    Session& s = *g_s;
    auto l = std::make_unique<Level>();
    l->items = &items;
    float width = kMinWidth, top = kPad;
    for (const MenuItem& it : items) {
        l->tops.push_back(top);
        top += it.separator ? kSepH : kItemH;
        if (!it.separator)
            width = std::max(width, kTextX + TextWidth(it.text, it.current ? s.current.Get() : s.text.Get()) + kTextRight +
                                        (it.sub.empty() ? 0 : kArrowW));
    }
    l->width = std::ceil(width);
    l->height = top + kPad;
    int w = (int)std::ceil(l->width * s.scale), h = (int)std::ceil(l->height * s.scale);

    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(s.monitor, &mi);
    const RECT& work = mi.rcWork;
    if (parent < 0) {
        x = rightAlign ? x - w : x;
        y = y - h;
        if (y < work.top) y += h;  // no room above: open downwards
    } else {
        // Beside the parent row, overlapping its edge a little; on the
        // other side if there's no room.
        Level& p = *s.levels[parent];
        RECT pr;
        GetWindowRect(p.hwnd, &pr);
        int overlap = (int)std::lround(2 * s.scale);
        x = pr.right - overlap;
        if (x + w > work.right) x = pr.left - w + overlap;
        y = pr.top + (int)std::lround((p.tops[row] - kPad) * s.scale);
    }
    x = std::clamp(x, (int)work.left, std::max((int)work.left, (int)work.right - w));
    y = std::clamp(y, (int)work.top, std::max((int)work.top, (int)work.bottom - h));

    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | (parent < 0 ? 0 : WS_EX_NOACTIVATE);
    HWND owner = parent < 0 ? nullptr : s.levels[0]->hwnd;
    l->hwnd = CreateWindowExW(ex, kClass, APP_NAME, WS_POPUP, x, y, w, h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!l->hwnd) return;

    // Acrylic and rounded, like the flyout's OSD and the Windows 11 menus.
    BOOL dark = !s.light;
    DwmSetWindowAttribute(l->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(l->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_TRANSIENTWINDOW;
    DwmSetWindowAttribute(l->hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(l->hwnd, &margins);

    HWND hwnd = l->hwnd;
    s.levels.push_back(std::move(l));
    if (parent < 0) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    } else {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
}

void OpenSub(int li, int row) {
    CloseFrom(li + 1);
    Level& p = *g_s->levels[li];
    p.open = p.hot = row;
    Redraw(p);
    OpenLevel((*p.items)[row].sub, li, row, 0, 0, false);
}

int FirstSelectable(const Level& l) {
    for (size_t i = 0; i < l.items->size(); i++)
        if (Selectable((*l.items)[i])) return (int)i;
    return -1;
}

void Activate(int li, int row) {
    Level& l = *g_s->levels[li];
    const MenuItem& it = (*l.items)[row];
    if (!it.sub.empty()) {
        if (l.open != row) OpenSub(li, row);
        return;
    }
    g_s->result = it.id;
    g_s->done = true;
}

void OnKey(WPARAM vk) {
    Session& s = *g_s;
    int li = (int)s.levels.size() - 1;
    Level& l = *s.levels[li];
    int n = (int)l.items->size();
    switch (vk) {
        case VK_DOWN:
        case VK_UP: {
            int step = vk == VK_DOWN ? 1 : n - 1;
            int i = l.hot;
            for (int k = 0; k < n; k++) {
                i = i < 0 ? (vk == VK_DOWN ? 0 : n - 1) : (i + step) % n;
                if (Selectable((*l.items)[i])) break;
            }
            l.hot = i;
            Redraw(l);
            break;
        }
        case VK_RIGHT:
            if (l.hot >= 0 && !(*l.items)[l.hot].sub.empty()) {
                OpenSub(li, l.hot);
                Level& sub = *s.levels.back();
                sub.hot = FirstSelectable(sub);
                Redraw(sub);
            }
            break;
        case VK_LEFT:
            if (li > 0) CloseFrom(li);
            break;
        case VK_ESCAPE:
            if (li > 0)
                CloseFrom(li);
            else
                s.done = true;
            break;
        case VK_RETURN:
        case VK_SPACE:
            if (l.hot >= 0) {
                Activate(li, l.hot);
                if (!s.done && s.levels.size() > (size_t)li + 1) {
                    Level& sub = *s.levels.back();
                    sub.hot = FirstSelectable(sub);
                    Redraw(sub);
                }
            }
            break;
    }
}

LRESULT CALLBACK MenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    int li = g_s ? FindLevel(hwnd) : -1;
    if (li < 0) return DefWindowProcW(hwnd, msg, wp, lp);
    Session& s = *g_s;
    Level& l = *s.levels[li];
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ACTIVATE:
            // Clicking anywhere else takes the activation away: close.
            if (li == 0 && LOWORD(wp) == WA_INACTIVE) s.done = true;
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Render(l);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!l.tracking) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd};
                l.tracking = TrackMouseEvent(&tme) != FALSE;
            }
            int i = HitTest(l, GET_Y_LPARAM(lp) / s.scale);
            if (i != l.hot) {
                l.hot = i;
                Redraw(l);
                // Submenus open and close after the system's menu delay.
                KillTimer(hwnd, kSubTimer);
                bool opens = i >= 0 && !(*l.items)[i].sub.empty() && l.open != i;
                bool closes = l.open >= 0 && l.open != i;
                if (opens || closes) {
                    UINT delay = 400;
                    SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &delay, 0);
                    SetTimer(hwnd, kSubTimer, delay, nullptr);
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            l.tracking = false;
            KillTimer(hwnd, kSubTimer);
            l.hot = l.open;  // a row with its submenu open stays lit
            Redraw(l);
            return 0;
        case WM_TIMER:
            if (wp == kSubTimer) {
                KillTimer(hwnd, kSubTimer);
                if (l.hot >= 0 && !(*l.items)[l.hot].sub.empty() && l.open != l.hot)
                    OpenSub(li, l.hot);
                else if (l.open >= 0 && l.hot != l.open)
                    CloseFrom(li + 1);
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            int i = HitTest(l, GET_Y_LPARAM(lp) / s.scale);
            if (i >= 0) Activate(li, i);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            OnKey(wp);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

UINT TrackMenu(const std::vector<MenuItem>& items, int x, int y, bool rightAlign, bool light) {
    if (g_s) {  // right-clicked again while open: close it
        g_s->done = true;
        return 0;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = MenuProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }

    Session s;
    s.monitor = MonitorFromPoint({x, y}, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(s.monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    s.dpi = dpiX;
    s.scale = dpiX / 96.0f;
    s.light = light;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, s.d2d.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)s.dw.GetAddressOf())))
        return 0;

    // The system menu font, at its 96-DPI size in DIPs.
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, 96);
    float size = (float)std::abs(ncm.lfMenuFont.lfHeight);
    if (size < 1) size = 12;
    auto format = [&](const wchar_t* family, DWRITE_FONT_WEIGHT weight, float sz, ComPtr<IDWriteTextFormat>& out) {
        s.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sz, L"", &out);
        if (!out) return;
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    format(ncm.lfMenuFont.lfFaceName, DWRITE_FONT_WEIGHT_NORMAL, size, s.text);
    format(ncm.lfMenuFont.lfFaceName, DWRITE_FONT_WEIGHT_SEMI_BOLD, size, s.current);
    format(L"Segoe Fluent Icons", DWRITE_FONT_WEIGHT_NORMAL, 10, s.glyph);
    if (!s.text || !s.current || !s.glyph) return 0;
    s.glyph->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

    if (light) {
        s.tint = Rgba(0xF9F9F9, 0.85f);
        s.fg = Rgba(0x000000, 0.894f);
        s.arrow = Rgba(0x000000, 0.62f);
        s.hover = Rgba(0x000000, 0.0373f);
        s.highlight = Rgba(0x000000, 0.06f);
        s.divider = Rgba(0x000000, 0.0803f);
    } else {
        s.tint = Rgba(0x2C2C2C, 0.85f);
        s.fg = Rgba(0xFFFFFF);
        s.arrow = Rgba(0xFFFFFF, 0.786f);
        s.hover = Rgba(0xFFFFFF, 0.0605f);
        s.highlight = Rgba(0xFFFFFF, 0.09f);
        s.divider = Rgba(0xFFFFFF, 0.0837f);
    }

    g_s = &s;
    OpenLevel(items, -1, -1, x, y, rightAlign);
    MSG m;
    while (!s.done && !s.levels.empty()) {
        if (GetMessageW(&m, nullptr, 0, 0) <= 0) {
            PostQuitMessage((int)m.wParam);
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CloseFrom(0);
    g_s = nullptr;
    return s.result;
}
