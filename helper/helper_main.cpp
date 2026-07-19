// Subprocess executable for Chromium in GD. CEF launches renderer, GPU and
// utility processes through this binary; it never runs game code.
//
// The renderer side hosts a message router so the page can notify the browser
// process (e.g. when pointer lock engages) via window.cefQuery.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <include/cef_app.h>
#include <include/cef_render_process_handler.h>
#include <include/wrapper/cef_message_router.h>

namespace {

class HelperRenderProcessHandler final : public CefRenderProcessHandler {
public:
    void OnWebKitInitialized() override {
        m_router = CefMessageRouterRendererSide::Create(CefMessageRouterConfig());
    }

    void OnContextCreated(
        CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context
    ) override {
        if (m_router) m_router->OnContextCreated(browser, frame, context);
    }

    void OnContextReleased(
        CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context
    ) override {
        if (m_router) m_router->OnContextReleased(browser, frame, context);
    }

    bool OnProcessMessageReceived(
        CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
        CefProcessId source, CefRefPtr<CefProcessMessage> message
    ) override {
        return m_router && m_router->OnProcessMessageReceived(browser, frame, source, message);
    }

private:
    CefRefPtr<CefMessageRouterRendererSide> m_router;
    IMPLEMENT_REFCOUNTING(HelperRenderProcessHandler);
};

class HelperApp final : public CefApp {
public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
        return m_handler;
    }

private:
    CefRefPtr<HelperRenderProcessHandler> m_handler = new HelperRenderProcessHandler();
    IMPLEMENT_REFCOUNTING(HelperApp);
};

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    CefMainArgs args(instance);
    return CefExecuteProcess(args, new HelperApp(), nullptr);
}
