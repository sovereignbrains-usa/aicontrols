#pragma once

#include <windows.h>

#include <objbase.h>
#include <WebView2.h>
#include <wil/com.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "core/browser.h"

namespace aic {

// The browser tool's page inside the client: a WebView2 of its own (profile %LOCALAPPDATA%\AIControls\browser-view)
// in the left column, driven over the DevTools Protocol. The tool's calls come from worker threads; everything that
// touches the WebView runs on the window's thread (a task posted as `message`, run by RunTask).
class BrowserView final : public BrowserPage {
public:
    // `show` makes the column's browser tab appear and places the page (UI thread); `hide` removes the tab.
    BrowserView(HWND window, UINT message, std::function<void()> show, std::function<void()> hide);

    // The window's handler for `message`: runs the posted task.
    static void RunTask(LPARAM param);

    // UI thread: the user's button wants the page (starts it) or has put it away. The tool's own open and close
    // go through Ready and Close.
    void Want(bool on);
    // UI thread: where the page sits (and whether it shows); a page taken away goes blank.
    void Place(const RECT& bounds, bool visible);
    void Blank();
    // UI thread, before the window goes: the WebView's processes end.
    void Shutdown();

    // What the column paints while the page is not up yet.
    struct Status {
        bool ready = false;
        std::string failure;
    };
    Status State();
    // The window moved: tell WebView2 so it does not leave a stale surface.
    void ParentMoved();

    // BrowserPage, worker threads.
    bool Ready(bool& fresh, std::string& error, const std::atomic<bool>& cancel) override;
    bool Call(const std::string& method, const Json& params, Json& result, std::string& error,
              const std::atomic<bool>& cancel, std::chrono::seconds timeout) override;
    bool WaitEvent(const std::string& method, std::chrono::seconds timeout, const std::atomic<bool>& cancel) override;
    void Forget(const std::string& method) override;
    bool Close() override;

private:
    bool Post(std::function<void()> task);
    void Create();  // UI thread
    void Fail(const std::string& why, HRESULT code = S_OK);

    HWND window_;
    UINT message_;
    std::function<void()> show_;
    std::function<void()> hide_;
    std::atomic<bool> shown_{false};
    std::atomic<bool> shutdown_{false};

    // UI thread only.
    wil::com_ptr<ICoreWebView2Controller> controller_;
    wil::com_ptr<ICoreWebView2> view_;
    wil::com_ptr<ICoreWebView2DevToolsProtocolEventReceiver> loadReceiver_;
    bool creating_ = false;

    std::mutex mutex_;
    std::condition_variable changed_;
    bool ready_ = false;           // guarded by mutex_
    bool fresh_ = false;           // just made: nothing set on it yet
    std::string failure_;          // why it could not start
    std::map<std::string, int> events_;
};

}  // namespace aic
