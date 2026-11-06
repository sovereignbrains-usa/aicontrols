#include "app/browser_view.h"

#include <commctrl.h>
#include <objidl.h>
#include <shlwapi.h>
#include <windowsx.h>
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
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shlwapi.lib")

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

// A site's icon (PNG, ICO, whatever GDI+ reads) as a 32-pixel square of its own; nothing if it does not decode.
std::shared_ptr<Gdiplus::Bitmap> DecodeIcon(const std::string& bytes) {
    if (bytes.empty() || bytes.size() > (1u << 20)) {
        return {};
    }
    wil::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size())));
    if (!stream) {
        return {};
    }
    std::unique_ptr<Gdiplus::Bitmap> source(Gdiplus::Bitmap::FromStream(stream.get()));
    if (!source || source->GetLastStatus() != Gdiplus::Ok || source->GetWidth() == 0 || source->GetHeight() == 0) {
        return {};
    }
    auto icon = std::make_shared<Gdiplus::Bitmap>(32, 32, PixelFormat32bppPARGB);
    if (icon->GetLastStatus() != Gdiplus::Ok) {
        return {};
    }
    Gdiplus::Graphics canvas(icon.get());
    canvas.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    canvas.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    canvas.Clear(Gdiplus::Color(0, 0, 0, 0));
    canvas.DrawImage(source.get(), 0, 0, 32, 32);
    return icon;
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
    return Dip(48) + (marks_.empty() ? 0 : Dip(28));  // the bookmarks row, while there is one
}

RECT BrowserView::FieldRect(const RECT& client) const {
    const int bar = Dip(48);  // the address row only: the bookmarks row hangs under it
    const int marginX = Dip(12) + Dip(32);  // the back arrow stands left of the pill
    const int fieldH = Dip(32);
    const int top = std::max(0, (bar - fieldH) / 2);
    const int right = std::max(marginX + Dip(64), static_cast<int>(client.right) - Dip(12));
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
    LoadMarks();
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
    for (HFONT* made : {&chipFont_, &monoFont_}) {
        if (*made != nullptr) {
            DeleteObject(*made);
        }
    }
    chipFont_ = CreateFontW(-Dip(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Geist");
    monoFont_ = CreateFontW(-Dip(9), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Geist");
}

void BrowserView::LayoutPage() {
    if (host_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(host_, &client);
    const int width = client.right;
    const int height = client.bottom;
    LayoutBar();
    const int bar = BarHeight();
    if (edit_ != nullptr) {
        const RECT field = FieldRect(client);
        const int left = static_cast<int>(field.left) + Dip(30);
        const int fieldRight = static_cast<int>(field.right);
        const int fieldTop = static_cast<int>(field.top);
        const int fieldBottom = static_cast<int>(field.bottom);
        const int right = (std::max)(left, fieldRight - Dip(34));  // the star sits in the field's right end
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
    Go(ResolveAddress(std::move(text)));
}

void BrowserView::Go(const std::wstring& url) {
    if (edit_ != nullptr && GetFocus() != edit_) {
        SetWindowTextW(edit_, url.c_str());
    }
    if (ICoreWebView2* view = ActiveView()) {
        view->Navigate(url.c_str());
        pending_.clear();
        if (ICoreWebView2Controller* controller = ActiveController()) {
            controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
    } else {
        if (edit_ != nullptr) {
            SetWindowTextW(edit_, url.c_str());
        }
        pending_ = url;  // the page is still starting: it opens this once it exists
    }
    InvalidateBar();
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
            const RECT below{0, self->BarHeight(), client.right, client.bottom};
            FillRect(dc, &below, self->ground_);
        }
        self->PaintBar(dc, client);
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
    if (self != nullptr) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        switch (message) {
        case WM_MOUSEMOVE:
            self->SetHot(self->HitBar(point));
            return 0;
        case WM_MOUSELEAVE:
            self->tracking_ = false;
            self->SetHot(kHitNone);
            return 0;
        case WM_LBUTTONUP:
            self->OnBarClick(point, false);
            return 0;
        case WM_MBUTTONUP:
            self->OnBarClick(point, true);
            return 0;
        case WM_RBUTTONUP:
            self->OnBarMenu(point);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT cursor{};
                GetCursorPos(&cursor);
                ScreenToClient(hwnd, &cursor);
                if (self->HitBar(cursor) != kHitNone) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        default:
            break;
        }
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
    for (HFONT* made : {&font_, &chipFont_, &monoFont_}) {
        if (*made != nullptr) {
            DeleteObject(*made);
            *made = nullptr;
        }
    }
    icons_.clear();  // GDI+ bitmaps, before GDI+ goes
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
    {
        // The back arrow: lit while the page has somewhere to go back to.
        BOOL can = FALSE;
        if (ICoreWebView2* view = ActiveView()) {
            view->get_CanGoBack(&can);
        }
        if (can && hot_ == kHitBack) {
            const Gdiplus::SolidBrush hover(Gdiplus::Color(255, 0x2e, 0x2e, 0x33));
            canvas.FillEllipse(&hover, static_cast<float>(back_.left), static_cast<float>(back_.top),
                               static_cast<float>(back_.right - back_.left),
                               static_cast<float>(back_.bottom - back_.top));
        }
        const BYTE shade = !can ? 0x55 : (hot_ == kHitBack ? 0xf2 : 0xb4);
        Gdiplus::Pen arrow(Gdiplus::Color(255, shade, shade, shade), std::max(1.4f, DipF(1.6f)));
        arrow.SetLineJoin(Gdiplus::LineJoinRound);
        arrow.SetStartCap(Gdiplus::LineCapRound);
        arrow.SetEndCap(Gdiplus::LineCapRound);
        const float ax = (back_.left + back_.right) / 2.f;
        const float ay = (back_.top + back_.bottom) / 2.f;
        const float half = DipF(6.f);
        canvas.DrawLine(&arrow, ax + half, ay, ax - half, ay);
        canvas.DrawLine(&arrow, ax - half, ay, ax - half * 0.15f, ay - half * 0.85f);
        canvas.DrawLine(&arrow, ax - half, ay, ax - half * 0.15f, ay + half * 0.85f);
    }
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

    // The star: this page in the bookmarks (lit) or not; only while a page is open.
    if (BookmarkHost(ActiveSource()).empty() || star_.right <= star_.left) {
        return;
    }
    if (hot_ == kHitStar) {
        const Gdiplus::SolidBrush hover(Gdiplus::Color(255, 0x2e, 0x2e, 0x33));
        Gdiplus::GraphicsPath back;
        const float sl = static_cast<float>(star_.left), st = static_cast<float>(star_.top);
        const float sr = static_cast<float>(star_.right), sb = static_cast<float>(star_.bottom);
        const float d = DipF(12.f);
        back.AddArc(sl, st, d, d, 180.f, 90.f);
        back.AddArc(sr - d, st, d, d, 270.f, 90.f);
        back.AddArc(sr - d, sb - d, d, d, 0.f, 90.f);
        back.AddArc(sl, sb - d, d, d, 90.f, 90.f);
        back.CloseFigure();
        canvas.FillPath(&hover, &back);
    }
    const float sx = (star_.left + star_.right) / 2.f;
    const float sy = (star_.top + star_.bottom) / 2.f + DipF(0.5f);
    const float outer = DipF(6.5f);
    const float inner = outer * 0.45f;
    Gdiplus::PointF points[10];
    for (int i = 0; i < 10; ++i) {
        const float angle = -1.5707963f + static_cast<float>(i) * 0.6283185f;
        const float r = i % 2 == 0 ? outer : inner;
        points[i] = Gdiplus::PointF(sx + r * std::cos(angle), sy + r * std::sin(angle));
    }
    if (Marked(ActiveSource())) {
        const Gdiplus::SolidBrush lit(Gdiplus::Color(255, 0x4c, 0x92, 0xff));
        canvas.FillPolygon(&lit, points, 10);
    } else {
        const BYTE shade = hot_ == kHitStar ? 0xf2 : 0x9c;
        Gdiplus::Pen outline(Gdiplus::Color(255, shade, shade, static_cast<BYTE>(shade + (shade == 0xf2 ? 6 : 8))),
                             std::max(1.f, DipF(1.2f)));
        outline.SetLineJoin(Gdiplus::LineJoinRound);
        canvas.DrawPolygon(&outline, points, 10);
    }
}

void BrowserView::SetHooks(std::function<int(HMENU, POINT)> menu, std::function<void()> newTab) {
    menu_ = std::move(menu);
    newTab_ = std::move(newTab);
}

void BrowserView::LoadMarks() {
    if (marksLoaded_) {
        return;
    }
    marksLoaded_ = true;
    marks_ = LoadBookmarks();
}

void BrowserView::SaveMarks() {
    if (!SaveBookmarks(marks_)) {
        JournalWrite("браузер", L"Закладки не сохранились в " + BookmarksFolder() + L"\\bookmarks.json");
    }
    LayoutPage();  // the row may have come or gone: the page moves with it
    if (host_ != nullptr) {
        InvalidateRect(host_, nullptr, FALSE);
    }
}

void BrowserView::LayoutBar() {
    chips_.clear();
    hidden_.clear();
    more_ = {};
    star_ = {};
    back_ = {};
    if (host_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(host_, &client);
    const RECT field = FieldRect(client);
    back_ = RECT{Dip(8), field.top, Dip(8) + Dip(32), field.bottom};
    star_ = RECT{field.right - Dip(30), field.top + Dip(4), field.right - Dip(6), field.bottom - Dip(4)};
    if (marks_.empty()) {
        return;
    }
    const HDC dc = GetDC(host_);
    if (dc == nullptr) {
        return;
    }
    const HGDIOBJ old = SelectObject(dc, chipFont_ != nullptr ? static_cast<HGDIOBJ>(chipFont_) : GetStockObject(DEFAULT_GUI_FONT));
    std::vector<int> widths;
    int total = 0;
    const int gap = Dip(2);
    for (const Bookmark& mark : marks_) {
        SIZE size{};
        GetTextExtentPoint32W(dc, mark.name.c_str(), static_cast<int>(mark.name.size()), &size);
        // Padding 5, the icon 14, 6 to the name, the name (at most 160), padding 8.
        const int width = Dip(5) + Dip(14) + Dip(6) + std::min(static_cast<int>(size.cx), Dip(160)) + Dip(8);
        widths.push_back(width);
        total += width + gap;
    }
    SelectObject(dc, old);
    ReleaseDC(host_, dc);
    // The first icon stands under the magnifier.
    int x = static_cast<int>(field.left) + Dip(4);
    const int top = Dip(46);
    const int bottom = top + Dip(24);
    const int limit = static_cast<int>(field.right);
    const bool all = x + total - gap <= limit;
    const int stop = all ? limit : limit - Dip(28) - gap;
    for (size_t i = 0; i < marks_.size(); ++i) {
        if (hidden_.empty() && x + widths[i] <= stop) {
            chips_.push_back({RECT{x, top, x + widths[i], bottom}, i});
            x += widths[i] + gap;
        } else {
            hidden_.push_back(i);
        }
    }
    if (!hidden_.empty()) {
        more_ = RECT{limit - Dip(28), top, limit, bottom};
    }
}

int BrowserView::HitBar(POINT point) const {
    if (PtInRect(&back_, point)) {
        return kHitBack;
    }
    if (PtInRect(&star_, point) && !BookmarkHost(ActiveSource()).empty()) {
        return kHitStar;
    }
    if (PtInRect(&more_, point)) {
        return kHitMore;
    }
    for (const Chip& chip : chips_) {
        if (PtInRect(&chip.rect, point)) {
            return static_cast<int>(chip.index);
        }
    }
    return kHitNone;
}

void BrowserView::SetHot(int hot) {
    if (hot != kHitNone && !tracking_ && host_ != nullptr) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, host_, 0};
        tracking_ = TrackMouseEvent(&track) != FALSE;
    }
    if (hot == hot_) {
        return;
    }
    hot_ = hot;
    InvalidateBar();
}

void BrowserView::InvalidateBar() {
    if (host_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(host_, &client);
    const RECT bar{0, 0, client.right, BarHeight()};
    InvalidateRect(host_, &bar, FALSE);
}

std::wstring BrowserView::ActiveSource() const {
    ICoreWebView2* view = ActiveView();
    if (view == nullptr) {
        return {};
    }
    wil::unique_cotaskmem_string source;
    if (FAILED(view->get_Source(&source)) || source == nullptr || wcscmp(source.get(), L"about:blank") == 0) {
        return {};
    }
    return source.get();
}

bool BrowserView::Marked(const std::wstring& source) const {
    return !source.empty() &&
           std::ranges::any_of(marks_, [&](const Bookmark& mark) { return SameAddress(mark.url, source); });
}

void BrowserView::OpenMark(size_t index, bool newTab) {
    if (index >= marks_.size()) {
        return;
    }
    const std::wstring url = marks_[index].url;
    if (newTab && newTab_) {
        newTab_();  // the new tab is on show now; its page comes up with this address
    }
    Go(url);
}

void BrowserView::ToggleStar() {
    const std::wstring source = ActiveSource();
    if (BookmarkHost(source).empty()) {
        return;
    }
    const auto found = std::ranges::find_if(marks_, [&](const Bookmark& mark) { return SameAddress(mark.url, source); });
    if (found != marks_.end()) {
        marks_.erase(found);
    } else {
        std::wstring title;
        if (ICoreWebView2* view = ActiveView()) {
            wil::unique_cotaskmem_string text;
            if (SUCCEEDED(view->get_DocumentTitle(&text)) && text != nullptr) {
                title = text.get();
            }
        }
        marks_.push_back({BookmarkName(title, source), source});
    }
    SaveMarks();
    FetchIcon(ActiveView());
}

void BrowserView::OnBarClick(POINT point, bool middle) {
    const int hit = HitBar(point);
    if (hit == kHitBack) {
        BOOL can = FALSE;
        if (ICoreWebView2* view = ActiveView(); view != nullptr && SUCCEEDED(view->get_CanGoBack(&can)) && can) {
            view->GoBack();
        }
    } else if (hit == kHitStar && !middle) {
        ToggleStar();
    } else if (hit == kHitMore) {
        if (!menu_ || host_ == nullptr) {
            return;
        }
        wil::unique_hmenu menu(CreatePopupMenu());
        for (const size_t index : hidden_) {
            AppendMenuW(menu.get(), MF_STRING, index + 1, marks_[index].name.c_str());
        }
        POINT at{more_.right, more_.bottom + Dip(2)};
        ClientToScreen(host_, &at);
        const int chosen = menu_(menu.get(), at);
        if (chosen > 0) {
            OpenMark(static_cast<size_t>(chosen - 1), middle || (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        }
    } else if (hit >= 0) {
        OpenMark(static_cast<size_t>(hit), middle || (GetKeyState(VK_CONTROL) & 0x8000) != 0);
    }
}

void BrowserView::OnBarMenu(POINT point) {
    if (!menu_ || host_ == nullptr || point.y >= BarHeight()) {
        return;
    }
    const int hit = HitBar(point);
    constexpr UINT kOpen = 1, kNewTab = 2, kLeft = 3, kRight = 4, kRemove = 5, kRestore = 6;
    wil::unique_hmenu menu(CreatePopupMenu());
    const bool onChip = hit >= 0 && static_cast<size_t>(hit) < marks_.size();
    if (onChip) {
        const size_t index = static_cast<size_t>(hit);
        AppendMenuW(menu.get(), MF_STRING, kOpen, L"Открыть");
        if (newTab_) {
            AppendMenuW(menu.get(), MF_STRING, kNewTab, L"Открыть в новой вкладке\tСр. кнопка");
        }
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu.get(), MF_STRING | (index == 0 ? MF_GRAYED : 0), kLeft, L"Левее");
        AppendMenuW(menu.get(), MF_STRING | (index + 1 >= marks_.size() ? MF_GRAYED : 0), kRight, L"Правее");
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu.get(), MF_STRING, kRemove, L"Удалить");
    }
    std::vector<Bookmark> probe = marks_;
    if (RestoreDefaultBookmarks(probe) > 0) {
        if (onChip) {
            AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(menu.get(), MF_STRING, kRestore, L"Вернуть закладки облачных ИИ");
    }
    if (GetMenuItemCount(menu.get()) <= 0) {
        return;
    }
    POINT at = point;
    ClientToScreen(host_, &at);
    const int chosen = menu_(menu.get(), at);
    const size_t index = onChip ? static_cast<size_t>(hit) : 0;
    switch (chosen) {
    case kOpen:
        OpenMark(index, false);
        break;
    case kNewTab:
        OpenMark(index, true);
        break;
    case kLeft:
        if (onChip && index > 0) {
            std::swap(marks_[index], marks_[index - 1]);
            SaveMarks();
        }
        break;
    case kRight:
        if (onChip && index + 1 < marks_.size()) {
            std::swap(marks_[index], marks_[index + 1]);
            SaveMarks();
        }
        break;
    case kRemove:
        if (onChip) {
            marks_.erase(marks_.begin() + static_cast<std::ptrdiff_t>(index));
            hot_ = kHitNone;
            SaveMarks();
        }
        break;
    case kRestore:
        RestoreDefaultBookmarks(marks_);
        SaveMarks();
        break;
    default:
        break;
    }
}

void BrowserView::PaintBar(HDC dc, const RECT& client) {
    const int bar = BarHeight();
    if (client.right <= 0 || bar <= 0) {
        return;
    }
    // Drawn aside and put down whole: a hover repaints the row without a flash.
    const HDC memory = CreateCompatibleDC(dc);
    const HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, bar);
    if (memory == nullptr || bitmap == nullptr) {
        if (memory != nullptr) {
            DeleteDC(memory);
        }
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        PaintChrome(dc, client);
        return;
    }
    const HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    const RECT area{0, 0, client.right, bar};
    if (ground_ != nullptr) {
        FillRect(memory, &area, ground_);
    }
    PaintChrome(memory, client);
    const std::wstring current = BookmarkHost(ActiveSource());
    const auto iconBox = [&](const Chip& chip) {
        const int size = Dip(14);
        const int left = static_cast<int>(chip.rect.left) + Dip(5);
        const int top = static_cast<int>(chip.rect.top + chip.rect.bottom) / 2 - size / 2;
        return RECT{left, top, left + size, top + size};
    };
    const auto rounded = [](Gdiplus::GraphicsPath& path, float l, float t, float r, float b, float radius) {
        const float d = radius * 2.f;
        path.AddArc(l, t, d, d, 180.f, 90.f);
        path.AddArc(r - d, t, d, d, 270.f, 90.f);
        path.AddArc(r - d, b - d, d, d, 0.f, 90.f);
        path.AddArc(l, b - d, d, d, 90.f, 90.f);
        path.CloseFigure();
    };
    std::vector<bool> lettered(chips_.size(), false);
    if (gdip_ != 0) {
        Gdiplus::Graphics canvas(memory);
        canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        canvas.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        canvas.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        for (size_t c = 0; c < chips_.size(); ++c) {
            const Chip& chip = chips_[c];
            const Bookmark& mark = marks_[chip.index];
            const std::wstring host = BookmarkHost(mark.url);
            if (hot_ == static_cast<int>(chip.index)) {
                Gdiplus::GraphicsPath back;
                rounded(back, static_cast<float>(chip.rect.left), static_cast<float>(chip.rect.top),
                        static_cast<float>(chip.rect.right), static_cast<float>(chip.rect.bottom), DipF(6.f));
                const Gdiplus::SolidBrush hover(Gdiplus::Color(255, 0x23, 0x23, 0x27));
                canvas.FillPath(&hover, &back);
            }
            const RECT box = iconBox(chip);
            const auto boxF = Gdiplus::RectF(static_cast<float>(box.left), static_cast<float>(box.top),
                                             static_cast<float>(box.right - box.left),
                                             static_cast<float>(box.bottom - box.top));
            if (Gdiplus::Bitmap* icon = Icon(host)) {
                canvas.DrawImage(icon, boxF);
            } else {
                // No icon yet (the site has not been opened here): its letter on a tint of its own, muted.
                unsigned hash = 2166136261u;
                for (const wchar_t ch : host) {
                    hash = (hash ^ static_cast<unsigned>(ch)) * 16777619u;
                }
                const float hue = static_cast<float>(hash % 360u);
                const auto channel = [&](float n) {
                    const float k = std::fmod(n + hue / 30.f, 12.f);
                    const float a = 0.30f * std::min(0.30f, 1.f - 0.30f);
                    return static_cast<BYTE>(255.f * (0.30f - a * std::max(-1.f, std::min({k - 3.f, 9.f - k, 1.f}))));
                };
                Gdiplus::GraphicsPath tile;
                rounded(tile, boxF.X, boxF.Y, boxF.X + boxF.Width, boxF.Y + boxF.Height, DipF(3.5f));
                const Gdiplus::SolidBrush tint(Gdiplus::Color(255, channel(0.f), channel(8.f), channel(4.f)));
                canvas.FillPath(&tint, &tile);
                lettered[c] = true;
            }
            if (host == current) {  // the site on show: a short line under its icon
                const float mid = boxF.X + boxF.Width / 2.f;
                const Gdiplus::SolidBrush lit(Gdiplus::Color(255, 0x4c, 0x92, 0xff));
                Gdiplus::GraphicsPath line;
                rounded(line, mid - DipF(5.f), static_cast<float>(chip.rect.bottom) - DipF(2.5f), mid + DipF(5.f),
                        static_cast<float>(chip.rect.bottom) - DipF(0.5f), DipF(1.f));
                canvas.FillPath(&lit, &line);
            }
        }
        if (more_.right > more_.left) {  // "»": the ones that did not fit
            if (hot_ == kHitMore) {
                Gdiplus::GraphicsPath back;
                rounded(back, static_cast<float>(more_.left), static_cast<float>(more_.top),
                        static_cast<float>(more_.right), static_cast<float>(more_.bottom), DipF(6.f));
                const Gdiplus::SolidBrush hover(Gdiplus::Color(255, 0x23, 0x23, 0x27));
                canvas.FillPath(&hover, &back);
            }
            const BYTE shade = hot_ == kHitMore ? 0xf2 : 0x9c;
            Gdiplus::Pen pen(Gdiplus::Color(255, shade, shade, shade), std::max(1.f, DipF(1.3f)));
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            const float mx = (more_.left + more_.right) / 2.f;
            const float my = (more_.top + more_.bottom) / 2.f;
            for (const float shift : {-DipF(2.5f), DipF(1.5f)}) {
                const Gdiplus::PointF chevron[3] = {{mx + shift - DipF(1.5f), my - DipF(3.5f)},
                                                    {mx + shift + DipF(1.5f), my},
                                                    {mx + shift - DipF(1.5f), my + DipF(3.5f)}};
                canvas.DrawLines(&pen, chevron, 3);
            }
        }
    }
    SetBkMode(memory, TRANSPARENT);
    const HGDIOBJ chipFont = chipFont_ != nullptr ? static_cast<HGDIOBJ>(chipFont_) : GetStockObject(DEFAULT_GUI_FONT);
    const HGDIOBJ oldFont = SelectObject(memory, chipFont);
    for (size_t c = 0; c < chips_.size(); ++c) {
        const Chip& chip = chips_[c];
        const Bookmark& mark = marks_[chip.index];
        const RECT box = iconBox(chip);
        if (lettered[c] && monoFont_ != nullptr && !mark.name.empty()) {
            SelectObject(memory, monoFont_);
            SetTextColor(memory, RGB(0xe6, 0xe8, 0xec));
            RECT letterBox = box;
            const wchar_t letter[2] = {static_cast<wchar_t>(std::towupper(mark.name[0])), 0};
            DrawTextW(memory, letter, 1, &letterBox, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(memory, chipFont);
        }
        const bool bright = hot_ == static_cast<int>(chip.index) || BookmarkHost(mark.url) == current;
        SetTextColor(memory, bright ? RGB(0xf2, 0xf4, 0xf8) : RGB(0x9c, 0x9e, 0xa4));
        RECT text{box.right + Dip(6), chip.rect.top, chip.rect.right - Dip(8), chip.rect.bottom};
        DrawTextW(memory, mark.name.c_str(), static_cast<int>(mark.name.size()), &text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(memory, oldFont);
    BitBlt(dc, 0, 0, client.right, bar, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
}

void BrowserView::WatchIcon(ICoreWebView2* view) {
    wil::com_ptr<ICoreWebView2_15> fifteen;
    if (view == nullptr || FAILED(view->QueryInterface(IID_PPV_ARGS(fifteen.put())))) {
        return;  // an older WebView2: the letters stay
    }
    EventRegistrationToken token{};
    fifteen->add_FaviconChanged(Microsoft::WRL::Callback<ICoreWebView2FaviconChangedEventHandler>(
                                    [this](ICoreWebView2* sender, IUnknown*) {
                                        FetchIcon(sender);
                                        return S_OK;
                                    })
                                    .Get(),
                                &token);
}

// A bookmarked site's icon, kept as favicons\<host>.png beside bookmarks.json once the site has been open here.
void BrowserView::FetchIcon(ICoreWebView2* view) {
    wil::com_ptr<ICoreWebView2_15> fifteen;
    if (view == nullptr || FAILED(view->QueryInterface(IID_PPV_ARGS(fifteen.put())))) {
        return;
    }
    wil::unique_cotaskmem_string source;
    if (FAILED(view->get_Source(&source)) || source == nullptr) {
        return;
    }
    const std::wstring host = BookmarkHost(source.get());
    if (host.empty() ||
        std::ranges::none_of(marks_, [&](const Bookmark& mark) { return BookmarkHost(mark.url) == host; })) {
        return;
    }
    fifteen->GetFavicon(COREWEBVIEW2_FAVICON_IMAGE_FORMAT_PNG,
                        Microsoft::WRL::Callback<ICoreWebView2GetFaviconCompletedHandler>(
                            [this, host](HRESULT result, IStream* stream) -> HRESULT {
                                if (shutdown_ || FAILED(result) || stream == nullptr) {
                                    return S_OK;
                                }
                                std::string bytes;
                                char chunk[8192];
                                ULONG read = 0;
                                while (SUCCEEDED(stream->Read(chunk, sizeof(chunk), &read)) && read > 0 &&
                                       bytes.size() < (1u << 20)) {
                                    bytes.append(chunk, read);
                                }
                                std::shared_ptr<Gdiplus::Bitmap> icon = DecodeIcon(bytes);
                                if (!icon) {
                                    return S_OK;
                                }
                                const std::filesystem::path folder =
                                    std::filesystem::path(BookmarksFolder()) / L"favicons";
                                std::error_code error;
                                std::filesystem::create_directories(folder, error);
                                std::ofstream(folder / (host + L".png"), std::ios::binary | std::ios::trunc) << bytes;
                                icons_[host] = std::move(icon);
                                InvalidateBar();
                                return S_OK;
                            })
                            .Get());
}

Gdiplus::Bitmap* BrowserView::Icon(const std::wstring& host) {
    if (host.empty() || gdip_ == 0) {
        return nullptr;
    }
    if (const auto found = icons_.find(host); found != icons_.end()) {
        return found->second.get();
    }
    std::shared_ptr<Gdiplus::Bitmap>& slot = icons_[host];
    std::ifstream in(std::filesystem::path(BookmarksFolder()) / L"favicons" / (host + L".png"), std::ios::binary);
    if (in) {
        std::stringstream bytes;
        bytes << in.rdbuf();
        slot = DecodeIcon(bytes.str());
    }
    return slot.get();
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
    view->add_HistoryChanged(  // the arrow lights or dims with the page's history
        Microsoft::WRL::Callback<ICoreWebView2HistoryChangedEventHandler>(
            [this](ICoreWebView2* sender, IUnknown*) {
                if (sender != nullptr && sender == ActiveView()) {
                    InvalidateBar();
                }
                return S_OK;
            })
            .Get(),
        &token);
    view->add_SourceChanged(
        Microsoft::WRL::Callback<ICoreWebView2SourceChangedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs*) {
                if (sender != nullptr && sender == ActiveView()) {
                    InvalidateBar();  // the star and the lit chip follow the page
                }
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
    WatchIcon(view);
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
            [this, view](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
                wil::unique_cotaskmem_string uri;
                const bool allowed = SUCCEEDED(args->get_Uri(&uri)) && uri && MayNavigate(ToUtf8(uri.get()));
                if (!allowed) {
                    args->put_Handled(TRUE);
                } else if (environment_ && host_ != nullptr && !shutdown_) {
                    // A real window tied to the page that opened it: a sign-in (Google, Kimi) answers
                    // that page through window.opener and then closes itself. Navigating this very page
                    // would throw the opener away and leave the sign-in blank.
                    OpenPopup(args);
                } else {
                    args->put_Handled(TRUE);
                    view->Navigate(uri.get());
                }
                return S_OK;
            })
            .Get(),
        &token);
    view->add_WindowCloseRequested(
        Microsoft::WRL::Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
            [this](ICoreWebView2* sender, IUnknown*) {
                CloseRequested(sender);
                return S_OK;
            })
            .Get(),
        &token);
    wil::com_ptr<ICoreWebView2_4> four;
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(four.put())))) {
        four->add_DownloadStarting(  // a file lands in Downloads only after the person says yes (the agent's page too)
            Microsoft::WRL::Callback<ICoreWebView2DownloadStartingEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) {
                    std::wstring name = L"(без имени)", from;
                    wil::unique_cotaskmem_string path;
                    if (SUCCEEDED(args->get_ResultFilePath(&path)) && path) {
                        name = std::filesystem::path(path.get()).filename().wstring();
                    }
                    wil::com_ptr<ICoreWebView2DownloadOperation> operation;
                    wil::unique_cotaskmem_string uri;
                    if (SUCCEEDED(args->get_DownloadOperation(&operation)) && operation &&
                        SUCCEEDED(operation->get_Uri(&uri)) && uri) {
                        from = BookmarkHost(uri.get());
                    }
                    const std::wstring text = L"Сохранить файл в Загрузки?\n\n" + name +
                                              (from.empty() ? L"" : L"\nс " + from);
                    if (MessageBoxW(host_ != nullptr ? host_ : window_, text.c_str(), L"Загрузка из браузера",
                                    MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) {
                        args->put_Cancel(TRUE);
                    }
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

void BrowserView::SetPopupHooks(std::function<void(int)> opened, std::function<void(int)> closed) {
    popupOpened_ = std::move(opened);
    popupClosed_ = std::move(closed);
}

// The page asked for a new window. It gets a page of its own (a slot after the person's tabs) that
// the engine builds as that window, so that window.opener and postMessage keep working.
void BrowserView::OpenPopup(ICoreWebView2NewWindowRequestedEventArgs* args) {
    wil::com_ptr<ICoreWebView2Deferral> deferral;
    if (FAILED(args->GetDeferral(deferral.put()))) {
        args->put_Handled(TRUE);
        return;
    }
    wil::com_ptr<ICoreWebView2NewWindowRequestedEventArgs> request(args);
    extras_.push_back({});
    const size_t index = extras_.size() - 1;
    const int slot = static_cast<int>(index) + 1;
    extras_[index].opening = true;
    const HRESULT creating = environment_->CreateCoreWebView2Controller(
        host_, Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                   [this, index, slot, deferral, request](HRESULT created, ICoreWebView2Controller* controller) -> HRESULT {
                       auto giveUp = [&] {
                           request->put_Handled(TRUE);
                           deferral->Complete();
                           if (index < extras_.size()) {
                               extras_[index].opening = false;
                               extras_[index].closed = true;
                           }
                           if (controller != nullptr) {
                               controller->Close();
                           }
                           if (!shutdown_ && popupClosed_ && index < extras_.size()) {
                               popupClosed_(slot);
                           }
                       };
                       if (index >= extras_.size() || shutdown_ || FAILED(created) || controller == nullptr) {
                           giveUp();
                           return S_OK;
                       }
                       ExtraPage& arrived = extras_[index];
                       arrived.opening = false;
                       arrived.controller = controller;
                       arrived.controller->get_CoreWebView2(arrived.view.put());
                       if (!arrived.view) {
                           arrived.controller.reset();
                           giveUp();
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
                       request->put_NewWindow(arrived.view.get());
                       request->put_Handled(TRUE);
                       deferral->Complete();
                       if (popupOpened_) {
                           popupOpened_(slot);  // the tab appears and shows this page
                       } else {
                           ShowPage(slot);
                       }
                       LayoutPage();
                       if (host_ != nullptr) {
                           InvalidateRect(host_, nullptr, FALSE);
                       }
                       return S_OK;
                   })
                   .Get());
    if (FAILED(creating)) {
        extras_[index].opening = false;
        extras_[index].closed = true;
        args->put_Handled(TRUE);
        deferral->Complete();
        if (popupClosed_) {
            popupClosed_(slot);
        }
    }
}

// window.close() from a page: drop its tab, but not inside the engine's own event.
void BrowserView::CloseRequested(ICoreWebView2* view) {
    if (view == nullptr || view == view_.get() || shutdown_) {
        return;  // the tool's page is not closed by a page
    }
    for (size_t i = 0; i < extras_.size(); ++i) {
        if (extras_[i].view.get() == view) {
            const int slot = static_cast<int>(i) + 1;
            Post([this, slot] {
                if (popupClosed_) {
                    popupClosed_(slot);
                } else {
                    ClosePage(slot);
                }
            });
            return;
        }
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
    InvalidateBar();
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
