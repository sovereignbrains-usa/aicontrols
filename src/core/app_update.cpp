#include "core/app_update.h"

#include <windows.h>

#include <softpub.h>
#include <wintrust.h>

#include <filesystem>

#include "core/claude_install.h"
#include "core/claude_process.h"

namespace aic {
namespace {

namespace fs = std::filesystem;

// The installer's AppId (installer\aicontrols.iss): its uninstall entry is "<AppId>_is1".
constexpr const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{B7D2E4A1-3C95-4F68-A0E1-52D9C8F4B613}_is1";

}  // namespace

std::string InstalledAppVersion() {
    wchar_t value[64]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, value, &size) !=
        ERROR_SUCCESS) {
        return {};
    }
    return ToUtf8(value);
}

std::string SetupVersion(const std::wstring& fileName) {
    constexpr std::wstring_view kPrefix = L"AIControls-Setup-";
    constexpr std::wstring_view kSuffix = L".exe";
    if (fileName.size() <= kPrefix.size() + kSuffix.size() || !fileName.starts_with(kPrefix) ||
        !fileName.ends_with(kSuffix)) {
        return {};
    }
    const std::wstring version = fileName.substr(kPrefix.size(), fileName.size() - kPrefix.size() - kSuffix.size());
    if (version.find_first_not_of(L"0123456789.") != std::wstring::npos || version.front() == L'.' ||
        version.back() == L'.') {
        return {};
    }
    return ToUtf8(version);
}

std::optional<AppUpdate> FindAppUpdate(const std::vector<std::wstring>& folders, const std::string& installed) {
    std::optional<AppUpdate> best;
    for (const std::wstring& folder : folders) {
        std::error_code error;
        for (const fs::directory_entry& entry : fs::directory_iterator(folder, error)) {
            const std::string version = SetupVersion(entry.path().filename().wstring());
            if (version.empty() || !entry.is_regular_file(error)) {
                continue;
            }
            const bool newer = installed.empty() || ClaudeVersionLess(installed, version);
            if (newer && (!best || ClaudeVersionLess(best->version, version))) {
                best = AppUpdate{version, entry.path().wstring()};
            }
        }
    }
    return best;
}

std::vector<std::wstring> AppUpdateFolders() {
    wchar_t profile[MAX_PATH]{};
    GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    return {(fs::path(profile) / L"projects" / L"aicontrols" / L"build" / L"installer").wstring()};
}

bool InstallerSignatureValid(const std::wstring& path) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    const LONG status = WinVerifyTrust(nullptr, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return status == ERROR_SUCCESS;
}

}  // namespace aic
