#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>

#include "core/agent_process.h"

namespace aic {

// The browser tool: a page in the client's own column (a WebView2 with its own profile in
// %LOCALAPPDATA%\AIControls\browser, see SetBrowserPage); with none given, one visible Edge (or Chrome) window with
// the same profile, driven over the DevTools Protocol. Edge lives on across chats and dies with the client (a job).
struct BrowserRequest {
    std::string action;    // open, text, screenshot, click, type, close
    std::string url;       // open: http or https
    std::string selector;  // click, type: a CSS selector; click may give `text` instead
    std::string text;      // click: the visible text of the link or button; type: what to type
    std::string key;       // type: "Enter" presses it after the text
};

struct BrowserResult {
    std::string text;           // what the page says: its title, address and visible text, or what happened
    std::string error;          // empty when it worked
    std::string imageBase64;    // screenshot: JPEG in base64
    std::string imageType;      // image/jpeg
};

// A page the client shows itself. Every call comes from a worker thread, one at a time.
class BrowserPage {
public:
    virtual ~BrowserPage() = default;
    // Shows the page and waits until it exists. `fresh`: it was just made, so nothing is set on it yet.
    virtual bool Ready(bool& fresh, std::string& error, const std::atomic<bool>& cancel) = 0;
    // A DevTools Protocol command for the page and its reply.
    virtual bool Call(const std::string& method, const Json& params, Json& result, std::string& error,
                      const std::atomic<bool>& cancel, std::chrono::seconds timeout) = 0;
    // True when an event of this method came within the time (only events after the last Forget count).
    virtual bool WaitEvent(const std::string& method, std::chrono::seconds timeout, const std::atomic<bool>& cancel) = 0;
    virtual void Forget(const std::string& method) = 0;
    // Takes the page away. True when there was one.
    virtual bool Close() = 0;
};

// An address on this computer or in the local network, or one a browser may not go to by itself.
bool LocalBrowserAddress(std::string_view url);

// The client's page for the tool (nullptr: back to Edge in a window of its own). Set once, before any action.
void SetBrowserPage(BrowserPage* page);

// Runs one action; calls are serialised. `cancel` stops a wait early.
BrowserResult RunBrowserAction(const BrowserRequest& request, const std::atomic<bool>& cancel);

}  // namespace aic
