#include "app/browser_view.h"

#include <wrl/event.h>

#include <wil/resource.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <memory>

#include "core/claude_process.h"

namespace aic {
namespace {

using namespace std::chrono_literals;

// Where the page may go by itself (a link, a redirect, a script, the tool): http(s) outside this computer's
// network, or a blank page. The tool's own `open` is checked before, and the page's requests by the DevTools block.
bool MayNavigate(const std::string& url) {
    if (url == "about:blank" || url.starts_with("chrome-error:")) {
        return true;
    }
    return (url.starts_with("http://") || url.starts_with("https://")) && !LocalBrowserAddress(url);
}

}  // namespace

BrowserView::BrowserView(HWND window, UINT message, std::function<void()> show, std::function<void()> hide)
    : window_(window), message_(message), show_(std::move(show)), hide_(std::move(hide)) {}

bool BrowserView::Post(std::function<void()> task) {
    if (shutdown_) {
        return false;
    }
    auto* heap = new std::function<void()>(std::move(task));
    if (!PostMessageW(window_, message_, 0, reinterpret_cast<LPARAM>(heap))) {
        delete heap;
        return false;
    }
    return true;
}

void BrowserView::RunTask(LPARAM param) {
    std::unique_ptr<std::function<void()>> task(reinterpret_cast<std::function<void()>*>(param));
    if (task && *task) {
        (*task)();
    }
}

void BrowserView::Fail(const std::string& why) {
    {
        std::lock_guard lock(mutex_);
        failure_ = why;
    }
    creating_ = false;
    changed_.notify_all();
}

void BrowserView::Create() {
    if (controller_ || creating_) {
        return;
    }
    creating_ = true;
    wchar_t local[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const std::wstring data = (std::filesystem::path(local) / L"AIControls" / L"browser-view").wstring();
    const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || environment == nullptr) {
                    Fail("Нужен WebView2 (Microsoft Edge): среда не создалась");
                    return S_OK;
                }
                const HRESULT creating = environment->CreateCoreWebView2Controller(
                    window_, Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                 [this](HRESULT created, ICoreWebView2Controller* controller) -> HRESULT {
                                     if (FAILED(created) || controller == nullptr) {
                                         Fail("Окно браузера не создалось");
                                         return S_OK;
                                     }
                                     controller_ = controller;
                                     controller_->get_CoreWebView2(view_.put());
                                     if (!view_) {
                                         controller_.reset();
                                         Fail("Окно браузера не создалось");
                                         return S_OK;
                                     }
                                     controller_->put_IsVisible(FALSE);
                                     if (const auto colored = controller_.try_query<ICoreWebView2Controller2>()) {
                                         colored->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 255, 255, 255});
                                     }
                                     wil::com_ptr<ICoreWebView2Settings> settings;
                                     if (SUCCEEDED(view_->get_Settings(settings.put()))) {
                                         settings->put_IsStatusBarEnabled(FALSE);
                                     }
                                     EventRegistrationToken token{};
                                     // Third layer of the block on local addresses: nothing the page does lands there.
                                     view_->add_NavigationStarting(
                                         Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                                             [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
                                                 wil::unique_cotaskmem_string uri;
                                                 if (FAILED(args->get_Uri(&uri)) || !uri || !MayNavigate(ToUtf8(uri.get()))) {
                                                     args->put_Cancel(TRUE);
                                                 }
                                                 return S_OK;
                                             })
                                             .Get(),
                                         &token);
                                     // A link that wants a window of its own opens in this page.
                                     view_->add_NewWindowRequested(
                                         Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                             [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
                                                 args->put_Handled(TRUE);
                                                 wil::unique_cotaskmem_string uri;
                                                 if (SUCCEEDED(args->get_Uri(&uri)) && uri && MayNavigate(ToUtf8(uri.get()))) {
                                                     view_->Navigate(uri.get());
                                                 }
                                                 return S_OK;
                                             })
                                             .Get(),
                                         &token);
                                     if (const auto four = view_.try_query<ICoreWebView2_4>()) {
                                         four->add_DownloadStarting(  // the agent downloads nothing
                                             Microsoft::WRL::Callback<ICoreWebView2DownloadStartingEventHandler>(
                                                 [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) {
                                                     args->put_Cancel(TRUE);
                                                     return S_OK;
                                                 })
                                                 .Get(),
                                             &token);
                                     }
                                     if (SUCCEEDED(view_->GetDevToolsProtocolEventReceiver(L"Page.loadEventFired",
                                                                                           loadReceiver_.put()))) {
                                         loadReceiver_->add_DevToolsProtocolEventReceived(
                                             Microsoft::WRL::Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
                                                 [this](ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs*) {
                                                     {
                                                         std::lock_guard lock(mutex_);
                                                         ++events_["Page.loadEventFired"];
                                                     }
                                                     changed_.notify_all();
                                                     return S_OK;
                                                 })
                                                 .Get(),
                                             &token);
                                     }
                                     view_->Navigate(L"about:blank");
                                     creating_ = false;
                                     {
                                         std::lock_guard lock(mutex_);
                                         ready_ = true;
                                         fresh_ = true;
                                         failure_.clear();
                                     }
                                     changed_.notify_all();
                                     if (shown_ && show_) {
                                         show_();  // the page gets its place
                                     }
                                     return S_OK;
                                 })
                                 .Get());
                if (FAILED(creating)) {
                    Fail("Окно браузера не создалось");
                }
                return S_OK;
            })
            .Get());
    if (FAILED(started)) {
        Fail("Нужен WebView2 (Microsoft Edge)");
    }
}

void BrowserView::Place(const RECT& bounds, bool visible) {
    if (!controller_) {
        return;
    }
    if (visible) {
        controller_->put_Bounds(bounds);
    }
    controller_->put_IsVisible(visible ? TRUE : FALSE);
}

void BrowserView::Blank() {
    shown_ = false;
    if (view_) {
        view_->Navigate(L"about:blank");
    }
    if (controller_) {
        controller_->put_IsVisible(FALSE);
    }
}

void BrowserView::Shutdown() {
    shutdown_ = true;
    if (controller_) {
        controller_->Close();  // the browser processes go with it
    }
    loadReceiver_.reset();
    view_.reset();
    controller_.reset();
}

bool BrowserView::Ready(bool& fresh, std::string& error, const std::atomic<bool>& cancel) {
    {
        std::lock_guard lock(mutex_);
        if (!ready_) {
            failure_.clear();
        }
    }
    shown_ = true;
    if (!Post([this] {
            Create();
            if (show_) {
                show_();
            }
        })) {
        error = "Окно клиента закрыто";
        return false;
    }
    std::unique_lock lock(mutex_);
    const auto until = std::chrono::steady_clock::now() + 30s;
    while (!ready_ && failure_.empty()) {
        if (cancel) {
            error = "Остановлено";
            return false;
        }
        if (std::chrono::steady_clock::now() >= until) {
            error = "Браузер не запустился за 30 секунд";
            return false;
        }
        changed_.wait_for(lock, 100ms);
    }
    if (!ready_) {
        error = failure_;
        return false;
    }
    fresh = fresh_;
    fresh_ = false;
    return true;
}

bool BrowserView::Call(const std::string& method, const Json& params, Json& result, std::string& error,
                       const std::atomic<bool>& cancel, std::chrono::seconds timeout) {
    struct State {
        std::mutex mutex;
        std::condition_variable done;
        bool finished = false;
        HRESULT code = S_OK;
        std::string json;
    };
    auto state = std::make_shared<State>();
    const auto finish = [state](HRESULT code, std::string json) {
        {
            std::lock_guard lock(state->mutex);
            state->code = code;
            state->json = std::move(json);
            state->finished = true;
        }
        state->done.notify_all();
    };
    const std::wstring name = ToWide(method);
    const std::wstring body = ToWide(params.dump(-1, ' ', false, Json::error_handler_t::replace));
    if (!Post([this, finish, name, body] {
            if (!view_) {
                finish(E_FAIL, {});
                return;
            }
            const HRESULT sent = view_->CallDevToolsProtocolMethod(
                name.c_str(), body.c_str(),
                Microsoft::WRL::Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
                    [finish](HRESULT code, LPCWSTR json) {
                        finish(code, json != nullptr ? ToUtf8(json) : std::string());
                        return S_OK;
                    })
                    .Get());
            if (FAILED(sent)) {
                finish(sent, {});
            }
        })) {
        error = "Окно клиента закрыто";
        return false;
    }
    std::unique_lock lock(state->mutex);
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (!state->finished) {
        if (cancel) {
            error = "Остановлено";
            return false;
        }
        if (std::chrono::steady_clock::now() >= until) {
            error = method + ": браузер не ответил";
            return false;
        }
        state->done.wait_for(lock, 100ms);
    }
    if (FAILED(state->code)) {
        error = method + ": ошибка DevTools (" + std::format("0x{:08X}", static_cast<unsigned>(state->code)) + ")";
        return false;
    }
    result = state->json.empty() ? Json::object() : Json::parse(state->json, nullptr, false);
    if (result.is_discarded()) {
        result = Json::object();
    }
    return true;
}

bool BrowserView::WaitEvent(const std::string& method, std::chrono::seconds timeout, const std::atomic<bool>& cancel) {
    std::unique_lock lock(mutex_);
    const auto until = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (int& count = events_[method]; count > 0) {
            --count;
            return true;
        }
        if (cancel || std::chrono::steady_clock::now() >= until) {
            return false;
        }
        changed_.wait_for(lock, 100ms);
    }
}

void BrowserView::Forget(const std::string& method) {
    std::lock_guard lock(mutex_);
    events_[method] = 0;
}

bool BrowserView::Close() {
    const bool was = shown_.exchange(false);
    Post([this] {
        Blank();
        if (hide_) {
            hide_();
        }
    });
    return was;
}

}  // namespace aic
