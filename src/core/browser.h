#pragma once

#include <atomic>
#include <string>

namespace aic {

// The browser tool: one visible Edge (or Chrome) window with its own profile in %LOCALAPPDATA%\AIControls\browser,
// driven over the DevTools Protocol. The window lives on across chats and dies with the client (a job object).
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

// Runs one action; calls are serialised. `cancel` stops a wait early.
BrowserResult RunBrowserAction(const BrowserRequest& request, const std::atomic<bool>& cancel);

}  // namespace aic
