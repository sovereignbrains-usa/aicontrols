// Pictures kept for the chat: saved once by their content, read back as previews. Exit code 0: all passed.
// Also the secret field: another thread's read gets nothing, and a capture of the reveal window is painted over.
#include <windows.h>
#include <wincodec.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <wil/com.h>

#include "core/attachment.h"
#include "core/secret_guard.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

// A 1x1 PNG.
constexpr const char* kPixel =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";

// A stand-in for a secret. The failure text never includes it.
constexpr wchar_t kProbe[] = L"zz91-field-probe";

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

struct FieldGuard {
    WNDPROC previous = nullptr;
};

LRESULT CALLBACK FieldGuardProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* guard = reinterpret_cast<FieldGuard*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    LRESULT result = 0;
    if (aic::GuardSecretField(hwnd, message, wParam, lParam, result)) {
        return result;
    }
    if (guard == nullptr || guard->previous == nullptr) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return CallWindowProcW(guard->previous, hwnd, message, wParam, lParam);
}

// True when this thread's own GetWindowText still returns the probe. The buffer is wiped before return.
bool FieldHoldsProbe(HWND edit) {
    wchar_t buffer[64]{};
    const int count = GetWindowTextW(edit, buffer, 64);
    const bool holds = count > 0 && wcscmp(buffer, kProbe) == 0;
    SecureZeroMemory(buffer, sizeof(buffer));
    return holds;
}

void PumpUntil(const std::atomic<bool>& done) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (!done.load() && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&message);
        }
        Sleep(1);
    }
}

// The edit keeps its dots. A SendMessage from another thread is what a stealer uses, and it gets nothing.
void SecretFieldGuard() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const HWND owner = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"Static", L"", WS_POPUP, -32000, -32000,
                                       80, 40, nullptr, nullptr, instance, nullptr);
    const HWND edit = owner == nullptr
                          ? nullptr
                          : CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_PASSWORD, 0, 0, 60, 20,
                                            owner, nullptr, instance, nullptr);
    Expect(edit != nullptr, "the secret field exists");
    if (edit == nullptr) {
        if (owner != nullptr) {
            DestroyWindow(owner);
        }
        return;
    }
    // Hidden on purpose: nothing is shown, so the test does not flash a window.
    SetWindowTextW(edit, kProbe);
    SendMessageW(edit, EM_SETPASSWORDCHAR, 0x25CF, 0);
    FieldGuard guard;
    SetWindowLongPtrW(edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&guard));
    guard.previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(FieldGuardProc)));
    Expect(FieldHoldsProbe(edit), "the field can be read on its own thread");

    struct Theft {
        HWND edit = nullptr;
        std::atomic<bool> done{false};
        std::atomic<bool> leaked{false};
        std::atomic<int> length{-1};
        std::atomic<int> mask{-1};
    } theft;
    theft.edit = edit;
    std::thread worker([&theft] {
        wchar_t buffer[64]{};
        wcscpy_s(buffer, kProbe);
        const LRESULT copied = SendMessageW(theft.edit, WM_GETTEXT, 64, reinterpret_cast<LPARAM>(buffer));
        if (!IsWindow(theft.edit)) {
            SecureZeroMemory(buffer, sizeof(buffer));
            theft.done.store(true);
            return;
        }
        const bool found = buffer[0] != L'\0' || wcsstr(buffer, kProbe) != nullptr || copied != 0;
        SecureZeroMemory(buffer, sizeof(buffer));
        theft.leaked.store(found);
        theft.length.store(static_cast<int>(SendMessageW(theft.edit, WM_GETTEXTLENGTH, 0, 0)));
        SendMessageW(theft.edit, EM_SETPASSWORDCHAR, 0, 0);
        theft.mask.store(static_cast<int>(SendMessageW(theft.edit, EM_GETPASSWORDCHAR, 0, 0)));
        theft.done.store(true);
    });
    PumpUntil(theft.done);
    if (!theft.done.load()) {
        DestroyWindow(edit);
    }
    worker.join();
    Expect(theft.done.load(), "the cross-thread read finished");
    Expect(!theft.leaked.load(), "cross-thread read leaked");
    Expect(theft.length.load() == 0, "cross-thread length is empty");
    Expect(theft.mask.load() == 0x25CF, "the password mask stays");
    if (IsWindow(edit)) {
        Expect(FieldHoldsProbe(edit), "the field still holds the value");
        Expect(SendMessageW(edit, EM_GETPASSWORDCHAR, 0, 0) == 0x25CF, "the mask is unchanged for the owner");
        DestroyWindow(edit);
    }
    if (IsWindow(owner)) {
        DestroyWindow(owner);
    }
}

// A top-level window can leave screen capture. A hidden one is tried first, so nothing is painted on a monitor.
void SecretAffinity() {
    Expect(!aic::ExcludeFromCapture(nullptr, true), "no window, no affinity");
    const HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"Static", L"", WS_POPUP, -32000, -32000,
                                      10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Expect(hwnd != nullptr, "the affinity window exists");
    if (hwnd == nullptr) {
        return;
    }
    bool took = aic::ExcludeFromCapture(hwnd, true);
    if (!took) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                          GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT);
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        took = aic::ExcludeFromCapture(hwnd, true);
    }
    Expect(took, "capture exclusion takes");
    DWORD affinity = 0;
    Expect(GetWindowDisplayAffinity(hwnd, &affinity) != FALSE && affinity == WDA_EXCLUDEFROMCAPTURE,
           "the window is excluded from capture");
    Expect(aic::ExcludeFromCapture(hwnd, false), "capture exclusion clears");
    affinity = WDA_EXCLUDEFROMCAPTURE;
    Expect(GetWindowDisplayAffinity(hwnd, &affinity) != FALSE && affinity == WDA_NONE,
           "the window is back in capture");
    DestroyWindow(hwnd);
}

// Bands over a marked bitmap. No letters are involved.
void SecretSmear() {
    const HDC screen = GetDC(nullptr);
    const HDC memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = memory == nullptr ? nullptr : CreateCompatibleBitmap(screen, 20, 20);
    Expect(bitmap != nullptr, "a smear bitmap exists");
    if (bitmap != nullptr) {
        const HGDIOBJ old = SelectObject(memory, bitmap);
        RECT all{0, 0, 20, 20};
        const HBRUSH marker = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(memory, &all, marker);
        DeleteObject(marker);
        aic::PaintSecretSmear(memory, all);
        const COLORREF corner = GetPixel(memory, 0, 0);
        const COLORREF next = GetPixel(memory, 0, 3);
        const COLORREF mid = GetPixel(memory, 5, 5);
        Expect(corner != RGB(255, 0, 255) && next != RGB(255, 0, 255) && mid != RGB(255, 0, 255),
               "a print is covered");
        Expect(corner != next, "a print is bands");
        aic::PaintSecretSmear(memory, RECT{0, 0, 0, 0});
        SelectObject(memory, old);
        DeleteObject(bitmap);
    }
    aic::PaintSecretSmear(nullptr, RECT{0, 0, 4, 4});
    if (memory != nullptr) {
        DeleteDC(memory);
    }
    if (screen != nullptr) {
        ReleaseDC(nullptr, screen);
    }
}

// The reveal class is visible to the scrubber and fully transparent, off the desktop, so a clamp onto a
// monitor still shows nothing and takes no clicks.
void SecretScrub() {
    WNDCLASSEXW revealClass{};
    revealClass.cbSize = sizeof(revealClass);
    revealClass.lpfnWndProc = DefWindowProcW;
    revealClass.hInstance = GetModuleHandleW(nullptr);
    revealClass.lpszClassName = aic::kSecretRevealClass;
    RegisterClassExW(&revealClass);
    const HWND reveal = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                        aic::kSecretRevealClass, L"", WS_POPUP, -32000, -32000, 40, 24, nullptr,
                                        nullptr, revealClass.hInstance, nullptr);
    Expect(reveal != nullptr, "the reveal window exists");
    if (reveal == nullptr) {
        return;
    }
    SetLayeredWindowAttributes(reveal, 0, 0, LWA_ALPHA);
    ShowWindow(reveal, SW_SHOWNOACTIVATE);
    Expect(IsWindowVisible(reveal) != FALSE, "the reveal window is visible to the scrub");
    RECT window{};
    Expect(GetWindowRect(reveal, &window) != FALSE && window.right > window.left && window.bottom > window.top,
           "the reveal window has a rectangle");
    const int originX = window.left - 8;
    const int originY = window.top - 8;
    const int width = (window.right - window.left) + 16;
    const int height = (window.bottom - window.top) + 16;
    const HDC screen = GetDC(nullptr);
    const HDC memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = memory == nullptr ? nullptr : CreateCompatibleBitmap(screen, width, height);
    Expect(bitmap != nullptr, "a scrub bitmap exists");
    if (bitmap != nullptr) {
        const HGDIOBJ old = SelectObject(memory, bitmap);
        RECT all{0, 0, width, height};
        const HBRUSH marker = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(memory, &all, marker);
        DeleteObject(marker);
        aic::ScrubSecretReveal(memory, originX, originY, width, height);
        const int insideX = (window.left - originX) + 2;
        const int insideY = (window.top - originY) + 2;
        Expect(GetPixel(memory, insideX, insideY) != RGB(255, 0, 255), "the reveal rectangle is scrubbed");
        Expect(GetPixel(memory, 1, 1) == RGB(255, 0, 255), "the margin stays");
        SelectObject(memory, old);
        DeleteObject(bitmap);
    }
    aic::ScrubSecretReveal(nullptr, originX, originY, width, height);
    if (memory != nullptr) {
        DeleteDC(memory);
    }
    if (screen != nullptr) {
        ReleaseDC(nullptr, screen);
    }
    DestroyWindow(reveal);
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 1;
    }
    // The pictures go under %LOCALAPPDATA%: a scratch one here, not the user's.
    const fs::path scratch = fs::temp_directory_path() / L"aic_attachment_test";
    std::error_code error;
    fs::remove_all(scratch, error);
    fs::create_directories(scratch, error);
    SetEnvironmentVariableW(L"LOCALAPPDATA", scratch.c_str());
    {
        const std::wstring first = aic::SaveChatImage(kPixel, "image/png");
        const std::wstring again = aic::SaveChatImage(kPixel, "image/png");
        Expect(!first.empty() && fs::exists(first), "a picture is saved");
        Expect(first == again, "the same picture is the same file");
        Expect(fs::path(first).extension() == L".png" && fs::path(first).parent_path() == scratch / L"AIControls" / L"images",
               "saved where it belongs, by its type");
        Expect(aic::SaveChatImage("", "image/png").empty(), "nothing to save, no file");

        wil::com_ptr<IWICImagingFactory> wic = wil::CoCreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);
        UINT width = 0;
        UINT height = 0;
        Expect(aic::LoadImagePreview(wic.get(), first, 64, width, height) && width == 1 && height == 1,
               "the preview reads it back with its size");
        Expect(!aic::LoadImagePreview(wic.get(), (scratch / L"missing.png").wstring(), 64, width, height) && width == 0,
               "a missing file gives no preview");
    }
    fs::remove_all(scratch, error);
    SecretFieldGuard();
    SecretAffinity();
    SecretSmear();
    SecretScrub();
    CoUninitialize();
    std::cout << (failures == 0 ? "all passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
