#pragma once

#ifdef GEODE_IS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <include/cef_base.h>
#include <include/cef_browser.h>
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
    [[nodiscard]] bool isAvailable() const { return m_cefStarted; }
    [[nodiscard]] std::string const& error() const { return m_error; }

    std::function<void()> onStateChanged;
    // Return true if a middle-click at these client pixels was consumed
    // (e.g. it hit a tab button); set by BrowserWindow.
    std::function<bool(int, int)> onMiddleClick;

    void setPointerLocked(bool locked);

private:
    friend class GdCefClient;

    BrowserHost() = default;
    ~BrowserHost() = default;
    BrowserHost(BrowserHost const&) = delete;
    BrowserHost& operator=(BrowserHost const&) = delete;

    HWND findGameWindow() const;
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
    HCURSOR m_cursor = nullptr;

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

    std::vector<uint8_t> m_composeScratch;
};

#endif
