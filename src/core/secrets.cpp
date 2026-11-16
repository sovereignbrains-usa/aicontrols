#include "core/secrets.h"

#include <windows.h>

#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "core/claude_process.h"

namespace aic {
namespace {

// Entropy mixed into DPAPI: another program running as the same user does not decrypt it by accident.
constexpr char kEntropy[] = "AIControls secrets v1";

DATA_BLOB Blob(const std::string& bytes) {
    return DATA_BLOB{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data()))};
}

std::mutex gAskPass;

std::filesystem::path AskPassDir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    const std::filesystem::path root = length > 0 && length < MAX_PATH
                                           ? std::filesystem::path(std::wstring(buffer, length))
                                           : std::filesystem::temp_directory_path();
    return root / L"AIControls" / L"askpass";
}

bool HexToken(std::wstring_view token) {
    if (token.size() != 32) {
        return false;
    }
    for (const wchar_t c : token) {
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) {
            return false;
        }
    }
    return true;
}

void SweepTickets(const std::filesystem::path& dir) {
    const auto now = std::filesystem::file_time_type::clock::now();
    std::vector<std::filesystem::path> stale;
    std::error_code error;
    for (std::filesystem::directory_iterator it(dir, error); !error && it != std::filesystem::directory_iterator();
         it.increment(error)) {
        std::error_code timeError;
        const auto written = it->last_write_time(timeError);
        if (timeError) {
            continue;
        }
        const auto age = now - written;
        if (age < decltype(age)::zero() || age <= std::chrono::minutes(2)) {
            continue;
        }
        stale.push_back(it->path());
    }
    for (const std::filesystem::path& path : stale) {
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
    }
}

std::wstring NewToken() {
    unsigned char bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring token(32, L'0');
    for (int i = 0; i < 16; ++i) {
        token[static_cast<size_t>(i) * 2] = kHex[bytes[i] >> 4];
        token[static_cast<size_t>(i) * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
    SecureZeroMemory(bytes, sizeof(bytes));
    return token;
}

}  // namespace

std::wstring ArmAskPass(const std::string& name) {
    if (!name.starts_with("ssh ")) {
        return {};
    }
    std::lock_guard lock(gAskPass);
    const std::filesystem::path dir = AskPassDir();
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        return {};
    }
    SweepTickets(dir);
    const std::wstring token = NewToken();
    if (!HexToken(token)) {
        return {};
    }
    const std::filesystem::path path = dir / token;
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << name;
        if (!file) {
            std::filesystem::remove(path, error);
            return {};
        }
    }
    return token;
}

std::optional<std::string> ClaimAskPass(std::wstring_view token) {
    if (!HexToken(token)) {
        return std::nullopt;
    }
    std::lock_guard lock(gAskPass);
    const std::filesystem::path path = AskPassDir() / std::wstring(token);
    const HANDLE opened = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (opened == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    struct CloseFile {
        HANDLE handle;
        ~CloseFile() {
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
        }
    } guard{opened};

    FILETIME written{};
    if (!GetFileTime(opened, nullptr, nullptr, &written)) {
        return std::nullopt;
    }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER writtenTicks{};
    writtenTicks.LowPart = written.dwLowDateTime;
    writtenTicks.HighPart = written.dwHighDateTime;
    ULARGE_INTEGER nowTicks{};
    nowTicks.LowPart = now.dwLowDateTime;
    nowTicks.HighPart = now.dwHighDateTime;
    constexpr ULONGLONG kTwoMinutes = 2ULL * 60 * 10000000ULL;
    if (nowTicks.QuadPart >= writtenTicks.QuadPart && nowTicks.QuadPart - writtenTicks.QuadPart > kTwoMinutes) {
        return std::nullopt;
    }

    char buffer[1024]{};
    DWORD read = 0;
    if (!ReadFile(opened, buffer, sizeof(buffer), &read, nullptr) || read == 0 || read >= sizeof(buffer)) {
        return std::nullopt;
    }
    const std::string name(buffer, read);
    if (!name.starts_with("ssh ")) {
        return std::nullopt;
    }
    return name;
}

std::wstring SecretStore::DefaultPath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
    std::wstring folder = length > 0 && length < MAX_PATH ? std::wstring(buffer, length) : L".";
    return folder + L"\\AIControls\\secrets.bin";
}

SecretStore::SecretStore(std::wstring path) : path_(std::move(path)) {}

bool SecretStore::Load() {
    std::ifstream file{std::filesystem::path(path_), std::ios::binary};
    if (!file) {
        return false;
    }
    const std::string encrypted((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::string entropyText(kEntropy);
    DATA_BLOB in = Blob(encrypted);
    DATA_BLOB entropy = Blob(entropyText);
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    const std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);

    const Json parsed = Json::parse(plain, nullptr, false);
    if (!parsed.is_object()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    values_.clear();
    for (const auto& [name, value] : parsed.items()) {
        if (value.is_string()) {
            values_[name] = value.get<std::string>();
        }
    }
    return true;
}

bool SecretStore::Save() const {
    Json object = Json::object();
    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, value] : values_) {
            object[name] = value;
        }
    }
    std::string plain = object.dump(-1, ' ', false, Json::error_handler_t::replace);
    const std::string entropyText(kEntropy);
    DATA_BLOB in = Blob(plain);
    DATA_BLOB entropy = Blob(entropyText);
    DATA_BLOB out{};
    const BOOL protectedOk =
        CryptProtectData(&in, L"AIControls secrets", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    SecureZeroMemory(plain.data(), plain.size());
    if (!protectedOk) {
        return false;
    }
    const std::filesystem::path target(path_);
    std::error_code ignored;
    std::filesystem::create_directories(target.parent_path(), ignored);
    // Written beside the target and moved over it: a crash never leaves half a file.
    const std::filesystem::path temporary = target.wstring() + L".tmp";
    bool ok = false;
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(out.pbData), static_cast<std::streamsize>(out.cbData));
        ok = file.good();
    }
    LocalFree(out.pbData);
    if (!ok) {
        return false;
    }
    return MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::vector<std::string> SecretStore::Names() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> names;
    for (const auto& entry : values_) {
        names.push_back(entry.first);
    }
    return names;
}

bool SecretStore::Has(const std::string& name) const {
    std::lock_guard lock(mutex_);
    return values_.contains(name);
}

std::optional<std::string> SecretStore::Get(const std::string& name) const {
    std::lock_guard lock(mutex_);
    if (const auto it = values_.find(name); it != values_.end()) {
        return it->second;
    }
    return std::nullopt;
}

void SecretStore::Set(const std::string& name, std::string value) {
    std::lock_guard lock(mutex_);
    values_[name] = std::move(value);
}

bool SecretStore::Remove(const std::string& name) {
    std::lock_guard lock(mutex_);
    return values_.erase(name) > 0;
}

std::string SecretStore::Substitute(std::string_view text, std::vector<std::string>& missing) const {
    constexpr std::string_view open = "{{secret:";
    std::string out;
    std::lock_guard lock(mutex_);
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t start = text.find(open, pos);
        const size_t end = start == std::string_view::npos ? start : text.find("}}", start + open.size());
        if (end == std::string_view::npos) {
            out.append(text.substr(pos));
            break;
        }
        out.append(text.substr(pos, start - pos));
        const std::string name(text.substr(start + open.size(), end - start - open.size()));
        if (const auto it = values_.find(name); it != values_.end()) {
            out += it->second;
        } else {
            missing.push_back(name);
            out.append(text.substr(start, end + 2 - start));
        }
        pos = end + 2;
    }
    return out;
}

std::string SecretStore::Redact(std::string_view text) const {
    std::vector<std::string> values;
    {
        std::lock_guard lock(mutex_);
        for (const auto& entry : values_) {
            if (entry.second.size() >= 3) {  // shorter ones would shred ordinary output
                values.push_back(entry.second);
            }
        }
    }
    // Longest first, so a value that contains another is cut out whole.
    std::ranges::sort(values, [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    std::string out(text);
    for (const std::string& value : values) {
        for (size_t at = out.find(value); at != std::string::npos; at = out.find(value, at + 3)) {
            out.replace(at, value.size(), "***");
        }
    }
    return out;
}

std::vector<std::pair<std::wstring, std::wstring>> SecretStore::Environment(
    const std::vector<std::string>& names, std::vector<std::string>& missing) const {
    std::vector<std::pair<std::wstring, std::wstring>> pairs;
    std::lock_guard lock(mutex_);
    for (const std::string& name : names) {
        if (const auto it = values_.find(name); it != values_.end()) {
            pairs.emplace_back(ToWide(name), ToWide(it->second));
        } else {
            missing.push_back(name);
        }
    }
    return pairs;
}

}  // namespace aic
