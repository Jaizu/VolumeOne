// VolumeOneTap.dll: loaded into explorer.exe by VolumeOne through
// InitializeXamlDiagnosticsEx. It watches the taskbar's XAML tree for the
// volume glyph inside the Quick Settings tray button (the one that also shows
// network/battery) and, depending on VolumeOne's settings, either collapses it
// (what the Windhawk mod "taskbar-tray-system-icon-tweaks" does with
// hideVolumeIcon) or keeps it and forwards its clicks to VolumeOne.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <ocidl.h>
#include <xamlOM.h>
#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../shared.h"

namespace wux = winrt::Windows::UI::Xaml;
using winrt::Windows::UI::Core::CoreDispatcher;
using winrt::Windows::UI::Core::CoreDispatcherPriority;

namespace {

struct Tracked {
    winrt::weak_ref<wux::Controls::TextBlock> textBlock;
    CoreDispatcher dispatcher{nullptr};
};

std::mutex g_lock;
std::vector<std::shared_ptr<Tracked>> g_tracked;
std::atomic<bool> g_hide{false};       // collapse the Windows volume icon
std::atomic<bool> g_intercept{false};  // keep it, but send its clicks to VolumeOne
std::atomic<bool> g_hideQs{false};     // hide the Quick Settings volume slider
winrt::com_ptr<IXamlDiagnostics> g_diag;
HANDLE g_refreshEvent = nullptr;

#ifdef TAP_LOG
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"VolumeOneTap.log");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"[%lu] %s\n", GetCurrentThreadId(), buf);
        fclose(f);
    }
}
#define LOG(...) Log(__VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

DWORD ReadDword(const wchar_t* name) {
    DWORD value = 0, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, REG_KEY_PATH, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value;
}

// Re-reads what VolumeOne wants. Nothing happens while it isn't running, so
// quitting it gives Windows its icon back.
void ReadState() {
    bool running = FindWindowW(HOST_WINDOW_CLASS, nullptr) != nullptr;
    bool useSystem = running && ReadDword(REG_USE_SYSTEM_ICON) != 0;
    g_intercept = useSystem;
    g_hide = running && !useSystem && ReadDword(REG_HIDE_ICON) != 0;
    g_hideQs = running && ReadDword(REG_HIDE_QS_VOLUME) != 0;
    LOG(L"ReadState: running=%d hide=%d intercept=%d", running, (bool)g_hide, (bool)g_intercept);
}

void PostToHost(WPARAM what, LPARAM lp = 0) {
    HWND host = FindWindowW(HOST_WINDOW_CLASS, nullptr);
    if (!host) return;
    // explorer has the foreground right now (the user clicked the taskbar);
    // pass it on so VolumeOne's flyout or menu can take focus.
    DWORD pid = 0;
    GetWindowThreadProcessId(host, &pid);
    AllowSetForegroundWindow(pid);
    PostMessageW(host, WM_VOLUMEONE_SYSICON, what, lp);
}

bool IsVolumeGlyph(const winrt::hstring& text) {
    if (text.size() != 1) return false;
    switch (text[0]) {
        case 0xE74F:  // Mute
        case 0xE992:  // Volume0
        case 0xE993:  // Volume1
        case 0xE994:  // Volume2
        case 0xE995:  // Volume3
        case 0xEA85:  // VolumeDisabled
        case 0xEBC5:  // VolumeBars
            return true;
    }
    return false;
}

// True if any text block inside the icon shows a volume glyph. One icon has
// several (Underlay with the dim bars, Base with the level, AccentOverlay),
// and when muted Base shows the mute glyph while Underlay goes empty, so the
// decision has to be made for the whole icon, not per text block.
bool ContentIsVolume(const wux::DependencyObject& node, int depth = 0) {
    if (auto tb = node.try_as<wux::Controls::TextBlock>())
        if (IsVolumeGlyph(tb.Text())) return true;
    if (depth > 6) return false;
    int n = wux::Media::VisualTreeHelper::GetChildrenCount(node);
    for (int i = 0; i < n; i++)
        if (ContentIsVolume(wux::Media::VisualTreeHelper::GetChild(node, i), depth + 1)) return true;
    return false;
}

// Icons we collapsed, so turning the option off only undoes our own changes.
// UI thread only.
std::vector<winrt::weak_ref<wux::FrameworkElement>> g_hiddenContents;

bool IsHiddenByUs(const wux::FrameworkElement& content, bool remove) {
    for (auto it = g_hiddenContents.begin(); it != g_hiddenContents.end();) {
        auto e = it->get();
        if (!e) {
            it = g_hiddenContents.erase(it);
            continue;
        }
        if (e == content) {
            if (remove) g_hiddenContents.erase(it);
            return true;
        }
        ++it;
    }
    return false;
}

// "Use the Windows icon" mode. Marking pointer events handled on the icon
// doesn't stop the Quick Settings button that contains it from opening, so
// instead a transparent popup is laid over the volume icon. The popup isn't
// part of the button's tree, so the button never sees those clicks. It draws
// its own hover highlight. UI thread only.
struct Overlay {
    winrt::weak_ref<wux::FrameworkElement> target;
    winrt::weak_ref<wux::Controls::Control> button;  // the Quick Settings button around the icon
    wux::Controls::Primitives::Popup popup{nullptr};
    wux::Controls::Border border{nullptr};
    winrt::event_token layoutToken{};
    bool leftDown = false;
};
Overlay g_overlay;

winrt::Windows::UI::Color HoverColor(const wux::FrameworkElement& e, bool pressed) {
    bool dark = e.ActualTheme() == wux::ElementTheme::Dark;
    uint8_t a = pressed ? 0x0A : 0x0F;  // SubtleFillColorSecondary / Tertiary
    return dark ? winrt::Windows::UI::Color{a, 255, 255, 255} : winrt::Windows::UI::Color{(uint8_t)(a - 4), 0, 0, 0};
}

void PlaceOverlay() {
    auto target = g_overlay.target.get();
    if (!target || !g_overlay.popup) return;
    auto p = target.TransformToVisual(nullptr).TransformPoint({0, 0});
    float w = (float)target.ActualWidth(), h = (float)target.ActualHeight();
    if (g_overlay.popup.HorizontalOffset() != p.X) g_overlay.popup.HorizontalOffset(p.X);
    if (g_overlay.popup.VerticalOffset() != p.Y) g_overlay.popup.VerticalOffset(p.Y);
    if (g_overlay.border.Width() != w) g_overlay.border.Width(w);
    if (g_overlay.border.Height() != h) g_overlay.border.Height(h);
}

void RemoveOverlay() {
    if (auto target = g_overlay.target.get())
        if (g_overlay.layoutToken) target.LayoutUpdated(g_overlay.layoutToken);
    if (g_overlay.popup) g_overlay.popup.IsOpen(false);
    g_overlay = Overlay{};
}

void EnsureOverlay(const wux::FrameworkElement& icon) {
    if (g_overlay.target.get() == icon && g_overlay.popup) {
        PlaceOverlay();
        return;
    }
    RemoveOverlay();

    wux::Controls::Border border;
    border.Background(wux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));  // transparent but hit-testable
    border.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(4));
    wux::Controls::Primitives::Popup popup;
    popup.XamlRoot(icon.XamlRoot());
    popup.Child(border);

    // Hover/press look: put the whole Quick Settings button in its own
    // visual state, so it highlights exactly as when hovering the network
    // icon. Only if the button doesn't have those states, draw our own.
    auto setFill = [](bool hover, bool pressed) {
        auto target = g_overlay.target.get();
        if (!target || !g_overlay.border) return;
        if (auto button = g_overlay.button.get()) {
            const wchar_t* state = pressed ? L"Pressed" : hover ? L"PointerOver" : L"Normal";
            if (wux::VisualStateManager::GoToState(button, state, true)) {
                g_overlay.border.Background(wux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
                return;
            }
        }
        auto c = hover ? HoverColor(target, pressed) : winrt::Windows::UI::Color{0, 0, 0, 0};
        g_overlay.border.Background(wux::Media::SolidColorBrush(c));
    };
    border.PointerEntered([setFill](auto&&, auto&&) { setFill(true, false); });
    border.PointerExited([setFill](auto&&, auto&&) {
        g_overlay.leftDown = false;
        setFill(false, false);
    });
    border.PointerPressed([setFill](auto&& sender, const wux::Input::PointerRoutedEventArgs& e) {
        auto props = e.GetCurrentPoint(nullptr).Properties();
        if (props.IsLeftButtonPressed()) {
            g_overlay.leftDown = true;
            setFill(true, true);
        } else if (props.IsMiddleButtonPressed()) {
            PostToHost(SYSICON_MIDDLE);
        }
        e.Handled(true);
    });
    border.PointerReleased([setFill](auto&&, const wux::Input::PointerRoutedEventArgs& e) {
        e.Handled(true);
        if (!g_overlay.leftDown) return;
        g_overlay.leftDown = false;
        setFill(true, false);
        PostToHost(SYSICON_CLICK);
    });
    border.RightTapped([](auto&&, const wux::Input::RightTappedRoutedEventArgs& e) {
        e.Handled(true);
        PostToHost(SYSICON_MENU);
    });
    border.PointerWheelChanged([](auto&&, const wux::Input::PointerRoutedEventArgs& e) {
        e.Handled(true);
        PostToHost(SYSICON_WHEEL, e.GetCurrentPoint(nullptr).Properties().MouseWheelDelta());
    });

    g_overlay.target = icon;
    for (wux::DependencyObject n = icon; n; n = wux::Media::VisualTreeHelper::GetParent(n)) {
        auto fe = n.try_as<wux::FrameworkElement>();
        if (fe && fe.Name() == L"ControlCenterButton") {
            g_overlay.button = fe.try_as<wux::Controls::Control>();
            break;
        }
    }
    g_overlay.popup = popup;
    g_overlay.border = border;
    // Follow the icon when the tray re-lays out (icons added/removed, DPI...).
    g_overlay.layoutToken = icon.LayoutUpdated([](auto&&, auto&&) {
        try {
            PlaceOverlay();
        } catch (...) {
        }
    });
    PlaceOverlay();
    popup.IsOpen(true);
}

// UI thread. TextBlock "InnerTextBlock" -> ... -> SystemTray.TextIconContent
// -> ... -> SystemTray.IconView -> ... -> "ControlCenterButton".
void Evaluate(Tracked& t) {
    auto tb = t.textBlock.get();
    if (!tb) return;

    wux::FrameworkElement content{nullptr};
    wux::Controls::Control iconView{nullptr};
    bool inControlCenter = false;
    wux::DependencyObject node = tb;
    for (int i = 0; i < 16; i++) {
        node = wux::Media::VisualTreeHelper::GetParent(node);
        if (!node) break;
        auto fe = node.try_as<wux::FrameworkElement>();
        if (!fe) continue;
        auto cls = winrt::get_class_name(fe);
        if (!content && cls == L"SystemTray.TextIconContent")
            content = fe;
        else if (!iconView && cls == L"SystemTray.IconView")
            iconView = fe.try_as<wux::Controls::Control>();
        if (fe.Name() == L"ControlCenterButton") {
            inControlCenter = true;
            break;
        }
    }
    if (!inControlCenter || !content) return;

    bool isVolume = ContentIsVolume(content);
    LOG(L"control center icon, glyph %04X, volume=%d hide=%d", tb.Text().empty() ? 0 : tb.Text()[0], isVolume,
        (bool)g_hide);
    if (isVolume && iconView) {
        if (g_intercept)
            EnsureOverlay(iconView);
        else if (g_overlay.target.get() == iconView)
            RemoveOverlay();
    }
    // Once an icon is known to be the volume one it stays hidden, even if its
    // glyphs are briefly all empty while Windows switches them.
    if (g_hide && (isVolume || IsHiddenByUs(content, false))) {
        // Check the real state, not just a flag: something else (e.g. a
        // Windhawk mod unloading) may have made it visible again.
        if (content.Visibility() != wux::Visibility::Collapsed) content.Visibility(wux::Visibility::Collapsed);
        if (iconView && iconView.IsEnabled()) iconView.IsEnabled(false);
        if (!IsHiddenByUs(content, false)) g_hiddenContents.push_back(winrt::make_weak(content));
    } else if (IsHiddenByUs(content, true)) {
        content.Visibility(wux::Visibility::Visible);
        if (iconView) iconView.IsEnabled(true);
    }
}

void SafeEvaluate(const std::shared_ptr<Tracked>& t) {
    try {
        Evaluate(*t);
    } catch (const winrt::hresult_error& e) {
        LOG(L"Evaluate error %08X %s", (unsigned)e.code(), e.message().c_str());
        (void)e;
    } catch (...) {
        LOG(L"Evaluate unknown error");
    }
}

// UI thread.
void OnTextBlock(const wux::Controls::TextBlock& tb) {
    auto t = std::make_shared<Tracked>();
    t->textBlock = tb;
    t->dispatcher = tb.Dispatcher();
    {
        std::lock_guard lock(g_lock);
        g_tracked.erase(std::remove_if(g_tracked.begin(), g_tracked.end(),
                                       [](const auto& x) { return !x->textBlock.get(); }),
                        g_tracked.end());
        g_tracked.push_back(t);
    }
    // The glyph is set (and changes with the volume level) after creation,
    // and the element is only parented once loaded, so re-check on both.
    tb.RegisterPropertyChangedCallback(wux::Controls::TextBlock::TextProperty(),
                                       [t](auto&&, auto&&) { SafeEvaluate(t); });
    tb.Loaded([t](auto&&, auto&&) { SafeEvaluate(t); });
    SafeEvaluate(t);
}

// --- Quick Settings (ShellHost.exe) ----------------------------------------
// The volume slider row is found from its "VolumeL2Button" (the ">" that opens
// the device list). Hidden element: the highest ancestor inside "SlidersGroup"
// that holds no other slider, so a brightness slider on laptops stays. On a
// desktop that is the whole group. UI thread only.

int CountSliders(const wux::DependencyObject& node, int depth = 0) {
    int n = winrt::get_class_name(node) == L"ControlCenter.AsyncSlider" ? 1 : 0;
    if (depth > 14) return n;
    int count = wux::Media::VisualTreeHelper::GetChildrenCount(node);
    for (int i = 0; i < count; i++) n += CountSliders(wux::Media::VisualTreeHelper::GetChild(node, i), depth + 1);
    return n;
}

// ControlCenterView keeps its old measured height when the row collapses (the
// panel stays as tall, with a gap above the footer), so re-measure the chain
// up to the panel's region.
void InvalidateQsLayout(const wux::DependencyObject& from) {
    for (wux::DependencyObject n = from; n; n = wux::Media::VisualTreeHelper::GetParent(n)) {
        if (auto u = n.try_as<wux::UIElement>()) u.InvalidateMeasure();
        auto fe = n.try_as<wux::FrameworkElement>();
        if (fe && fe.Name() == L"ControlCenterRegion") break;
    }
}

winrt::weak_ref<wux::FrameworkElement> g_qsButton;
winrt::weak_ref<wux::UIElement> g_qsHidden;
CoreDispatcher g_qsDispatcher{nullptr};

void EvaluateQs() {
    auto button = g_qsButton.get();
    if (!button) return;
    wux::UIElement row{nullptr};
    bool inGroup = false, full = false;
    for (wux::DependencyObject n = button; n; n = wux::Media::VisualTreeHelper::GetParent(n)) {
        auto fe = n.try_as<wux::FrameworkElement>();
        if (!fe) continue;
        if (!full && CountSliders(fe) > 1) full = true;  // holds another slider too: stop growing the row
        if (!full) row = fe;
        if (fe.Name() == L"SlidersGroup") {
            inGroup = true;
            break;
        }
    }
    // Never touch anything that isn't inside the sliders group.
    if (!row || !inGroup) return;
    LOG(L"QS row %s name=%s", winrt::get_class_name(row).c_str(), row.try_as<wux::FrameworkElement>().Name().c_str());
    if (g_hideQs) {
        if (row.Visibility() != wux::Visibility::Collapsed) row.Visibility(wux::Visibility::Collapsed);
        g_qsHidden = row;
        InvalidateQsLayout(row);
    } else if (auto hidden = g_qsHidden.get()) {
        hidden.Visibility(wux::Visibility::Visible);
        g_qsHidden = nullptr;
        InvalidateQsLayout(hidden);
    }
}

void SafeEvaluateQs() {
    try {
        EvaluateQs();
    } catch (...) {
    }
}

void OnQsVolumeButton(const wux::FrameworkElement& button) {
    g_qsButton = button;
    g_qsDispatcher = button.Dispatcher();
    // The row is only complete once loaded; re-check then too.
    button.Loaded([](auto&&, auto&&) { SafeEvaluateQs(); });
    SafeEvaluateQs();
}

void ApplyAll() {
    std::vector<std::shared_ptr<Tracked>> copy;
    {
        std::lock_guard lock(g_lock);
        copy = g_tracked;
    }
    for (auto& t : copy) {
        try {
            if (t->dispatcher) t->dispatcher.RunAsync(CoreDispatcherPriority::Normal, [t] { SafeEvaluate(t); });
        } catch (...) {
        }
    }
    try {
        if (g_qsDispatcher) g_qsDispatcher.RunAsync(CoreDispatcherPriority::Normal, [] { SafeEvaluateQs(); });
    } catch (...) {
    }
}

struct TreeWatcher : winrt::implements<TreeWatcher, IVisualTreeServiceCallback2> {
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation, VisualElement element,
                                                 VisualMutationType mutation) override {
        try {
#ifdef TAP_LOG_ALL
            LOG(L"TREE %s type=%s name=%s", mutation == Add ? L"add" : L"remove", element.Type ? element.Type : L"?",
                element.Name ? element.Name : L"");
#endif
            if (mutation == Add && element.Name && wcscmp(element.Name, L"VolumeL2Button") == 0) {
                winrt::Windows::Foundation::IInspectable o;
                if (SUCCEEDED(g_diag->GetIInspectableFromHandle(element.Handle,
                                                                reinterpret_cast<::IInspectable**>(winrt::put_abi(o))))) {
                    if (auto button = o.try_as<wux::FrameworkElement>()) {
                        auto d = button.Dispatcher();
                        if (d.HasThreadAccess())
                            OnQsVolumeButton(button);
                        else
                            d.RunAsync(CoreDispatcherPriority::Normal, [button] { OnQsVolumeButton(button); });
                    }
                }
                return S_OK;
            }
            if (mutation != Add || !element.Name || wcscmp(element.Name, L"InnerTextBlock") != 0) return S_OK;
            LOG(L"InnerTextBlock added, type=%s", element.Type ? element.Type : L"?");
            winrt::Windows::Foundation::IInspectable obj;
            if (FAILED(g_diag->GetIInspectableFromHandle(element.Handle,
                                                         reinterpret_cast<::IInspectable**>(winrt::put_abi(obj)))))
                return S_OK;
            auto tb = obj.try_as<wux::Controls::TextBlock>();
            if (!tb) return S_OK;
            auto dispatcher = tb.Dispatcher();
            LOG(L"dispatcher=%d access=%d", (bool)dispatcher, dispatcher ? dispatcher.HasThreadAccess() : -1);
            if (dispatcher.HasThreadAccess())
                OnTextBlock(tb);
            else
                dispatcher.RunAsync(CoreDispatcherPriority::Normal, [tb] {
                    try {
                        OnTextBlock(tb);
                    } catch (...) {
                    }
                });
        } catch (const winrt::hresult_error& e) {
            LOG(L"OnVisualTreeChange error %08X %s", (unsigned)e.code(), e.message().c_str());
            (void)e;
        } catch (...) {
            LOG(L"OnVisualTreeChange unknown error");
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) override {
        return S_OK;
    }
};

winrt::com_ptr<TreeWatcher> g_watcher;

DWORD WINAPI AdviseThread(void*) {
    // Advising from the thread that called SetSite deadlocks, so do it here.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (auto tree = g_diag.try_as<IVisualTreeService3>()) {
        HRESULT hr = tree->AdviseVisualTreeChange(g_watcher.get());
        LOG(L"AdviseVisualTreeChange hr=%08X", hr);
    } else {
        LOG(L"no IVisualTreeService3");
    }
    return 0;
}

DWORD WINAPI RefreshThread(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    for (;;) {
        WaitForSingleObject(g_refreshEvent, INFINITE);
        ReadState();
        ApplyAll();
    }
}

struct Tap : winrt::implements<Tap, IObjectWithSite> {
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        if (!site || g_diag) return S_OK;
        if (FAILED(site->QueryInterface(IID_PPV_ARGS(g_diag.put())))) return E_NOINTERFACE;

        // Stay loaded for the lifetime of explorer: our callbacks live here.
        HMODULE self;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&ReadState), &self);

        std::wstring refreshName = TAP_REFRESH_PREFIX + std::to_wstring(GetCurrentProcessId());
        g_refreshEvent = CreateEventW(nullptr, FALSE, FALSE, refreshName.c_str());
        ReadState();
        g_watcher = winrt::make_self<TreeWatcher>();
        LOG(L"SetSite pid=%lu hide=%d", GetCurrentProcessId(), (bool)g_hide);

        if (HANDLE h = CreateThread(nullptr, 0, AdviseThread, nullptr, 0, nullptr)) CloseHandle(h);
        if (HANDLE h = CreateThread(nullptr, 0, RefreshThread, nullptr, 0, nullptr)) CloseHandle(h);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppv) override {
        if (!g_diag) {
            *ppv = nullptr;
            return E_FAIL;
        }
        return g_diag->QueryInterface(riid, ppv);
    }
};

struct TapFactory : winrt::implements<TapFactory, IClassFactory> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        try {
            return winrt::make_self<Tap>()->QueryInterface(riid, ppv);
        } catch (...) {
            return winrt::to_hresult();
        }
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};

}  // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    *ppv = nullptr;
    if (clsid != CLSID_VolumeOneTap) return CLASS_E_CLASSNOTAVAILABLE;
    try {
        return winrt::make_self<TapFactory>()->QueryInterface(riid, ppv);
    } catch (...) {
        return winrt::to_hresult();
    }
}

STDAPI DllCanUnloadNow() { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
