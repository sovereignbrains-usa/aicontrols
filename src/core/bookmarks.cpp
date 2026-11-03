#include "core/bookmarks.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/claude_process.h"

namespace aic {
namespace {

namespace fs = std::filesystem;

std::wstring g_folder;  // empty: %LOCALAPPDATA%\AIControls

fs::path File() {
    return fs::path(BookmarksFolder()) / L"bookmarks.json";
}

std::wstring Lower(std::wstring_view text) {
    std::wstring lower(text);
    std::ranges::transform(lower, lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return lower;
}

std::wstring Trim(std::wstring_view text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return std::wstring(text.substr(first, last - first + 1));
}

// The comparable form: lower-case scheme and host, no fragment, no trailing "/".
std::wstring Normal(std::wstring_view url) {
    std::wstring text = Trim(url);
    if (const size_t hash = text.find(L'#'); hash != std::wstring::npos) {
        text.resize(hash);
    }
    const size_t scheme = text.find(L"://");
    if (scheme != std::wstring::npos) {
        const size_t pathStart = text.find(L'/', scheme + 3);
        const size_t hostEnd = pathStart == std::wstring::npos ? text.size() : pathStart;
        text = Lower(text.substr(0, hostEnd)) + text.substr(hostEnd);
    }
    while (!text.empty() && text.back() == L'/') {
        text.pop_back();
    }
    return text;
}

}  // namespace

std::vector<Bookmark> DefaultBookmarks() {
    // The chats people use most, the order roughly by how often; all open in the tab as they are.
    return {
        {L"ChatGPT", L"https://chatgpt.com/"},
        {L"Claude", L"https://claude.ai/"},
        {L"Gemini", L"https://gemini.google.com/"},
        {L"Grok", L"https://grok.com/"},
        {L"Qwen", L"https://chat.qwen.ai/"},
        {L"DeepSeek", L"https://chat.deepseek.com/"},
        {L"Perplexity", L"https://www.perplexity.ai/"},
        {L"Le Chat", L"https://chat.mistral.ai/"},
        {L"Kimi", L"https://www.kimi.com/"},
        {L"GLM", L"https://chat.z.ai/"},
        {L"Copilot", L"https://copilot.microsoft.com/"},
        {L"AI Studio", L"https://aistudio.google.com/"},
    };
}

void SetBookmarksFolder(const std::wstring& folder) {
    g_folder = folder;
}

std::wstring BookmarksFolder() {
    if (!g_folder.empty()) {
        return g_folder;
    }
    wchar_t local[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return (fs::path(local) / L"AIControls").wstring();
}

std::vector<Bookmark> LoadBookmarks() {
    std::error_code error;
    if (!fs::exists(File(), error)) {
        return DefaultBookmarks();
    }
    std::ifstream in(File(), std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const Json parsed = Json::parse(text.str(), nullptr, false);
    if (!parsed.is_array()) {
        return DefaultBookmarks();  // a broken file: the set to start again with, the file stays until a change
    }
    std::vector<Bookmark> bookmarks;
    for (const Json& item : parsed) {
        if (!item.is_object()) {
            continue;
        }
        Bookmark bookmark{Trim(ToWide(JsonStr(item, "name"))), Trim(ToWide(JsonStr(item, "url")))};
        if (BookmarkHost(bookmark.url).empty()) {
            continue;
        }
        if (bookmark.name.empty()) {
            bookmark.name = BookmarkHost(bookmark.url);
        }
        bookmarks.push_back(std::move(bookmark));
    }
    return bookmarks;
}

bool SaveBookmarks(const std::vector<Bookmark>& bookmarks) {
    Json list = Json::array();
    for (const Bookmark& bookmark : bookmarks) {
        list.push_back({{"name", ToUtf8(bookmark.name)}, {"url", ToUtf8(bookmark.url)}});
    }
    std::error_code error;
    fs::create_directories(File().parent_path(), error);
    const fs::path temp = File().wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << list.dump(2);
        if (!out) {
            return false;
        }
    }
    return MoveFileExW(temp.c_str(), File().c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::wstring BookmarkHost(std::wstring_view url) {
    const std::wstring text = Lower(Trim(url));
    size_t start = 0;
    if (text.starts_with(L"https://")) {
        start = 8;
    } else if (text.starts_with(L"http://")) {
        start = 7;
    } else {
        return {};
    }
    const size_t end = text.find_first_of(L"/?#:", start);
    std::wstring host = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    if (const size_t at = host.rfind(L'@'); at != std::wstring::npos) {
        host.erase(0, at + 1);
    }
    if (host.starts_with(L"www.")) {
        host.erase(0, 4);
    }
    return host;
}

bool SameAddress(std::wstring_view a, std::wstring_view b) {
    return Normal(a) == Normal(b);
}

std::wstring BookmarkName(std::wstring_view title, std::wstring_view url) {
    std::wstring name = Trim(title);
    for (const std::wstring_view separator : {L" - ", L" | ", L" — ", L" – ", L" · "}) {
        if (const size_t at = name.find(separator); at != std::wstring::npos && at > 0) {
            name.resize(at);
        }
    }
    name = Trim(name);
    constexpr size_t kMost = 24;
    if (name.size() > kMost) {
        size_t cut = name.rfind(L' ', kMost);
        if (cut == std::wstring::npos || cut < kMost / 2) {
            cut = kMost;
        }
        name = Trim(name.substr(0, cut)) + L"…";
    }
    if (name.empty()) {
        name = BookmarkHost(url);
    }
    return name;
}

size_t RestoreDefaultBookmarks(std::vector<Bookmark>& bookmarks) {
    size_t added = 0;
    for (Bookmark& bookmark : DefaultBookmarks()) {
        const std::wstring host = BookmarkHost(bookmark.url);
        const bool present = std::ranges::any_of(bookmarks, [&](const Bookmark& b) { return BookmarkHost(b.url) == host; });
        if (!present) {
            bookmarks.push_back(std::move(bookmark));
            ++added;
        }
    }
    return added;
}

}  // namespace aic
