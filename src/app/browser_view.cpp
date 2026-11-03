#include "app/browser_view.h"

#include <commctrl.h>
#include <objidl.h>
#include <uxtheme.h>
#include <wrl/event.h>

#include <algorithm>
namespace Gdiplus {
using std::min;
using std::max;
}
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

#include <wil/resource.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <memory>

#pragma comment(lib, "uxtheme.lib")

#include "core/claude_process.h"
#include "core/journal.h"

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

void BrowserView::Fail(const std::string& why, HRESULT code) {
    {
        std::lock_guard lock(mutex_);
        failure_ = why;
        if (FAILED(code)) {
            failure_ += std::format(" (0x{:08X})", static_cast<unsigned>(code));
        }
        JournalWrite("браузер", ToWide(failure_));  // the browser tab's failures go to the journal too
    }
    creating_ = false;
    changed_.notify_all();
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

int BrowserView::Dip(int dip) const {
    const HWND dpiWindow = host_ != nullptr ? host_ : window_;
    const UINT dpi = dpiWindow != nullptr ? GetDpiForWindow(dpiWindow) : 96;
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

float BrowserView::DipF(float dip) const {
    const HWND dpiWindow = host_ != nullptr ? host_ : window_;
    const UINT dpi = dpiWindow != nullptr ? GetDpiForWindow(dpiWindow) : 96;
    return dip * static_cast<float>(dpi) / 96.f;
}

int BrowserView::BarHeight() const {
    return Dip(48);
}

RECT BrowserView::FieldRect(const RECT& client) const {
    const int bar = BarHeight();
    const int marginX = Dip(12);
    const int fieldH = Dip(32);
    const int top = std::max(0, (bar - fieldH) / 2);
    const int right = std::max(marginX + Dip(64), static_cast<int>(client.right) - marginX);
    return RECT{marginX, top, right, top + fieldH};
}

void BrowserView::EnsureHost() {
    if (host_ != nullptr || window_ == nullptr) {
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW windowClass{};
        // Redraw the whole bar on a resize: the pill's right end stayed behind at every old width (03.11.2026).
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = HostProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"AIControlsBrowserHost";
        registered = RegisterClassW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    // Owned popup, not a child: the client's Direct2D present paints the whole client and covers a child
    // WebView, which is why the tab stayed black with the page already created at the right size.
    host_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"AIControlsBrowserHost", L"", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0, 0,
                            window_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (host_ == nullptr) {
        return;
    }
    SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    if (gdip_ == 0) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&gdip_, &input, nullptr);
    }
    field_ = CreateSolidBrush(RGB(0x23, 0x23, 0x27));
    ground_ = CreateSolidBrush(RGB(0x12, 0x12, 0x13));
    edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 0, 0, host_,
                            reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
    if (edit_ != nullptr) {
        // Themed edits ignore WM_CTLCOLOREDIT and stay white. No theme: the dark field and the cue show.
        SetWindowTheme(edit_, L"", L"");
        ApplyFont();
        SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Dip(2), Dip(6)));
        // FALSE: the cue hides once the field has the caret. Drawn together, the two sat on each other.
        SendMessageW(edit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Поиск или адрес"));
        SetWindowSubclass(edit_, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }
}

void BrowserView::ApplyFont() {
    if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    font_ = CreateFontW(-Dip(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Geist");
    if (edit_ != nullptr && font_ != nullptr) {
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
}

void BrowserView::LayoutPage() {
    if (host_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(host_, &client);
    const int width = client.right;
    const int height = client.bottom;
    const int bar = BarHeight();
    if (edit_ != nullptr) {
        const RECT field = FieldRect(client);
        const int left = static_cast<int>(field.left) + Dip(30);
        const int fieldRight = static_cast<int>(field.right);
        const int fieldTop = static_cast<int>(field.top);
        const int fieldBottom = static_cast<int>(field.bottom);
        const int right = (std::max)(left, fieldRight - Dip(10));
        const int editH = (std::max)(Dip(16), (fieldBottom - fieldTop) - Dip(8));
        const int top = fieldTop + (std::max)(0, (fieldBottom - fieldTop - editH) / 2);
        SetWindowPos(edit_, nullptr, left, top, right - left, editH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    const auto place = [&](ICoreWebView2Controller* controller, bool visible) {
        if (controller == nullptr) {
            return;
        }
        const RECT page{0, bar, width, std::max(bar, height)};
        controller->put_Bounds(page);
        controller->put_IsVisible(visible && placeVisible_ ? TRUE : FALSE);
        controller->NotifyParentWindowPositionChanged();
    };
    place(controller_.get(), page_ == 0);
    for (size_t i = 0; i < extras_.size(); ++i) {
        place(extras_[i].controller.get(), page_ == static_cast<int>(i) + 1);
    }
}

void BrowserView::ApplyPlace() {
    if (!placeVisible_ || window_ == nullptr || IsIconic(window_) || !IsWindowVisible(window_)) {
        if (host_ != nullptr) {
            ShowWindow(host_, SW_HIDE);
        }
        if (controller_) {
            controller_->put_IsVisible(FALSE);
        }
        return;
    }
    EnsureHost();
    if (host_ == nullptr) {
        return;
    }
    POINT origin{bounds_.left, bounds_.top};
    ClientToScreen(window_, &origin);
    const int width = std::max(0L, bounds_.right - bounds_.left);
    const int height = std::max(0L, bounds_.bottom - bounds_.top);
    if (width <= 0 || height <= 0) {
        ShowWindow(host_, SW_HIDE);
        return;
    }
    RECT now{};
    GetWindowRect(host_, &now);
    const bool moved = !IsWindowVisible(host_) || now.left != origin.x || now.top != origin.y ||
                       now.right - now.left != width || now.bottom - now.top != height;
    if (moved) {
        // Above the owner while this app is in front. A move while another app is in front stays put,
        // so the page does not climb over it.
        const HWND foreground = GetForegroundWindow();
        const bool front = foreground == window_ || foreground == host_ || foreground == edit_;
        SetWindowPos(host_, front ? HWND_TOP : HWND_NOTOPMOST, origin.x, origin.y, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | (front ? 0 : SWP_NOZORDER));
        LayoutPage();
        InvalidateRect(host_, nullptr, TRUE);
    }
}

std::wstring BrowserView::AddressText() const {
    if (edit_ == nullptr) {
        return {};
    }
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(edit_)) + 1, L'\0');
    GetWindowTextW(edit_, text.data(), static_cast<int>(text.size()));
    text.resize(wcslen(text.c_str()));
    const size_t first = text.find_first_not_of(L" \t");
    const size_t last = text.find_last_not_of(L" \t");
    if (first == std::wstring::npos) {
        return {};
    }
    return text.substr(first, last - first + 1);
}

std::wstring BrowserView::ResolveAddress(std::wstring text) {
    const bool hasScheme = text.find(L"://") != std::wstring::npos;
    const bool likeUrl = hasScheme || (text.find(L' ') == std::wstring::npos && text.find(L'.') != std::wstring::npos);
    if (!likeUrl) {
        std::wstring query;
        for (const unsigned char byte : ToUtf8(text)) {
            if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
                byte == '-' || byte == '_' || byte == '.' || byte == '~') {
                query.push_back(static_cast<wchar_t>(byte));
            } else {
                query += std::format(L"%{:02X}", byte);
            }
        }
        return L"https://www.startpage.com/sp/search?query=" + query;  // his choice (03.11.2026): Startpage, not Yandex
    }
    if (!hasScheme) {
        text = L"https://" + text;
    }
    return text;
}

void BrowserView::NavigateBar() {
    if (edit_ == nullptr) {
        return;
    }
    std::wstring text = AddressText();
    if (text.empty()) {
        return;
    }
    text = ResolveAddress(std::move(text));
    if (ICoreWebView2* view = ActiveView()) {
        view->Navigate(text.c_str());
        pending_.clear();
        if (ICoreWebView2Controller* controller = ActiveController()) {
            controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
    } else {
        pending_ = std::move(text);  // the page is still starting: it opens this once it exists
    }
}

void BrowserView::FocusAddress() {
    focusBar_ = true;
    Post([this] {
        EnsureHost();
        if (edit_ != nullptr) {
            SetFocus(edit_);
        }
    });
}

LRESULT CALLBACK BrowserView::HostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<BrowserView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_CTLCOLOREDIT && self != nullptr && self->field_ != nullptr) {
        const auto dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(0xf2, 0xf4, 0xf8));
        SetBkColor(dc, RGB(0x23, 0x23, 0x27));  // the pill's own colour (PaintChrome), or the edit shows as a darker slab
        return reinterpret_cast<LRESULT>(self->field_);
    }
    if (message == WM_ERASEBKGND && self != nullptr && self->ground_ != nullptr) {
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, self->ground_);
        return 1;
    }
    if (message == WM_PAINT && self != nullptr) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        if (self->ground_ != nullptr) {
            FillRect(dc, &client, self->ground_);
        }
        self->PaintChrome(dc, client);
        const Status status = self->State();
        if (!status.ready || !status.failure.empty()) {
            client.top = self->BarHeight() + 16;
            client.left += 16;
            client.right -= 16;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, status.failure.empty() ? RGB(0x9c, 0x9e, 0xa4) : RGB(0xf0, 0x60, 0x60));
            if (self->font_ != nullptr) {
                SelectObject(dc, self->font_);
            }
            const std::wstring note = status.failure.empty() ? L"Запускаю браузер…" : ToWide(status.failure);
            DrawTextW(dc, note.c_str(), -1, &client, DT_LEFT | DT_WORDBREAK);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_DPICHANGED && self != nullptr) {
        self->ApplyFont();
        self->LayoutPage();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK BrowserView::EditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR self) {
    if (message == WM_KEYDOWN && wParam == VK_RETURN) {
        reinterpret_cast<BrowserView*>(self)->NavigateBar();
        return 0;
    }
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        SetFocus(reinterpret_cast<BrowserView*>(self)->window_);
        return 0;
    }
    if ((message == WM_SETFOCUS || message == WM_KILLFOCUS) && self != 0) {
        const auto* view = reinterpret_cast<BrowserView*>(self);
        if (view->host_ != nullptr) {
            InvalidateRect(view->host_, nullptr, FALSE);
        }
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void BrowserView::Create() {
    if (controller_ || creating_) {
        return;
    }
    EnsureHost();
    if (host_ == nullptr) {
        Fail("Окно браузера не создалось", E_FAIL);
        return;
    }
    creating_ = true;
    wchar_t local[MAX_PATH]{};
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) {
        if (GetTempPathW(MAX_PATH, local) == 0) {
            Fail("Нет папки для данных браузера", E_FAIL);
            return;
        }
    }
    // AIC_BROWSER_PROFILE lets a gallery check use its own folder while the installed client holds the real one.
    std::filesystem::path folder = std::filesystem::path(local) / L"AIControls" / L"browser-view";
    wchar_t profileOverride[MAX_PATH]{};
    if (const DWORD over = GetEnvironmentVariableW(L"AIC_BROWSER_PROFILE", profileOverride, MAX_PATH);
        over > 0 && over < MAX_PATH) {
        folder = profileOverride;
    }
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        Fail("Не удалось создать папку браузера",
             HRESULT_FROM_WIN32(static_cast<unsigned long>(error.value())));
        return;
    }
    {
        std::lock_guard lock(mutex_);
        failure_.clear();
    }
    const std::wstring data = folder.wstring();
    const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (shutdown_) {
                    return S_OK;
                }
                if (FAILED(result) || environment == nullptr) {
                    Fail("Нужен WebView2 (Microsoft Edge): среда не создалась", FAILED(result) ? result : E_FAIL);
                    return S_OK;
                }
                environment_ = environment;
                for (size_t i = 0; i < extras_.size(); ++i) {  // a tab opened while this was still starting
                    CreateExtra(i);
                }
                const HRESULT creating = environment->CreateCoreWebView2Controller(
                    host_, Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                 [this](HRESULT created, ICoreWebView2Controller* controller) -> HRESULT {
                                     if (shutdown_) {
                                         if (controller != nullptr) {
                                             controller->Close();
                                         }
                                         return S_OK;
                                     }
                                     if (FAILED(created) || controller == nullptr) {
                                         Fail("Окно браузера не создалось", FAILED(created) ? created : E_FAIL);
                                         return S_OK;
                                     }
                                     controller_ = controller;
                                     controller_->get_CoreWebView2(view_.put());
                                     if (!view_) {
                                         controller_.reset();
                                         Fail("Окно браузера не создалось", E_FAIL);
                                         return S_OK;
                                     }
                                     controller_->put_IsVisible(FALSE);
                                     if (const auto colored = controller_.try_query<ICoreWebView2Controller2>()) {
                                         colored->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 0x12, 0x12, 0x13});
                                     }
                                     wil::com_ptr<ICoreWebView2Settings> settings;
                                     if (SUCCEEDED(view_->get_Settings(settings.put()))) {
                                         settings->put_IsStatusBarEnabled(FALSE);
                                     }
                                     Arm(view_.get());
                                     WatchAddress(view_.get());
                                     EventRegistrationToken token{};
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
                                     if (page_ == 0 && !pending_.empty()) {
                                         view_->Navigate(pending_.c_str());
                                         pending_.clear();
                                     } else {
                                         view_->Navigate(L"about:blank");
                                     }
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
                                     // ApplyPlace skips the layout when the host was already sitting at this rect,
                                     // which is the usual case: the tab opened before the page finished starting.
                                     LayoutPage();
                                     if (focusBar_ && edit_ != nullptr) {
                                         const HWND focus = GetFocus();
                                         if (focus == nullptr || focus == window_ || focus == host_ || focus == edit_) {
                                             SetFocus(edit_);
                                         }
                                         focusBar_ = false;
                                     }
                                     if (host_ != nullptr) {
                                         InvalidateRect(host_, nullptr, FALSE);
                                     }
                                     return S_OK;
                                 })
                                 .Get());
                if (FAILED(creating)) {
                    Fail("Окно браузера не создалось", creating);
                }
                return S_OK;
            })
            .Get());
    if (FAILED(started)) {
        Fail("Нужен WebView2 (Microsoft Edge)", started);
    }
}

void BrowserView::Want(bool on) {
    shown_ = on;
    if (on) {
        Post([this] { Create(); });
    }
}

void BrowserView::Place(const RECT& bounds, bool visible) {
    bounds_ = bounds;
    placeVisible_ = visible;
    ApplyPlace();
}

void BrowserView::Blank() {
    shown_ = false;
    placeVisible_ = false;
    if (view_) {
        view_->Navigate(L"about:blank");
    }
    // The tool's page goes blank. A tab the person opened keeps its address.
    if (page_ == 0 && edit_ != nullptr) {
        SetWindowTextW(edit_, L"");
    }
    if (controller_) {
        controller_->put_IsVisible(FALSE);
    }
    for (ExtraPage& page : extras_) {
        if (page.controller) {
            page.controller->put_IsVisible(FALSE);
        }
    }
    if (host_ != nullptr) {
        ShowWindow(host_, SW_HIDE);
    }
}

void BrowserView::Shutdown() {
    shutdown_ = true;
    for (ExtraPage& page : extras_) {
        if (page.controller) {
            page.controller->Close();
        }
        page.controller.reset();
        page.view.reset();
    }
    if (controller_) {
        controller_->Close();  // the browser processes go with it; the host goes after
    }
    loadReceiver_.reset();
    view_.reset();
    controller_.reset();
    if (edit_ != nullptr) {
        RemoveWindowSubclass(edit_, EditProc, 1);
        edit_ = nullptr;
    }
    if (host_ != nullptr) {
        DestroyWindow(host_);
        host_ = nullptr;
    }
    if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    if (field_ != nullptr) {
        DeleteObject(field_);
        field_ = nullptr;
    }
    if (ground_ != nullptr) {
        DeleteObject(ground_);
        ground_ = nullptr;
    }
    if (gdip_ != 0) {
        Gdiplus::GdiplusShutdown(gdip_);
        gdip_ = 0;
    }
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

BrowserView::Status BrowserView::State() {
    if (page_ > 0) {
        const size_t index = static_cast<size_t>(page_ - 1);
        if (index < extras_.size() && extras_[index].view) {
            return {true, {}};
        }
        std::lock_guard lock(mutex_);
        if (!environment_ && !failure_.empty()) {  // the tool's page never started: this one cannot either
            return {false, failure_};
        }
        return {false, {}};
    }
    std::lock_guard lock(mutex_);
    return {ready_, failure_};
}

void BrowserView::ParentMoved() {
    ApplyPlace();
}

void BrowserView::PaintChrome(HDC dc, const RECT& client) {
    if (gdip_ == 0 || client.right <= 0) {
        return;
    }
    const RECT field = FieldRect(client);
    if (field.right - field.left < Dip(16) || field.bottom <= field.top) {
        return;
    }
    Gdiplus::Graphics canvas(dc);
    canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    canvas.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float radius = DipF(8.f);
    const float left = static_cast<float>(field.left);
    const float top = static_cast<float>(field.top);
    const float right = static_cast<float>(field.right);
    const float bottom = static_cast<float>(field.bottom);
    const float diameter = radius * 2.f;
    Gdiplus::GraphicsPath path;
    path.AddArc(left, top, diameter, diameter, 180.f, 90.f);
    path.AddArc(right - diameter, top, diameter, diameter, 270.f, 90.f);
    path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0.f, 90.f);
    path.AddArc(left, bottom - diameter, diameter, diameter, 90.f, 90.f);
    path.CloseFigure();
    const Gdiplus::SolidBrush fill(Gdiplus::Color(255, 0x23, 0x23, 0x27));
    canvas.FillPath(&fill, &path);
    if (edit_ != nullptr && GetFocus() == edit_) {
        const Gdiplus::Pen ring(Gdiplus::Color(190, 0x4c, 0x92, 0xff), std::max(1.f, DipF(1.f)));
        canvas.DrawPath(&ring, &path);
    }
    // The same magnifier as the sidebar's search, sitting in the field's left end.
    const float cx = left + DipF(16.f);
    const float cy = (top + bottom) / 2.f;
    const Gdiplus::Pen icon(Gdiplus::Color(255, 0x9c, 0x9e, 0xa4), std::max(1.f, DipF(1.3f)));
    const float glass = DipF(4.5f);
    canvas.DrawEllipse(&icon, cx - DipF(1.f) - glass, cy - DipF(1.f) - glass, glass * 2.f, glass * 2.f);
    canvas.DrawLine(&icon, cx + DipF(2.5f), cy + DipF(2.5f), cx + DipF(5.5f), cy + DipF(5.5f));
}

ICoreWebView2* BrowserView::ActiveView() const {
    if (page_ <= 0) {
        return view_.get();
    }
    const size_t index = static_cast<size_t>(page_ - 1);
    return index < extras_.size() ? extras_[index].view.get() : nullptr;
}

ICoreWebView2Controller* BrowserView::ActiveController() const {
    if (page_ <= 0) {
        return controller_.get();
    }
    const size_t index = static_cast<size_t>(page_ - 1);
    return index < extras_.size() ? extras_[index].controller.get() : nullptr;
}

void BrowserView::WatchAddress(ICoreWebView2* view) {
    if (view == nullptr) {
        return;
    }
    EventRegistrationToken token{};
    view->add_SourceChanged(
        Microsoft::WRL::Callback<ICoreWebView2SourceChangedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs*) {
                if (edit_ == nullptr || GetFocus() == edit_ || sender == nullptr || sender != ActiveView()) {
                    return S_OK;
                }
                wil::unique_cotaskmem_string source;
                if (FAILED(sender->get_Source(&source)) || source == nullptr ||
                    wcscmp(source.get(), L"about:blank") == 0) {
                    return S_OK;
                }
                SetWindowTextW(edit_, source.get());
                return S_OK;
            })
            .Get(),
        &token);
}

void BrowserView::Arm(ICoreWebView2* view) {
    if (view == nullptr) {
        return;
    }
    EventRegistrationToken token{};
    // Nothing the page does on its own may land on this computer or the home network.
    view->add_NavigationStarting(
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
    view->add_NewWindowRequested(
        Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [view](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
                args->put_Handled(TRUE);
                wil::unique_cotaskmem_string uri;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri && MayNavigate(ToUtf8(uri.get()))) {
                    view->Navigate(uri.get());
                }
                return S_OK;
            })
            .Get(),
        &token);
    wil::com_ptr<ICoreWebView2_4> four;
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(four.put())))) {
        four->add_DownloadStarting(  // the agent downloads nothing, and neither does a tab beside it
            Microsoft::WRL::Callback<ICoreWebView2DownloadStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) {
                    args->put_Cancel(TRUE);
                    return S_OK;
                })
                .Get(),
            &token);
    }
}

void BrowserView::CreateExtra(size_t index) {
    if (!environment_ || host_ == nullptr || index >= extras_.size()) {
        return;
    }
    ExtraPage& page = extras_[index];
    if (page.closed || page.controller || page.opening) {
        return;
    }
    page.opening = true;
    const int slot = static_cast<int>(index) + 1;
    const HRESULT creating = environment_->CreateCoreWebView2Controller(
        host_, Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                   [this, index, slot](HRESULT created, ICoreWebView2Controller* controller) -> HRESULT {
                       if (index >= extras_.size()) {
                           if (controller != nullptr) {
                               controller->Close();
                           }
                           return S_OK;
                       }
                       ExtraPage& arrived = extras_[index];
                       arrived.opening = false;
                       if (shutdown_ || arrived.closed || FAILED(created) || controller == nullptr) {
                           if (controller != nullptr) {
                               controller->Close();
                           }
                           if (host_ != nullptr) {
                               InvalidateRect(host_, nullptr, FALSE);
                           }
                           return S_OK;
                       }
                       arrived.controller = controller;
                       arrived.controller->get_CoreWebView2(arrived.view.put());
                       if (!arrived.view) {
                           arrived.controller->Close();
                           arrived.controller.reset();
                           return S_OK;
                       }
                       arrived.controller->put_IsVisible(FALSE);
                       if (const auto colored = arrived.controller.try_query<ICoreWebView2Controller2>()) {
                           colored->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 0x12, 0x12, 0x13});
                       }
                       wil::com_ptr<ICoreWebView2Settings> settings;
                       if (SUCCEEDED(arrived.view->get_Settings(settings.put()))) {
                           settings->put_IsStatusBarEnabled(FALSE);
                       }
                       Arm(arrived.view.get());
                       WatchAddress(arrived.view.get());
                       if (page_ == slot && !pending_.empty()) {
                           arrived.view->Navigate(pending_.c_str());
                           pending_.clear();
                       } else {
                           arrived.view->Navigate(L"about:blank");
                       }
                       LayoutPage();
                       if (host_ != nullptr) {
                           InvalidateRect(host_, nullptr, FALSE);
                       }
                       return S_OK;
                   })
                   .Get());
    if (FAILED(creating)) {
        page.opening = false;
    }
}

int BrowserView::AddPage() {
    EnsureHost();
    extras_.push_back({});
    const int slot = static_cast<int>(extras_.size());
    if (environment_) {
        CreateExtra(static_cast<size_t>(slot - 1));
    }
    ShowPage(slot);
    return slot;
}

void BrowserView::ShowPage(int slot) {
    if (slot < 0) {
        slot = 0;
    }
    const bool changed = slot != page_;
    page_ = slot;
    LayoutPage();
    if (!changed || edit_ == nullptr) {
        return;
    }
    ICoreWebView2* view = ActiveView();
    if (view == nullptr) {
        SetWindowTextW(edit_, L"");
        return;
    }
    wil::unique_cotaskmem_string source;
    if (FAILED(view->get_Source(&source)) || source == nullptr || wcscmp(source.get(), L"about:blank") == 0) {
        SetWindowTextW(edit_, L"");
        return;
    }
    SetWindowTextW(edit_, source.get());
}

void BrowserView::ClosePage(int slot) {
    if (slot <= 0) {
        return;
    }
    const size_t index = static_cast<size_t>(slot - 1);
    if (index >= extras_.size()) {
        return;
    }
    ExtraPage& page = extras_[index];
    page.closed = true;
    if (page.controller) {
        page.controller->Close();
    }
    page.controller.reset();
    page.view.reset();
    // Leave page_ on this slot. The next ShowPage sees the change and rewrites the address;
    // zeroing it here would keep the closed page's URL on the tool's tab.
}

}  // namespace aic
