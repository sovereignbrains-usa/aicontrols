#include "core/secret_guard.h"

#include <richedit.h>

#include <algorithm>

#include <wil/resource.h>

namespace aic {
namespace {

// The field's own ground, and one step darker, so a print is bands rather than letter shapes.
constexpr COLORREF kSmearDark = RGB(0x23, 0x23, 0x27);
constexpr COLORREF kSmearDeep = RGB(0x12, 0x12, 0x14);
constexpr int kBand = 3;

// Win10 2004+. The SDK usually defines it; a copy keeps an older header building.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

struct ScrubTarget {
    HDC dc = nullptr;
    RECT capture{};
    int originX = 0;
    int originY = 0;
};

BOOL CALLBACK ScrubOne(HWND hwnd, LPARAM param) {
    auto* scrub = reinterpret_cast<ScrubTarget*>(param);
    wchar_t name[64]{};
    if (GetClassNameW(hwnd, name, 64) == 0 || wcscmp(name, kSecretRevealClass) != 0) {
        return TRUE;
    }
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
    }
    RECT window{};
    RECT hit{};
    if (!GetWindowRect(hwnd, &window) || !IntersectRect(&hit, &window, &scrub->capture)) {
        return TRUE;
    }
    const RECT local{hit.left - scrub->originX, hit.top - scrub->originY, hit.right - scrub->originX,
                     hit.bottom - scrub->originY};
    PaintSecretSmear(scrub->dc, local);
    return TRUE;
}

}  // namespace

bool GuardSecretField(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    // InSendMessage is set only for a SendMessage from another thread. The UI thread's own GetWindowText
    // (saving the secret) must still read the field.
    if (hwnd == nullptr || !InSendMessage()) {
        return false;
    }
    switch (message) {
    case WM_GETTEXT:
        if (lParam != 0 && wParam != 0) {
            reinterpret_cast<wchar_t*>(lParam)[0] = L'\0';
        }
        result = 0;
        return true;
    case WM_GETTEXTLENGTH:
    case EM_GETLINE:
    case EM_GETHANDLE:
    case EM_STREAMOUT:
    case WM_COPY:
    case WM_GETOBJECT:
    case EM_SETPASSWORDCHAR:  // a stealer clearing the mask would paint the letters into the field itself
        result = 0;
        return true;
    default:
        return false;
    }
}

void PaintSecretSmear(HDC dc, const RECT& rect) {
    if (dc == nullptr || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    const wil::unique_hbrush dark(CreateSolidBrush(kSmearDark));
    const wil::unique_hbrush deep(CreateSolidBrush(kSmearDeep));
    if (!dark || !deep) {
        return;
    }
    for (int y = rect.top; y < rect.bottom; y += kBand) {
        RECT band = rect;
        band.top = y;
        band.bottom = std::min<LONG>(static_cast<LONG>(y) + kBand, rect.bottom);
        FillRect(dc, &band, ((y - rect.top) / kBand) % 2 == 0 ? dark.get() : deep.get());
    }
}

void ScrubSecretReveal(HDC dc, int originX, int originY, int width, int height) {
    if (dc == nullptr || width <= 0 || height <= 0) {
        return;
    }
    ScrubTarget scrub;
    scrub.dc = dc;
    scrub.originX = originX;
    scrub.originY = originY;
    scrub.capture = RECT{originX, originY, originX + width, originY + height};
    EnumWindows(ScrubOne, reinterpret_cast<LPARAM>(&scrub));
}

bool ExcludeFromCapture(HWND hwnd, bool exclude) {
    if (hwnd == nullptr) {
        return false;
    }
    return SetWindowDisplayAffinity(hwnd, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) != FALSE;
}

}  // namespace aic
