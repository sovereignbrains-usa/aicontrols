#pragma once

#include <windows.h>

#include <objbase.h>
#include <WebView2.h>
#include <wil/com.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/bookmarks.h"
#include "core/browser.h"

namespace Gdiplus {
class Bitmap;
}

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
    // UI thread: the window's own menu (its style) and its "another browser tab", for the bookmarks row.
    void SetHooks(std::function<int(HMENU, POINT)> menu, std::function<void()> newTab);
    void SetPopupHooks(std::function<void(int)> opened, std::function<void(int)> closed);
    // UI thread: the address field, so a person can type where to go. The tool does not call this.
    void FocusAddress();
    // UI thread: another page beside the tool's. 0 is the tool's page; the rest are the person's own tabs.
    // The number stays put after the page is closed, so a tab keeps pointing at it.
    int AddPage();
    void ShowPage(int slot);
    void ClosePage(int slot);
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
    void EnsureHost();   // the page's own window: the client's Direct2D present paints over child windows
    void ApplyPlace();
    void LayoutPage();
    void NavigateBar();
    void Go(const std::wstring& url);  // the page on show goes there (or will, once it exists)
    // The bookmarks row under the address, and the star at the address's right end.
    void LoadMarks();
    void LayoutBar();
    int HitBar(POINT point) const;  // a chip's index, or kHit*
    void SetHot(int hot);
    void OnBarClick(POINT point, bool middle);
    void OnBarMenu(POINT point);
    void ToggleStar();
    void OpenMark(size_t index, bool newTab);
    void SaveMarks();
    std::wstring ActiveSource() const;
    bool Marked(const std::wstring& source) const;
    void PaintBar(HDC dc, const RECT& client);
    void WatchIcon(ICoreWebView2* view);
    void FetchIcon(ICoreWebView2* view);
    Gdiplus::Bitmap* Icon(const std::wstring& host);
    void InvalidateBar();
    void PaintChrome(HDC dc, const RECT& client);
    void ApplyFont();
    RECT FieldRect(const RECT& client) const;
    int Dip(int dip) const;
    float DipF(float dip) const;
    int BarHeight() const;
    std::wstring AddressText() const;
    static std::wstring ResolveAddress(std::wstring text);
    ICoreWebView2* ActiveView() const;
    ICoreWebView2Controller* ActiveController() const;
    void WatchAddress(ICoreWebView2* view);
    void Arm(ICoreWebView2* view);
    void CreateExtra(size_t index);
    void OpenPopup(ICoreWebView2NewWindowRequestedEventArgs* args);
    void CloseRequested(ICoreWebView2* view);
    static LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR self);

    HWND window_;
    HWND host_ = nullptr;  // owned popup, over the column; the WebView is its child
    HWND edit_ = nullptr;  // the address, at the top of host_
    HFONT font_ = nullptr;
    HFONT chipFont_ = nullptr;   // the bookmarks' names
    HFONT monoFont_ = nullptr;   // the letter of a bookmark whose site has not shown its icon yet
    HBRUSH field_ = nullptr;
    HBRUSH ground_ = nullptr;
    RECT bounds_{};  // client coordinates of window_, the last place asked for
    bool placeVisible_ = false;
    bool focusBar_ = false;
    UINT message_;
    std::function<void()> show_;
    std::function<void()> hide_;
    std::function<int(HMENU, POINT)> menu_;
    std::function<void()> newTab_;
    std::function<void(int)> popupOpened_;  // a page opened a window (a sign-in): its slot, already a tab to show
    std::function<void(int)> popupClosed_;  // that window asked to close itself (window.close)

    // UI thread only: the bookmarks row.
    static constexpr int kHitNone = -1, kHitStar = -2, kHitMore = -3, kHitBack = -4;
    struct Chip {
        RECT rect{};
        size_t index = 0;
    };
    std::vector<Bookmark> marks_;
    bool marksLoaded_ = false;
    std::vector<Chip> chips_;          // the ones that fit, left to right
    std::vector<size_t> hidden_;       // the rest, behind the "»"
    RECT more_{};                      // the "»", empty when all fit
    RECT star_{};
    RECT back_{};                      // the "back" arrow, left of the address
    int hot_ = kHitNone;
    bool tracking_ = false;
    std::map<std::wstring, std::shared_ptr<Gdiplus::Bitmap>> icons_;  // by host; empty: looked, none
    std::atomic<bool> shown_{false};
    std::atomic<bool> shutdown_{false};

    // UI thread only.
    wil::com_ptr<ICoreWebView2Environment> environment_;
    wil::com_ptr<ICoreWebView2Controller> controller_;
    wil::com_ptr<ICoreWebView2> view_;
    wil::com_ptr<ICoreWebView2DevToolsProtocolEventReceiver> loadReceiver_;
    // The person's own pages (the "+" on a browser tab). Slot 0 is the tool's page above; these are 1, 2, ...
    struct ExtraPage {
        wil::com_ptr<ICoreWebView2Controller> controller;
        wil::com_ptr<ICoreWebView2> view;
        bool opening = false;  // the controller is on its way: do not start a second one
        bool closed = false;   // the tab went away before the controller arrived
    };
    std::vector<ExtraPage> extras_;
    int page_ = 0;             // which page the column is showing
    std::wstring pending_;     // typed before that page existed
    ULONG_PTR gdip_ = 0;
    bool creating_ = false;

    std::mutex mutex_;
    std::condition_variable changed_;
    bool ready_ = false;           // guarded by mutex_
    bool fresh_ = false;           // just made: nothing set on it yet
    std::string failure_;          // why it could not start
    std::map<std::string, int> events_;
};

}  // namespace aic
