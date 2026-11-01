// By hand: the browser tool (core/browser.h) against the real Edge: open a URL, print the page's text, save a
// picture of it to %TEMP%\aic-browser.jpg, then close the window. Usage: aic_browser_probe [url [seconds to keep it open]] (default https://example.com).
// It opens a visible window and keeps the profile %LOCALAPPDATA%\AIControls\browser.
#include <windows.h>
#include <wincrypt.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "core/browser.h"
#include "core/claude_process.h"

namespace {

bool Report(const char* step, const aic::BrowserResult& result) {
    if (!result.error.empty()) {
        std::cout << "FAIL " << step << ": " << result.error << "\n";
        return false;
    }
    std::cout << "OK " << step << "\n" << result.text << "\n\n";
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::string url = argc > 1 ? aic::ToUtf8(argv[1]) : "https://example.com";
    const std::atomic<bool> never{false};
    bool ok = true;

    aic::BrowserRequest request;
    request.action = "open";
    request.url = url;
    ok = Report("open", aic::RunBrowserAction(request, never)) && ok;

    request = aic::BrowserRequest{};
    request.action = "screenshot";
    const aic::BrowserResult shot = aic::RunBrowserAction(request, never);
    if (!shot.error.empty() || shot.imageBase64.empty()) {
        std::cout << "FAIL screenshot: " << shot.error << "\n";
        ok = false;
    } else {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring path = std::wstring(temp) + L"aic-browser.jpg";
        const std::string bytes = [&] {
            // base64 back to bytes, through the system's decoder
            DWORD length = 0;
            CryptStringToBinaryA(shot.imageBase64.data(), static_cast<DWORD>(shot.imageBase64.size()),
                                 CRYPT_STRING_BASE64, nullptr, &length, nullptr, nullptr);
            std::string out(length, '\0');
            CryptStringToBinaryA(shot.imageBase64.data(), static_cast<DWORD>(shot.imageBase64.size()),
                                 CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(out.data()), &length, nullptr, nullptr);
            out.resize(length);
            return out;
        }();
        std::ofstream(std::filesystem::path(path), std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        std::cout << "OK screenshot: " << bytes.size() << " bytes -> " << aic::ToUtf8(path) << "\n\n";
    }

    if (argc > 2) {  // keep the window open for that many seconds, to look at it
        Sleep(static_cast<DWORD>(_wtoi(argv[2])) * 1000);
    }
    request = aic::BrowserRequest{};
    request.action = "close";
    ok = Report("close", aic::RunBrowserAction(request, never)) && ok;
    return ok ? 0 : 1;
}
