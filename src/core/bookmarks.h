#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aic {

// The browser tab's bookmarks: a row under the address, the cloud AIs to start with (his ask, 03.11.2026:
// "все облачные ии добавить в закладки"). Kept in %LOCALAPPDATA%\AIControls\bookmarks.json as
// [{"name":..,"url":..}]; no file means the starting set, an empty list stays empty.
struct Bookmark {
    std::wstring name;
    std::wstring url;
};

std::vector<Bookmark> DefaultBookmarks();
std::vector<Bookmark> LoadBookmarks();
bool SaveBookmarks(const std::vector<Bookmark>& bookmarks);
void SetBookmarksFolder(const std::wstring& folder);  // tests and the gallery: somewhere else
std::wstring BookmarksFolder();

// "chat.qwen.ai" from "https://chat.qwen.ai/c/123"; "www." dropped. Empty when it is not http(s).
std::wstring BookmarkHost(std::wstring_view url);
// One address written two ways is the same bookmark: the scheme's case, a trailing "/" and a "#..." do not count.
bool SameAddress(std::wstring_view a, std::wstring_view b);
// A short name for a page: its title cut at a separator (" - ", " | ", " — ") and on a word at 24, else the host.
std::wstring BookmarkName(std::wstring_view title, std::wstring_view url);
// The defaults that are not in the list (by host), appended; how many came back.
size_t RestoreDefaultBookmarks(std::vector<Bookmark>& bookmarks);

}  // namespace aic
