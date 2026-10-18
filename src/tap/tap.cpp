// VolumeOneTap.dll: loaded into explorer.exe by VolumeOne through
// InitializeXamlDiagnosticsEx. It watches the taskbar's XAML tree and
// collapses the volume glyph inside the Quick Settings tray button (the one
// that also shows network/battery), exactly what the Windhawk mod
// "taskbar-tray-system-icon-tweaks" does with hideVolumeIcon.

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
#include <winrt/Windows.UI.Xaml.Media.h>

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
    bool hidden = false;
};

std::mutex g_lock;
std::vector<std::shared_ptr<Tracked>> g_tracked;
std::atomic<bool> g_hide{false};
winrt::com_ptr<IXamlDiagnostics> g_diag;
HANDLE g_refreshEvent = nullptr;
HANDLE g_loadedEvent = nullptr;

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

bool ShouldHide() {
    if (!FindWindowW(HOST_WINDOW_CLASS, nullptr)) return false;
    DWORD value = 0, size = sizeof(value);
    LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, REG_KEY_PATH, REG_HIDE_ICON, RRF_RT_REG_DWORD, nullptr, &value, &size);
    LOG(L"ShouldHide: window found, reg status=%ld value=%lu", st, value);
    (void)st;
    return value != 0;
}

bool IsVolumeGlyph(const winrt::hstring& text) {
    if (text.size() != 1) return false;
    switch (text[0]) {
        case L'':  // Mute
        case L'':  // Volume0
        case L'':  // Volume1
        case L'':  // Volume2
        case L'':  // Volume3
        case L'':  // VolumeDisabled
        case L'':  // VolumeBars
            return true;
    }
    return false;
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
        LOG(L"  parent %d: %s name=%s", i, cls.c_str(), fe.Name().c_str());
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
    LOG(L"control center icon, glyph %04X, hide=%d", tb.Text().empty() ? 0 : tb.Text()[0], (bool)g_hide);

    LOG(L"eval inCC=%d content=%d text=%04X", inControlCenter, (bool)content, tb.Text().empty() ? 0 : tb.Text()[0]);
    bool hide = g_hide && IsVolumeGlyph(tb.Text());
    if (hide) {
        // Check the real state, not just our flag: something else (e.g. a
        // Windhawk mod unloading) may have made it visible again.
        if (content.Visibility() != wux::Visibility::Collapsed) content.Visibility(wux::Visibility::Collapsed);
        if (iconView && iconView.IsEnabled()) iconView.IsEnabled(false);
        t.hidden = true;
    } else if (t.hidden) {  // only undo what we did
        content.Visibility(wux::Visibility::Visible);
        if (iconView) iconView.IsEnabled(true);
        t.hidden = false;
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
}

struct TreeWatcher : winrt::implements<TreeWatcher, IVisualTreeServiceCallback2> {
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation, VisualElement element,
                                                 VisualMutationType mutation) override {
        try {
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
        g_hide = ShouldHide();
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
                           reinterpret_cast<LPCWSTR>(&ShouldHide), &self);

        std::wstring loadedName = TAP_LOADED_PREFIX + std::to_wstring(GetCurrentProcessId());
        g_loadedEvent = CreateEventW(nullptr, TRUE, FALSE, loadedName.c_str());
        g_refreshEvent = CreateEventW(nullptr, FALSE, FALSE, TAP_REFRESH_EVENT_NAME);
        g_hide = ShouldHide();
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
