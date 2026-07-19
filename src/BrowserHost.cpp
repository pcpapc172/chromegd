#include "BrowserHost.hpp"

#ifdef GEODE_IS_WINDOWS

#include <Geode/Geode.hpp>

#include <include/cef_app.h>
#include <include/cef_client.h>
#include <include/cef_context_menu_handler.h>
#include <include/cef_display_handler.h>
#include <include/cef_download_handler.h>
#include <include/cef_jsdialog_handler.h>
#include <include/cef_life_span_handler.h>
#include <include/cef_load_handler.h>
#include <include/cef_permission_handler.h>
#include <include/cef_render_handler.h>
#include <include/cef_task.h>
#include <include/wrapper/cef_helpers.h>
#include <include/wrapper/cef_message_router.h>

#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>

using namespace geode::prelude;

namespace {

// Chromium's BindOnce rejects capturing lambdas, so post CefTasks instead.
class FunctionTask final : public CefTask {
public:
    explicit FunctionTask(std::function<void()> fn) : m_fn(std::move(fn)) {}
    void Execute() override { m_fn(); }

private:
    std::function<void()> m_fn;
    IMPLEMENT_REFCOUNTING(FunctionTask);
};

template <typename Fn>
void runOnUI(Fn&& fn) {
    if (CefCurrentlyOn(TID_UI)) fn();
    else CefPostTask(TID_UI, new FunctionTask(std::forward<Fn>(fn)));
}

// Shows a JS dialog (alert/confirm/leave-site) as an in-game popup instead of
// a native window, which would minimize a fullscreen game. Reparented onto
// the overlay so it renders above the browser window (z 10000).
void showPageDialog(
    char const* title, std::string message, char const* btn1, char const* btn2,
    std::function<void(bool)> done
) {
    geode::queueInMainThread([title, message = std::move(message), btn1, btn2, done = std::move(done)] {
        auto* popup = geode::createQuickPopup(title, message, btn1, btn2, [done](FLAlertLayer*, bool second) {
            done(second);
        }, false);
        popup->show();
        if (auto* parent = popup->getParent(); parent && parent != OverlayManager::get()) {
            popup->retain();
            popup->removeFromParentAndCleanup(false);
            OverlayManager::get()->addChild(popup, 20000);
            popup->release();
        }
    });
}

uint32_t currentModifiers() {
    uint32_t mods = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= EVENTFLAG_SHIFT_DOWN;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= EVENTFLAG_CONTROL_DOWN;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= EVENTFLAG_ALT_DOWN;
    if (GetKeyState(VK_LBUTTON) & 0x8000) mods |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (GetKeyState(VK_RBUTTON) & 0x8000) mods |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    if (GetKeyState(VK_MBUTTON) & 0x8000) mods |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    return mods;
}

struct WindowSearch {
    DWORD pid;
    HWND hwnd = nullptr;
};

BOOL CALLBACK enumWindow(HWND hwnd, LPARAM data) {
    auto* search = reinterpret_cast<WindowSearch*>(data);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == search->pid && GetWindow(hwnd, GW_OWNER) == nullptr && IsWindowVisible(hwnd)) {
        search->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

// TEMP diagnostics for pointer-lock debugging.
void debugLog(std::string const& line) {
    auto path = Mod::get()->getSaveDir() / "pointerlock-debug.log";
    if (auto* file = _wfopen(path.wstring().c_str(), L"ab")) {
        fwrite(line.data(), 1, line.size(), file);
        fwrite("\n", 1, 1, file);
        fclose(file);
    }
}

// Injected into every page. Chromium's off-screen renderer refuses real
// pointer lock ("root document not valid"), so we fake the whole Pointer Lock
// API in JS: the page believes it is locked and reads movementX/Y from mouse
// events (which Chromium still computes from the positions the native side
// feeds it), while native hides and recenters the OS cursor.
constexpr char kPointerLockScript[] = R"JS(
if (!window.__gdPointerLockHooked) {
    window.__gdPointerLockHooked = true;
    var ping = function (msg) {
        if (window.cefQuery) {
            window.cefQuery({request: msg, persistent: false,
                onSuccess: function () {}, onFailure: function () {}});
        }
    };
    var lockEl = null;
    Object.defineProperty(document, 'pointerLockElement', {
        configurable: true, get: function () { return lockEl; }
    });
    var fire = function () { document.dispatchEvent(new Event('pointerlockchange')); };
    Element.prototype.requestPointerLock = function () {
        lockEl = this;
        ping('pointerlock:on');
        setTimeout(fire, 0);
        return Promise.resolve();
    };
    document.exitPointerLock = function () {
        if (!lockEl) return;
        lockEl = null;
        ping('pointerlock:off');
        setTimeout(fire, 0);
    };
    // Native warps the virtual cursor across the view to allow unbounded
    // turning; those warp frames carry a huge movement delta, so drop them
    // (real per-frame motion is small) to keep turning seamless.
    window.addEventListener('mousemove', function (e) {
        if (lockEl && (Math.abs(e.movementX) > 400 || Math.abs(e.movementY) > 400)) {
            e.stopImmediatePropagation();
        }
    }, true);
    // If the page loses focus, drop the fake lock too.
    window.addEventListener('blur', function () { document.exitPointerLock(); });
}
)JS";

class PointerLockQueryHandler final : public CefMessageRouterBrowserSide::Handler {
public:
    bool OnQuery(
        CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int64_t, CefString const& request,
        bool, CefRefPtr<Callback> callback
    ) override {
        auto text = request.ToString();
        debugLog("OnQuery: " + text);
        if (text == "pointerlock:on" || text == "pointerlock:off") {
            bool locked = text == "pointerlock:on";
            geode::queueInMainThread([locked] { BrowserHost::get().setPointerLocked(locked); });
            callback->Success("");
            return true;
        }
        if (text.starts_with("bridge:")) { callback->Success(""); return true; }
        return false;
    }
};

class GdCefApp final : public CefApp {
public:
    void OnBeforeCommandLineProcessing(
        CefString const& processType, CefRefPtr<CefCommandLine> commandLine
    ) override {
        commandLine->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
    }

private:
    IMPLEMENT_REFCOUNTING(GdCefApp);
};

}  // namespace

class GdCefClient final : public CefClient,
                          public CefRenderHandler,
                          public CefLifeSpanHandler,
                          public CefDisplayHandler,
                          public CefLoadHandler,
                          public CefPermissionHandler,
                          public CefJSDialogHandler,
                          public CefDownloadHandler,
                          public CefContextMenuHandler {
public:
    explicit GdCefClient(std::shared_ptr<BrowserSurface> surface)
      : m_surface(std::move(surface)) {}

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
    CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }

    // CEF's default context menu is a native popup that doesn't render in
    // off-screen mode, so render our own in-game menu from the model instead.
    bool RunContextMenu(
        CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams> params,
        CefRefPtr<CefMenuModel> model, CefRefPtr<CefRunContextMenuCallback> callback
    ) override {
        std::vector<BrowserHost::ContextMenuItem> items;
        for (size_t i = 0; i < model->GetCount(); ++i) {
            BrowserHost::ContextMenuItem item;
            item.separator = model->GetTypeAt(i) == MENUITEMTYPE_SEPARATOR;
            item.commandId = model->GetCommandIdAt(i);
            item.label = model->GetLabelAt(i).ToString();
            item.enabled = model->IsEnabledAt(i);
            if (!item.separator && item.label.empty()) continue;
            items.push_back(std::move(item));
        }
        auto& host = BrowserHost::get();
        auto bounds = host.boundsSnapshot();
        float scale = host.dpiScale();
        int clientX = bounds.left + static_cast<int>(params->GetXCoord() * scale);
        int clientY = bounds.top + static_cast<int>(params->GetYCoord() * scale);
        host.m_contextCallback = callback;
        geode::queueInMainThread([clientX, clientY, items = std::move(items)] {
            auto& h = BrowserHost::get();
            if (h.onContextMenu) h.onContextMenu(clientX, clientY, items);
            else h.contextMenuCancel();
        });
        return true;
    }

    bool OnProcessMessageReceived(
        CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
        CefProcessId source, CefRefPtr<CefProcessMessage> message
    ) override {
        auto& router = BrowserHost::get().m_router;
        return router && router->OnProcessMessageReceived(browser, frame, source, message);
    }

    // -- CefRenderHandler (called on the CEF UI thread) --

    // View coordinates are DIP: window client pixels divided by the DPI scale.

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        auto& host = BrowserHost::get();
        auto bounds = host.boundsSnapshot();
        float scale = host.dpiScale();
        rect.Set(0, 0,
            std::max<int>(1, static_cast<int>(std::ceil((bounds.right - bounds.left) / scale))),
            std::max<int>(1, static_cast<int>(std::ceil((bounds.bottom - bounds.top) / scale))));
    }

    bool GetScreenPoint(
        CefRefPtr<CefBrowser>, int viewX, int viewY, int& screenX, int& screenY
    ) override {
        auto& host = BrowserHost::get();
        auto bounds = host.boundsSnapshot();
        float scale = host.dpiScale();
        POINT point{
            bounds.left + static_cast<LONG>(viewX * scale),
            bounds.top + static_cast<LONG>(viewY * scale)};
        ClientToScreen(host.parentWindow(), &point);
        screenX = point.x;
        screenY = point.y;
        return true;
    }

    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& info) override {
        CefRect view;
        GetViewRect(browser, view);
        info.device_scale_factor = BrowserHost::get().renderScale();
        info.depth = 32;
        info.depth_per_component = 8;
        info.is_monochrome = 0;
        info.rect = view;
        info.available_rect = view;
        return true;
    }

    void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override {
        std::scoped_lock lock(m_surface->mutex);
        m_surface->popupShown = show;
        if (!show) m_surface->popupPixels.clear();
        m_surface->dirty = true;
    }

    void OnPopupSize(CefRefPtr<CefBrowser>, CefRect const& rect) override {
        std::scoped_lock lock(m_surface->mutex);
        m_surface->popupRect = rect;
    }

    void OnPaint(
        CefRefPtr<CefBrowser> browser, PaintElementType type, RectList const&,
        void const* buffer, int width, int height
    ) override {
        if (type == PET_VIEW) {
            // If this frame was laid out for an outdated viewport, ask the
            // renderer to size itself again (we're already on the UI thread).
            CefRect view;
            GetViewRect(browser, view);
            float scale = BrowserHost::get().renderScale();
            int expectedW = static_cast<int>(view.width * scale);
            int expectedH = static_cast<int>(view.height * scale);
            if (std::abs(width - expectedW) > 2 || std::abs(height - expectedH) > 2) {
                browser->GetHost()->WasResized();
            }
        }
        auto size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        std::scoped_lock lock(m_surface->mutex);
        if (type == PET_VIEW) {
            m_surface->pixels.assign(
                static_cast<uint8_t const*>(buffer), static_cast<uint8_t const*>(buffer) + size);
            m_surface->width = width;
            m_surface->height = height;
            m_surface->scale = BrowserHost::get().renderScale();
        } else {
            m_surface->popupPixels.assign(
                static_cast<uint8_t const*>(buffer), static_cast<uint8_t const*>(buffer) + size);
            m_surface->popupRect.width = width;
            m_surface->popupRect.height = height;
        }
        m_surface->dirty = true;
    }

    // -- CefLifeSpanHandler --

    bool OnBeforePopup(
        CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, CefString const& targetUrl,
        CefString const&, WindowOpenDisposition, bool, CefPopupFeatures const&,
        CefWindowInfo&, CefRefPtr<CefClient>&, CefBrowserSettings&,
        CefRefPtr<CefDictionaryValue>&, bool*
    ) override {
        std::string url = targetUrl.ToString();
        geode::queueInMainThread([url] { BrowserHost::get().addTab(url); });
        return true;  // open in a new in-game tab instead of a native window
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        geode::queueInMainThread([self = CefRefPtr(this), browser] {
            auto& host = BrowserHost::get();
            if (auto* tab = host.tabForClient(self.get())) {
                tab->browser = browser;
                tab->ready = true;
                host.updateVisibility();
                // The browser was likely created before the real bounds were
                // known; make the renderer re-query GetViewRect now.
                runOnUI([browser] { browser->GetHost()->WasResized(); });
                host.notify();
            } else {
                browser->GetHost()->CloseBrowser(true);
            }
        });
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        if (auto& router = BrowserHost::get().m_router) router->OnBeforeClose(browser);
        // The browser has finished closing (beforeunload already resolved);
        // now drop its tab from the model.
        geode::queueInMainThread([self = CefRefPtr(this)] {
            BrowserHost::get().removeTabByClient(self.get());
        });
    }

    // -- CefDisplayHandler --

    void OnAddressChange(
        CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefString const& url
    ) override {
        if (!frame->IsMain()) return;
        geode::queueInMainThread([self = CefRefPtr(this), url = url.ToString()] {
            auto& host = BrowserHost::get();
            if (auto* tab = host.tabForClient(self.get())) {
                tab->url = url;
                host.recordHistory(url);
                host.notify();
            }
        });
    }

    void OnTitleChange(CefRefPtr<CefBrowser>, CefString const& title) override {
        geode::queueInMainThread([self = CefRefPtr(this), title = title.ToString()] {
            auto& host = BrowserHost::get();
            if (auto* tab = host.tabForClient(self.get())) {
                tab->title = title.empty() ? "New tab" : title;
                host.notify();
            }
        });
    }

    bool OnCursorChange(
        CefRefPtr<CefBrowser>, CefCursorHandle cursor, cef_cursor_type_t, CefCursorInfo const&
    ) override {
        BrowserHost::get().m_cursor = cursor;
        return true;
    }

    // -- CefLoadHandler --

    void OnLoadingStateChange(
        CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack, bool canGoForward
    ) override {
        if (!isLoading) {
            // Install the pointer-lock reporter once the document is ready.
            if (auto frame = browser->GetMainFrame()) {
                frame->ExecuteJavaScript(kPointerLockScript, frame->GetURL(), 0);
            }
        }
        geode::queueInMainThread([self = CefRefPtr(this), isLoading, canGoBack, canGoForward] {
            auto& host = BrowserHost::get();
            if (auto* tab = host.tabForClient(self.get())) {
                tab->loading = isLoading;
                tab->canGoBack = canGoBack;
                tab->canGoForward = canGoForward;
                host.notify();
            }
        });
    }

    // -- CefPermissionHandler --

    bool OnRequestMediaAccessPermission(
        CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefString const&,
        uint32_t requestedPermissions, CefRefPtr<CefMediaAccessCallback> callback
    ) override {
        if (Mod::get()->getSettingValue<bool>("allow-microphone")) {
            callback->Continue(requestedPermissions);
        } else {
            callback->Cancel();
        }
        return true;
    }

    bool OnShowPermissionPrompt(
        CefRefPtr<CefBrowser>, uint64_t, CefString const&,
        uint32_t requestedPermissions, CefRefPtr<CefPermissionPromptCallback> callback
    ) override {
        // Grant pointer/keyboard lock so browser games (e.g.
        // classic.minecraft.net) can capture input without a native prompt.
        debugLog(std::format("OnShowPermissionPrompt: perms=0x{:X}", requestedPermissions));
        constexpr uint32_t kAutoGrant =
            CEF_PERMISSION_TYPE_POINTER_LOCK | CEF_PERMISSION_TYPE_KEYBOARD_LOCK;
        if (requestedPermissions != 0 && (requestedPermissions & ~kAutoGrant) == 0) {
            callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
            return true;
        }
        return false;
    }

    // -- CefJSDialogHandler: render page dialogs in-game instead of as native
    // windows, which would minimize the fullscreen game. --

    bool OnJSDialog(
        CefRefPtr<CefBrowser>, CefString const&, JSDialogType dialogType,
        CefString const& messageText, CefString const& defaultPromptText,
        CefRefPtr<CefJSDialogCallback> callback, bool&
    ) override {
        auto message = messageText.ToString();
        if (dialogType == JSDIALOGTYPE_ALERT) {
            showPageDialog("Page says", std::move(message), "OK", nullptr, [callback](bool) {
                runOnUI([callback] { callback->Continue(true, ""); });
            });
        } else {
            // Confirm and prompt; a prompt submits its default text on OK.
            auto defaultText = defaultPromptText.ToString();
            showPageDialog("Page asks", std::move(message), "Cancel", "OK",
                [callback, defaultText](bool ok) {
                    runOnUI([callback, ok, defaultText] { callback->Continue(ok, defaultText); });
                });
        }
        return true;
    }

    bool OnBeforeUnloadDialog(
        CefRefPtr<CefBrowser>, CefString const&, bool, CefRefPtr<CefJSDialogCallback> callback
    ) override {
        showPageDialog("Leave site?", "Changes you made may not be saved.", "Cancel", "Leave",
            [callback](bool leave) {
                runOnUI([callback, leave] { callback->Continue(leave, ""); });
            });
        return true;
    }

    // -- CefDownloadHandler: save to the OS Downloads folder without a native
    // save dialog, and report progress to the in-game downloads list. --

    // Chrome extensions (.crx) are unsupported by CEF's off-screen runtime and
    // installing one crashes Chromium, so refuse those downloads outright.
    bool CanDownload(CefRefPtr<CefBrowser>, CefString const& url, CefString const&) override {
        auto u = url.ToString();
        return u.find(".crx") == std::string::npos;
    }

    bool OnBeforeDownload(
        CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item,
        CefString const& suggestedName, CefRefPtr<CefBeforeDownloadCallback> callback
    ) override {
        auto target = downloadsDir() / suggestedName.ToString();
        callback->Continue(target.wstring(), false);
        pushDownload(item, suggestedName.ToString(), target.string());
        return true;
    }

    void OnDownloadUpdated(
        CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item, CefRefPtr<CefDownloadItemCallback>
    ) override {
        pushDownload(item, item->GetSuggestedFileName().ToString(), item->GetFullPath().ToString());
    }

private:
    static std::filesystem::path downloadsDir() {
        // %USERPROFILE%\Downloads; fall back to the mod save dir.
        if (auto const* profile = std::getenv("USERPROFILE")) {
            auto dir = std::filesystem::path(profile) / "Downloads";
            std::error_code ec;
            if (std::filesystem::exists(dir, ec)) return dir;
        }
        return Mod::get()->getSaveDir();
    }

    void pushDownload(CefRefPtr<CefDownloadItem> item, std::string name, std::string path) {
        BrowserHost::DownloadInfo info;
        info.id = item->GetId();
        // GetSuggestedFileName can be empty in later updates; fall back to the
        // path's filename so the entry never shows up blank.
        if (name.empty() && !path.empty()) {
            info.name = std::filesystem::path(path).filename().string();
        } else {
            info.name = name;
        }
        info.path = path;
        info.percent = item->GetPercentComplete();
        info.complete = item->IsComplete();
        info.canceled = item->IsCanceled();
        geode::queueInMainThread([info] { BrowserHost::get().updateDownload(info); });
    }

public:

private:
    std::shared_ptr<BrowserSurface> m_surface;

    IMPLEMENT_REFCOUNTING(GdCefClient);
};

BrowserHost& BrowserHost::get() {
    static BrowserHost instance;
    return instance;
}

HWND BrowserHost::findGameWindow() const {
    if (auto active = GetActiveWindow()) return GetAncestor(active, GA_ROOT);
    WindowSearch search{GetCurrentProcessId()};
    EnumWindows(enumWindow, reinterpret_cast<LPARAM>(&search));
    return search.hwnd;
}

bool BrowserHost::initialize() {
    if (m_cefStarted) return true;
    if (m_cefFailed) return false;

    if (m_history.empty() && m_downloads.empty()) loadPersisted();

    m_parent = findGameWindow();
    if (!m_parent) {
        m_error = "Could not find the Geometry Dash window";
        notify();
        return false;
    }

    if (!startCef()) {
        m_cefFailed = true;
        notify();
        return false;
    }

    m_cefStarted = true;
    m_error.clear();
    installWndProcHook();

    if (m_pendingUrls.empty() && m_tabs.empty()) {
        m_pendingUrls.push_back(Mod::get()->getSettingValue<std::string>("home-page"));
    }
    auto pending = std::move(m_pendingUrls);
    m_pendingUrls.clear();
    for (auto const& url : pending) addTab(url);
    notify();
    return true;
}

bool BrowserHost::startCef() {
    namespace fs = std::filesystem;
    auto resources = Mod::get()->getResourcesDir();
    auto libcef = resources / "libcef.dll";
    if (!fs::exists(libcef)) {
        m_error = "Chromium files are missing from the mod's resources";
        return false;
    }
    // The mod links libcef.dll via /DELAYLOAD; loading it by full path here
    // lets the delay-load resolve against this module. libcef.dll's own
    // dependencies (chrome_elf.dll etc.) sit next to it, so the search path
    // must include its directory, not just the game's.
    if (!LoadLibraryExW(
        libcef.wstring().c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
    )) {
        m_error = std::format("Could not load libcef.dll (error {})", GetLastError());
        return false;
    }

    CefMainArgs args(GetModuleHandleW(nullptr));
    CefSettings settings;
    settings.no_sandbox = 1;
    settings.windowless_rendering_enabled = 1;
    settings.multi_threaded_message_loop = 1;
    settings.background_color = 0xFFFFFFFF;
    settings.log_severity = LOGSEVERITY_WARNING;
    CefString(&settings.browser_subprocess_path) = (resources / "chromium-in-gd-helper.exe").wstring();
    CefString(&settings.resources_dir_path) = resources.wstring();
    CefString(&settings.locales_dir_path) = resources.wstring();
    CefString(&settings.locale) = "en-US";
    CefString(&settings.cache_path) = (Mod::get()->getSaveDir() / "chromium-profile").wstring();
    CefString(&settings.log_file) = (Mod::get()->getSaveDir() / "cef.log").wstring();

    if (!CefInitialize(args, settings, new GdCefApp(), nullptr)) {
        m_error = "Chromium failed to start (see cef.log in the mod's save folder)";
        return false;
    }

    // Message router: lets injected page JS notify us of pointer-lock changes.
    m_router = CefMessageRouterBrowserSide::Create(CefMessageRouterConfig());
    m_queryHandler = std::make_unique<PointerLockQueryHandler>();
    runOnUI([this] { m_router->AddHandler(m_queryHandler.get(), false); });
    // CefShutdown is deliberately never called: from an injected DLL it would
    // run under the loader lock and deadlock. Chromium subprocesses exit on
    // their own when the game process ends.
    return true;
}

void BrowserHost::shutdown() {
    for (auto& tab : m_tabs) {
        if (tab->browser) {
            runOnUI([browser = tab->browser] { browser->GetHost()->CloseBrowser(true); });
        }
    }
    m_tabs.clear();
    m_pendingUrls.clear();
    m_active = 0;
    m_keyboardFocus = false;
    notify();
}

// Each tab runs its own renderer process and paint surface; keep a sane cap.
constexpr size_t kMaxTabs = 8;

void BrowserHost::addTab(std::string const& requestedUrl) {
    if (m_tabs.size() >= kMaxTabs) {
        log::warn("Tab limit of {} reached, not opening a new tab", kMaxTabs);
        return;
    }
    // The first tab opens the home page; tabs added later open the new-tab page.
    auto url = requestedUrl;
    if (url.empty()) {
        bool firstTab = m_tabs.empty() && m_pendingUrls.empty();
        url = Mod::get()->getSettingValue<std::string>(firstTab ? "home-page" : "new-tab-page");
    }
    if (!m_cefStarted) {
        m_pendingUrls.push_back(url);
        initialize();
        notify();
        return;
    }

    auto tab = std::make_unique<Tab>();
    tab->url = normalizeAddress(url);
    tab->surface = std::make_shared<BrowserSurface>();
    tab->client = new GdCefClient(tab->surface);
    m_tabs.push_back(std::move(tab));
    m_active = m_tabs.size() - 1;
    createBrowserForTab(m_active);
    updateVisibility();
    notify();
}

void BrowserHost::createBrowserForTab(size_t index) {
    auto& tab = *m_tabs[index];
    CefWindowInfo windowInfo;
    windowInfo.SetAsWindowless(m_parent);
    CefBrowserSettings settings;
    settings.windowless_frame_rate = 60;
    settings.background_color = 0xFFFFFFFF;
    if (!CefBrowserHost::CreateBrowser(windowInfo, tab.client, tab.url, settings, nullptr, nullptr)) {
        m_error = "Could not create a browser tab";
        notify();
    }
}

BrowserHost::Tab* BrowserHost::tabForClient(GdCefClient* client) {
    for (auto& tab : m_tabs) {
        if (tab->client.get() == client) return tab.get();
    }
    return nullptr;
}

void BrowserHost::closeActiveTab() {
    closeTab(m_active);
}

void BrowserHost::closeTab(size_t index) {
    if (index >= m_tabs.size()) return;
    if (auto browser = m_tabs[index]->browser) {
        // CloseBrowser(false) runs the page's beforeunload handler, so a
        // "Leave site?" dialog can still cancel the close. The tab is removed
        // from the model in OnBeforeClose once the close actually completes.
        runOnUI([browser] { browser->GetHost()->CloseBrowser(false); });
        return;
    }
    // No live browser (still starting): drop it immediately.
    m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(index));
    if (m_tabs.empty()) { addTab(); return; }
    m_active = std::min(m_active, m_tabs.size() - 1);
    updateVisibility();
    notify();
}

void BrowserHost::removeTabByClient(GdCefClient* client) {
    auto it = std::find_if(m_tabs.begin(), m_tabs.end(), [client](auto const& tab) {
        return tab->client.get() == client;
    });
    if (it == m_tabs.end()) return;
    m_tabs.erase(it);
    if (m_tabs.empty()) { addTab(); return; }
    m_active = std::min(m_active, m_tabs.size() - 1);
    updateVisibility();
    notify();
}

void BrowserHost::selectTab(size_t index) {
    if (index >= m_tabs.size()) return;
    m_active = index;
    updateVisibility();
    notify();
}

void BrowserHost::navigate(std::string text) {
    auto* tab = m_tabs.empty() ? nullptr : m_tabs[m_active].get();
    if (!tab || !tab->browser) return;
    tab->browser->GetMainFrame()->LoadURL(normalizeAddress(std::move(text)));
}

void BrowserHost::goBack() {
    if (auto const* tab = activeTab(); tab && tab->browser) tab->browser->GoBack();
}

void BrowserHost::goForward() {
    if (auto const* tab = activeTab(); tab && tab->browser) tab->browser->GoForward();
}

void BrowserHost::reload() {
    if (auto const* tab = activeTab(); tab && tab->browser) tab->browser->Reload();
}

void BrowserHost::toggleMute() {
    auto* tab = m_tabs.empty() ? nullptr : m_tabs[m_active].get();
    if (!tab || !tab->browser) return;
    tab->muted = !tab->muted;
    runOnUI([browser = tab->browser, muted = tab->muted] {
        browser->GetHost()->SetAudioMuted(muted);
    });
    notify();
}

void BrowserHost::openDevTools() {
    auto const* tab = activeTab();
    if (!tab || !tab->browser) return;
    runOnUI([browser = tab->browser] {
        CefWindowInfo windowInfo;
        windowInfo.SetAsPopup(nullptr, "DevTools");
        browser->GetHost()->ShowDevTools(windowInfo, nullptr, CefBrowserSettings(), CefPoint());
    });
}

void BrowserHost::setBounds(RECT bounds) {
    bool resized;
    {
        std::scoped_lock lock(m_boundsMutex);
        resized = (bounds.right - bounds.left) != (m_bounds.right - m_bounds.left)
               || (bounds.bottom - bounds.top) != (m_bounds.bottom - m_bounds.top);
        m_bounds = bounds;
    }
    if (!resized) return;
    for (auto& tab : m_tabs) {
        if (tab->browser) {
            runOnUI([browser = tab->browser] { browser->GetHost()->WasResized(); });
        }
    }
}

void BrowserHost::setWindowBounds(RECT bounds) {
    std::scoped_lock lock(m_boundsMutex);
    m_windowBounds = bounds;
}

RECT BrowserHost::boundsSnapshot() const {
    std::scoped_lock lock(m_boundsMutex);
    return m_bounds;
}

float BrowserHost::dpiScale() const {
    if (!m_parent) return 1.f;
    UINT dpi = GetDpiForWindow(m_parent);
    return dpi ? static_cast<float>(dpi) / 96.f : 1.f;
}

float BrowserHost::renderScale() const {
    // 2x super-sampling: sharp text with a manageable upload size.
    return dpiScale() * 2.0f;
}

void BrowserHost::setVisible(bool visible) {
    m_visible = visible;
    if (!visible) setKeyboardFocus(false);
    updateVisibility();
}

void BrowserHost::updateVisibility() {
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        if (!m_tabs[i]->browser) continue;
        bool shown = m_visible && i == m_active;
        // WasHidden(true) only pauses painting; audio keeps playing, so
        // hidden tabs still carry voice chat.
        runOnUI([browser = m_tabs[i]->browser, hidden = !shown] {
            browser->GetHost()->WasHidden(hidden);
        });
    }
}

bool BrowserHost::copyActiveFrame(std::vector<uint8_t>& out, int& width, int& height) {
    auto const* tab = activeTab();
    if (!tab || !tab->surface) return false;
    auto& surface = *tab->surface;
    std::scoped_lock lock(surface.mutex);
    if (!surface.dirty || surface.width <= 0 || surface.height <= 0) return false;
    if (surface.pixels.size() != static_cast<size_t>(surface.width) * surface.height * 4) return false;

    out = surface.pixels;
    width = surface.width;
    height = surface.height;

    // Composite the widget popup (dropdown menus etc.) over the view.
    // The popup rect is in DIP; the pixel buffers are physical pixels.
    if (surface.popupShown && !surface.popupPixels.empty()) {
        auto rect = surface.popupRect;
        rect.x = static_cast<int>(rect.x * surface.scale);
        rect.y = static_cast<int>(rect.y * surface.scale);
        for (int row = 0; row < rect.height; ++row) {
            int destY = rect.y + row;
            if (destY < 0 || destY >= height) continue;
            int srcX = std::max(0, -rect.x);
            int destX = rect.x + srcX;
            int columns = std::min(rect.width - srcX, width - destX);
            if (columns <= 0) continue;
            std::memcpy(
                out.data() + (static_cast<size_t>(destY) * width + destX) * 4,
                surface.popupPixels.data() + (static_cast<size_t>(row) * rect.width + srcX) * 4,
                static_cast<size_t>(columns) * 4);
        }
    }
    surface.dirty = false;
    return true;
}

void BrowserHost::loadPersisted() {
    m_history = Mod::get()->getSavedValue<std::vector<std::string>>("browser-history", {});
    auto names = Mod::get()->getSavedValue<std::vector<std::string>>("dl-names", {});
    auto paths = Mod::get()->getSavedValue<std::vector<std::string>>("dl-paths", {});
    for (size_t i = 0; i < names.size(); ++i) {
        DownloadInfo d;
        d.id = 0;  // past downloads have no live id
        d.name = names[i];
        d.path = i < paths.size() ? paths[i] : "";
        d.percent = 100;
        d.complete = true;
        m_downloads.push_back(d);
    }
}

void BrowserHost::saveDownloads() {
    std::vector<std::string> names, paths;
    for (auto const& d : m_downloads) { names.push_back(d.name); paths.push_back(d.path); }
    Mod::get()->setSavedValue("dl-names", names);
    Mod::get()->setSavedValue("dl-paths", paths);
}

void BrowserHost::updateDownload(DownloadInfo const& info) {
    auto it = std::find_if(m_downloads.begin(), m_downloads.end(),
        [&](auto const& d) { return d.id == info.id && d.id != 0; });
    auto notice = [this](std::string msg) { if (onDownloadNotice) onDownloadNotice(std::move(msg)); };
    if (it == m_downloads.end()) {
        m_downloads.push_back(info);
        if (info.complete) notice("Downloaded " + info.name);
        else notice("Downloading " + info.name + "...");
    } else {
        bool wasComplete = it->complete;
        auto name = info.name.empty() ? it->name : info.name;  // keep known name
        *it = info;
        it->name = name;
        if (info.complete && !wasComplete) notice("Downloaded " + name);
        else if (info.canceled) notice("Download canceled");
    }
    saveDownloads();
    notify();
}

void BrowserHost::recordHistory(std::string const& url) {
    if (url.empty() || url == "about:blank") return;
    if (!m_history.empty() && m_history.back() == url) return;
    m_history.push_back(url);
    if (m_history.size() > 200) m_history.erase(m_history.begin());
    Mod::get()->setSavedValue("browser-history", m_history);
}

BrowserHost::Tab const* BrowserHost::activeTab() const {
    if (m_tabs.empty() || m_active >= m_tabs.size()) return nullptr;
    return m_tabs[m_active].get();
}

BrowserHost::Tab const* BrowserHost::tabAt(size_t index) const {
    if (index >= m_tabs.size()) return nullptr;
    return m_tabs[index].get();
}

// -- input --

bool BrowserHost::isCursorOverView() const {
    if (!m_visible || !m_parent || m_tabs.empty()) return false;
    POINT point{};
    GetCursorPos(&point);
    ScreenToClient(m_parent, &point);
    return pointInView(point.x, point.y);
}

bool BrowserHost::isCursorOverWindow() const {
    if (!m_visible || !m_parent) return false;
    POINT point{};
    GetCursorPos(&point);
    ScreenToClient(m_parent, &point);
    RECT b;
    {
        std::scoped_lock lock(m_boundsMutex);
        b = m_windowBounds;
    }
    return point.x >= b.left && point.x < b.right
        && point.y >= b.top && point.y < b.bottom;
}

bool BrowserHost::pointInView(int clientX, int clientY) const {
    auto bounds = boundsSnapshot();
    return clientX >= bounds.left && clientX < bounds.right
        && clientY >= bounds.top && clientY < bounds.bottom;
}

void BrowserHost::sendMouseButton(int clientX, int clientY, bool up, int clickCount) {
    auto const* tab = activeTab();
    if (!tab || !tab->browser) return;
    auto bounds = boundsSnapshot();
    float scale = dpiScale();
    // While pointer-locked, clicks must land at the locked virtual position so
    // they don't register as a cursor jump (which yanks the camera).
    int px = m_pointerLocked ? m_lockVirtual.x : clientX;
    int py = m_pointerLocked ? m_lockVirtual.y : clientY;
    CefMouseEvent event;
    event.x = static_cast<int>((px - bounds.left) / scale);
    event.y = static_cast<int>((py - bounds.top) / scale);
    event.modifiers = currentModifiers();
    runOnUI([browser = tab->browser, event, up, clickCount] {
        browser->GetHost()->SendMouseClickEvent(event, MBT_LEFT, up, clickCount);
    });
}

void BrowserHost::sendMouseDrag(int clientX, int clientY) {
    // While locked the WndProc mouse-move path drives motion; ignore drags.
    if (m_pointerLocked) return;
    auto const* tab = activeTab();
    if (!tab || !tab->browser) return;
    auto bounds = boundsSnapshot();
    float scale = dpiScale();
    CefMouseEvent event;
    event.x = static_cast<int>((clientX - bounds.left) / scale);
    event.y = static_cast<int>((clientY - bounds.top) / scale);
    event.modifiers = currentModifiers() | EVENTFLAG_LEFT_MOUSE_BUTTON;
    runOnUI([browser = tab->browser, event] {
        browser->GetHost()->SendMouseMoveEvent(event, false);
    });
}

void BrowserHost::setPointerLocked(bool locked) {
    debugLog(std::format("setPointerLocked({})", locked));
    if (m_pointerLocked == locked) return;
    m_pointerLocked = locked;
    if (locked) {
        auto b = boundsSnapshot();
        m_lockCenter = {(b.left + b.right) / 2, (b.top + b.bottom) / 2};
        m_lockVirtual = m_lockCenter;
        POINT screen = m_lockCenter;
        if (m_parent) {
            ClientToScreen(m_parent, &screen);
            SetCursorPos(screen.x, screen.y);
        }
        setKeyboardFocus(true);  // pointer lock implies the page wants keys too
    }
    // WM_SETCURSOR handles hiding/showing the cursor based on m_pointerLocked.
}

void BrowserHost::setKeyboardFocus(bool focused) {
    debugLog(std::string("setKeyboardFocus ") + (focused ? "true" : "false"));
    if (!focused && m_pointerLocked) {
        // Tell the page to drop its (faked) pointer lock, then stop emulation.
        if (auto const* tab = activeTab(); tab && tab->browser) {
            runOnUI([browser = tab->browser] {
                browser->GetMainFrame()->ExecuteJavaScript(
                    "document.exitPointerLock && document.exitPointerLock()", "", 0);
            });
        }
        m_pointerLocked = false;
    }
    if (m_keyboardFocus == focused) return;
    m_keyboardFocus = focused;
    if (auto const* tab = activeTab(); tab && tab->browser) {
        runOnUI([browser = tab->browser, focused] {
            browser->GetHost()->SetFocus(focused);
        });
    }
}

void BrowserHost::installWndProcHook() {
    if (m_prevWndProc || !m_parent) return;
    m_prevWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        m_parent, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&BrowserHost::hookedWndProc)));
}

void BrowserHost::removeWndProcHook() {
    if (!m_prevWndProc || !m_parent) return;
    SetWindowLongPtrW(m_parent, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_prevWndProc));
    m_prevWndProc = nullptr;
}

LRESULT CALLBACK BrowserHost::hookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& host = BrowserHost::get();
    bool handled = false;
    LRESULT result = host.handleMessage(hwnd, msg, wParam, lParam, handled);
    if (handled) return result;
    return CallWindowProcW(host.m_prevWndProc, hwnd, msg, wParam, lParam);
}

void BrowserHost::forwardMouse(UINT msg, int clientX, int clientY) {
    auto const* tab = activeTab();
    if (!tab || !tab->browser) return;
    auto bounds = boundsSnapshot();
    float scale = dpiScale();
    CefMouseEvent event;
    event.x = static_cast<int>((clientX - bounds.left) / scale);
    event.y = static_cast<int>((clientY - bounds.top) / scale);
    event.modifiers = currentModifiers();

    auto browser = tab->browser;
    switch (msg) {
        case WM_MOUSEMOVE:
            runOnUI([browser, event] { browser->GetHost()->SendMouseMoveEvent(event, false); });
            break;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            runOnUI([browser, event, up = (msg == WM_RBUTTONUP)] {
                browser->GetHost()->SendMouseClickEvent(event, MBT_RIGHT, up, 1);
            });
            break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            runOnUI([browser, event, up = (msg == WM_MBUTTONUP)] {
                browser->GetHost()->SendMouseClickEvent(event, MBT_MIDDLE, up, 1);
            });
            break;
        default:
            break;
    }
}

LRESULT BrowserHost::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool& handled) {
    if (!m_visible || m_tabs.empty()) return 0;

    switch (msg) {
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            if (m_pointerLocked) {
                // Emulated pointer lock: feed accumulated motion to Chromium
                // (so it computes movementX/Y) and snap the OS cursor back to
                // the view center so it never escapes the window.
                if (x == m_lockCenter.x && y == m_lockCenter.y) { handled = true; return 0; }
                m_lockVirtual.x += x - m_lockCenter.x;
                m_lockVirtual.y += y - m_lockCenter.y;
                auto const* tab = activeTab();
                auto bounds = boundsSnapshot();
                float scale = dpiScale();
                // Keep the sent coordinate inside the view (Chromium clamps to
                // it) by warping across when near an edge. The warp frame's
                // large delta is dropped by the injected JS filter.
                constexpr LONG margin = 80;
                LONG spanX = (bounds.right - bounds.left) - 2 * margin;
                LONG spanY = (bounds.bottom - bounds.top) - 2 * margin;
                if (spanX > 0) {
                    while (m_lockVirtual.x > bounds.right - margin) m_lockVirtual.x -= spanX;
                    while (m_lockVirtual.x < bounds.left + margin) m_lockVirtual.x += spanX;
                }
                if (spanY > 0) {
                    while (m_lockVirtual.y > bounds.bottom - margin) m_lockVirtual.y -= spanY;
                    while (m_lockVirtual.y < bounds.top + margin) m_lockVirtual.y += spanY;
                }
                if (tab && tab->browser) {
                    CefMouseEvent event;
                    event.x = static_cast<int>((m_lockVirtual.x - bounds.left) / scale);
                    event.y = static_cast<int>((m_lockVirtual.y - bounds.top) / scale);
                    event.modifiers = currentModifiers();
                    runOnUI([browser = tab->browser, event] {
                        browser->GetHost()->SendMouseMoveEvent(event, false);
                    });
                }
                POINT screen = m_lockCenter;
                ClientToScreen(hwnd, &screen);
                SetCursorPos(screen.x, screen.y);
                handled = true;
                return 0;
            }
            if (pointInView(x, y)) forwardMouse(msg, x, y);
            return 0;  // not handled: the game still sees mouse moves
        }

        case WM_MBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            // Middle-click on a tab closes it; otherwise forward to the page.
            if (onMiddleClick && onMiddleClick(x, y)) { handled = true; return 0; }
            if (pointInView(x, y)) { forwardMouse(msg, x, y); handled = true; }
            return 0;
        }

        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP: {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            if (pointInView(x, y)) {
                forwardMouse(msg, x, y);
                handled = true;
            }
            return 0;
        }

        case WM_XBUTTONDOWN: {
            // Mouse side buttons navigate the browser (back / forward) when the
            // cursor is over the page.
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pointInView(point.x, point.y)) {
                WORD button = GET_XBUTTON_WPARAM(wParam);
                if (button == XBUTTON1) goBack();
                else if (button == XBUTTON2) goForward();
                handled = true;
                return TRUE;
            }
            return 0;
        }

        // Keyboard focus is managed by the cocos touch handler (which knows the
        // whole window, not just the page rect), so WM_LBUTTONDOWN does nothing
        // here — dropping focus on toolbar clicks was releasing it too eagerly.

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            if (!pointInView(point.x, point.y)) return 0;
            auto const* tab = activeTab();
            if (!tab || !tab->browser) return 0;
            auto bounds = boundsSnapshot();
            float scale = dpiScale();
            CefMouseEvent event;
            event.x = static_cast<int>((point.x - bounds.left) / scale);
            event.y = static_cast<int>((point.y - bounds.top) / scale);
            event.modifiers = currentModifiers();
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            runOnUI([browser = tab->browser, event, delta, horizontal = (msg == WM_MOUSEHWHEEL)] {
                browser->GetHost()->SendMouseWheelEvent(event, horizontal ? delta : 0, horizontal ? 0 : delta);
            });
            handled = true;
            return 0;
        }

        case WM_SETCURSOR: {
            if (m_pointerLocked) {  // hide the cursor while the page has it locked
                SetCursor(nullptr);
                handled = true;
                return TRUE;
            }
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            if (pointInView(point.x, point.y) && m_cursor) {
                SetCursor(m_cursor);
                handled = true;
                return TRUE;
            }
            return 0;
        }

        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SYSCHAR: {
            if (!m_keyboardFocus) return 0;
            // Plain Esc goes to the page (closing modals, game menus etc.);
            // Shift+Esc gives the keyboard back to the game.
            if (msg == WM_KEYDOWN && wParam == VK_ESCAPE && (GetKeyState(VK_SHIFT) & 0x8000)) {
                setKeyboardFocus(false);
                handled = true;
                return 0;
            }
            auto const* tab = activeTab();
            if (!tab || !tab->browser) return 0;
            CefKeyEvent event;
            event.windows_key_code = static_cast<int>(wParam);
            event.native_key_code = static_cast<int>(lParam);
            event.is_system_key = (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_SYSCHAR);
            event.modifiers = currentModifiers();
            if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) event.type = KEYEVENT_RAWKEYDOWN;
            else if (msg == WM_KEYUP || msg == WM_SYSKEYUP) event.type = KEYEVENT_KEYUP;
            else event.type = KEYEVENT_CHAR;
            runOnUI([browser = tab->browser, event] {
                browser->GetHost()->SendKeyEvent(event);
            });
            handled = true;  // swallow so GD does not react to typing
            return 0;
        }

        default:
            return 0;
    }
}

std::string BrowserHost::normalizeAddress(std::string text) const {
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](unsigned char c) { return !std::isspace(c); }));
    text.erase(std::find_if(text.rbegin(), text.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), text.end());
    if (text.empty()) return "about:blank";
    if (text.find("://") != std::string::npos || text.starts_with("about:") || text.starts_with("file:")) return text;
    if (text.find(' ') == std::string::npos && text.find('.') != std::string::npos) return "https://" + text;
    std::string encoded;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') encoded += static_cast<char>(c);
        else if (c == ' ') encoded += '+';
        else encoded += std::format("%{:02X}", c);
    }
    return "https://www.google.com/search?q=" + encoded;
}

void BrowserHost::notify() {
    if (onStateChanged) onStateChanged();
}

void BrowserHost::logDebug(std::string const& line) {
    debugLog(line);
}

void BrowserHost::contextMenuCommand(int commandId) {
    if (auto cb = m_contextCallback) {
        runOnUI([cb, commandId] { cb->Continue(commandId, EVENTFLAG_NONE); });
        m_contextCallback = nullptr;
    }
}

void BrowserHost::contextMenuCancel() {
    if (auto cb = m_contextCallback) {
        runOnUI([cb] { cb->Cancel(); });
        m_contextCallback = nullptr;
    }
}

#endif
