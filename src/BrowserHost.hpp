#pragma once

#ifdef GEODE_IS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <include/cef_base.h>
#include <include/cef_browser.h>
#include <include/cef_context_menu_handler.h>
#include <include/wrapper/cef_message_router.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class GdCefClient;

// Pixel surface a tab paints into from the CEF UI thread. The game thread
// reads it when uploading to the GL texture, hence the mutex.
struct BrowserSurface {
    std::mutex mutex;
    std::vector<uint8_t> pixels;  // BGRA, top-down
    int width = 0;
    int height = 0;
    float scale = 1.f;  // DPI scale the frame was painted at
    bool dirty = false;
    // Widget popups (select dropdowns etc.) are painted separately in OSR
    // and composited over the view during upload.
    std::vector<uint8_t> popupPixels;
    CefRect popupRect;
    bool popupShown = false;
};

class BrowserHost final {
public:
    struct Tab {
        CefRefPtr<CefBrowser> browser;
        CefRefPtr<GdCefClient> client;
        std::shared_ptr<BrowserSurface> surface;
        std::string title = "New tab";
        std::string url;
        bool muted = false;
        bool ready = false;
        bool loading = false;
        bool canGoBack = false;
        bool canGoForward = false;
    };

    struct DownloadInfo {
        uint32_t id = 0;
        std::string name;
        std::string path;
        int percent = 0;
        bool complete = false;
        bool canceled = false;
    };

    static BrowserHost& get();

    bool initialize();
    void shutdown();
    void addTab(std::string const& url = {});
    void closeActiveTab();
    void closeTab(size_t index);
    void selectTab(size_t index);
    void navigate(std::string text);
    void goBack();
    void goForward();
    void reload();
    void toggleMute();
    void openDevTools();
    void setBounds(RECT bounds);
    // Full window rect (including chrome), client pixels. Used to block game
    // input anywhere over the window, not just over the rendered page.
    void setWindowBounds(RECT bounds);
    void setVisible(bool visible);

    // Copies the active tab's latest frame (BGRA, top-down) if it changed
    // since the last call. Returns false if there is nothing new.
    bool copyActiveFrame(std::vector<uint8_t>& out, int& width, int& height);

    // Mouse input in game-window client pixels (same space as setBounds).
    void sendMouseButton(int clientX, int clientY, bool up, int clickCount);
    void sendMouseDrag(int clientX, int clientY);
    void setKeyboardFocus(bool focused);
    [[nodiscard]] bool hasKeyboardFocus() const { return m_keyboardFocus; }
    // True when the browser is visible and the mouse cursor is over the page.
    [[nodiscard]] bool isCursorOverView() const;
    // True when the browser is visible and the cursor is anywhere over the
    // window (page or chrome).
    [[nodiscard]] bool isCursorOverWindow() const;
    // True while the browser window is open/visible on screen. Game keyboard
    // and jump input are locked in this state so they don't leak to the game.
    [[nodiscard]] bool isBrowserVisible() const { return m_visible; }

    struct ContextMenuItem {
        int commandId = 0;
        std::string label;
        bool enabled = true;
        bool separator = false;
    };
    void contextMenuCommand(int commandId);
    void contextMenuCancel();

    [[nodiscard]] size_t tabCount() const { return m_tabs.size(); }
    [[nodiscard]] size_t activeIndex() const { return m_active; }
    [[nodiscard]] Tab const* activeTab() const;
    [[nodiscard]] Tab const* tabAt(size_t index) const;
    [[nodiscard]] std::vector<std::string> const& history() const { return m_history; }
    [[nodiscard]] std::vector<DownloadInfo> const& downloads() const { return m_downloads; }
    void updateDownload(DownloadInfo const& info);
    void recordHistory(std::string const& url);
    [[nodiscard]] HWND parentWindow() const { return m_parent; }
    [[nodiscard]] float dpiScale() const;
    // Physical pixels CEF paints per DIP. Higher than dpiScale so the page is
    // rendered at a super-sampled resolution and downscaled when displayed,
    // which keeps text/edges crisp instead of softly upscaled.
    [[nodiscard]] float renderScale() const;
    [[nodiscard]] bool isAvailable() const { return m_cefStarted; }
    [[nodiscard]] std::string const& error() const { return m_error; }

    std::function<void()> onStateChanged;
    // Return true if a middle-click at these client pixels was consumed
    // (e.g. it hit a tab button); set by BrowserWindow.
    std::function<bool(int, int)> onMiddleClick;
    // A short download status line to show as an in-window toast.
    std::function<void(std::string)> onDownloadNotice;
    // Show a context menu at the given client-pixel position; set by
    // BrowserWindow, which renders it in-game.
    std::function<void(int clientX, int clientY, std::vector<ContextMenuItem>)> onContextMenu;

    void setPointerLocked(bool locked);
    void logDebug(std::string const& line);  // TEMP: pause-key diagnostics

private:
    friend class GdCefClient;

    BrowserHost() = default;
    ~BrowserHost() = default;
    BrowserHost(BrowserHost const&) = delete;
    BrowserHost& operator=(BrowserHost const&) = delete;

    HWND findGameWindow() const;
    void loadPersisted();
    void saveDownloads();
    bool startCef();
    void installWndProcHook();
    void removeWndProcHook();
    static LRESULT CALLBACK hookedWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(HWND, UINT, WPARAM, LPARAM, bool& handled);

    void createBrowserForTab(size_t index);
    Tab* tabForClient(GdCefClient* client);
    void removeTabByClient(GdCefClient* client);
    void notify();
    void updateVisibility();
    RECT boundsSnapshot() const;
    bool pointInView(int clientX, int clientY) const;
    void forwardMouse(UINT msg, int clientX, int clientY);
    std::string normalizeAddress(std::string text) const;

    HWND m_parent = nullptr;
    WNDPROC m_prevWndProc = nullptr;
    std::vector<std::unique_ptr<Tab>> m_tabs;
    std::vector<std::string> m_pendingUrls;
    size_t m_active = 0;

    mutable std::mutex m_boundsMutex;
    RECT m_bounds{0, 0, 1, 1};
    RECT m_windowBounds{0, 0, 1, 1};
    HCURSOR m_cursor = nullptr;

    // Last forwarded drag position (client px) so drags can be interpolated
    // into a continuous trajectory rather than sparse jumps.
    int m_dragLastX = 0;
    int m_dragLastY = 0;
    bool m_dragActive = false;

    bool m_visible = false;
    bool m_keyboardFocus = false;
    bool m_cefStarted = false;
    bool m_cefFailed = false;
    std::string m_error;

    // Pointer-lock emulation for OSR (CEF has no native OSR cursor lock).
    bool m_pointerLocked = false;
    POINT m_lockCenter{};   // view center in client pixels
    POINT m_lockVirtual{};  // accumulated virtual cursor position

    CefRefPtr<CefMessageRouterBrowserSide> m_router;
    std::unique_ptr<CefMessageRouterBrowserSide::Handler> m_queryHandler;

    std::vector<std::string> m_history;
    std::vector<DownloadInfo> m_downloads;
    CefRefPtr<CefRunContextMenuCallback> m_contextCallback;

    std::vector<uint8_t> m_composeScratch;
};

#endif
