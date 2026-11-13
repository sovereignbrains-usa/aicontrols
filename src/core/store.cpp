#include "core/store.h"

#include <shellapi.h>
#include <shlobj.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "core/attachment.h"
#include "core/claude_process.h"
#include "core/tools.h"

namespace aic {
namespace {

namespace fs = std::filesystem;

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
        return {};
    }
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

// The CLI's folder name for a workspace: every character but [A-Za-z0-9] becomes '-'.
fs::path ProjectDir(const std::wstring& workspace) {
    std::wstring name = workspace;
    for (wchar_t& c : name) {
        const bool keep = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
        if (!keep) {
            c = L'-';
        }
    }
    return fs::path(KnownFolder(FOLDERID_Profile)) / L".claude" / L"projects" / name;
}

fs::path SessionFile(const std::wstring& workspace, const std::string& id) {
    return ProjectDir(workspace) / (ToWide(id) + L".jsonl");
}

std::string ReadRange(const fs::path& file, uint64_t offset, uint64_t size) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(static_cast<std::streamoff>(offset));
    std::string data(static_cast<size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(size));
    data.resize(static_cast<size_t>(in.gcount()));
    return data;
}

template <typename Fn>
void ForEachLine(std::string_view text, Fn&& fn) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        if (end > start) {
            fn(text.substr(start, end - start));
        }
        start = end + 1;
    }
}

std::wstring TextOf(const Json& content, bool images = true);  // images: a line for each picture

// A text file sent along is shown as "📎 name", not its content (13.11.2026: a chat read again showed the whole file
// as the message). Now it comes as <attached_file name="…">…</attached_file>. Before, it was "Файл «name»:\n\n…":
// a block of its own in Claude's transcript (`whole`), but Grok joins the blocks with a space, so there the words
// typed after it are only guessed: past the last "\n " (a file ends in a line break).
std::string MarkAttachedFiles(std::string text, bool whole) {
    const auto mark = [](std::string_view name) { return "📎 " + std::string(name); };
    for (size_t open = text.find(kAttachedOpen); open != std::string::npos; open = text.find(kAttachedOpen, open)) {
        const size_t nameAt = open + kAttachedOpen.size();
        const size_t nameEnd = text.find("\">", nameAt);
        const size_t close = nameEnd == std::string::npos ? nameEnd : text.find(kAttachedClose, nameEnd);
        if (close == std::string::npos) {
            break;
        }
        size_t end = close + kAttachedClose.size();
        while (end < text.size() && (text[end] == ' ' || text[end] == '\r' || text[end] == '\n')) {
            ++end;
        }
        std::string line = mark(std::string_view(text).substr(nameAt, nameEnd - nameAt));
        line += end < text.size() ? "\n\n" : "";
        text.replace(open, end - open, line);
        open += line.size();
    }
    constexpr std::string_view kOld = "Файл «";
    constexpr std::string_view kOldEnd = "»:\n\n";
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || std::string_view(text).substr(first, kOld.size()) != kOld) {
        return text;
    }
    const size_t nameEnd = text.find(kOldEnd, first + kOld.size());
    if (nameEnd == std::string::npos || nameEnd - first > 400 ||
        text.find('\n', first) < nameEnd) {
        return text;
    }
    std::string shown = mark(std::string_view(text).substr(first + kOld.size(), nameEnd - first - kOld.size()));
    if (!whole) {
        if (const size_t join = text.rfind("\n "); join != std::string::npos && join > nameEnd) {
            const std::string_view words = std::string_view(text).substr(join + 2);
            if (words.find_first_not_of(" \t\r\n") != std::string_view::npos) {
                shown += "\n\n" + std::string(words);
            }
        }
    }
    return shown;
}

// A message as a title: the first line of the words; a picture or file mark (an attachment's line) only if there are
// none, and then without the mark, which a font may draw as a squiggle. Shortened.
std::wstring PromptTitle(const std::wstring& prompt) {
    const std::wstring_view mark = L"📎 ";
    std::wstring titled;
    std::wstring attached;
    for (size_t at = std::min(prompt.find_first_not_of(L" \t\r\n"), prompt.size()); at < prompt.size();) {
        const size_t end = std::min(prompt.find_first_of(L"\r\n", at), prompt.size());
        const std::wstring_view row = std::wstring_view(prompt).substr(at, end - at);
        if (row.starts_with(mark)) {
            attached = attached.empty() ? std::wstring(row.substr(mark.size())) : attached;
        } else if (!row.empty()) {
            titled = row;
            break;
        }
        at = end + 1;
    }
    std::wstring title = titled.empty() ? attached : titled;
    // Only until Haiku names the chat by what it is about (title_writer): the first sentence of it, not a whole
    // paragraph, shortened on a word. The row trims it again to its width; the tip shows this.
    for (size_t at = title.find_first_of(L".?!"); at != std::wstring::npos; at = title.find_first_of(L".?!", at + 1)) {
        if (at >= 12 && at + 1 < title.size() && title[at + 1] == L' ') {
            title.resize(title[at] == L'.' ? at : at + 1);  // a question keeps its mark
            break;
        }
    }
    constexpr size_t kLongest = 80;
    if (title.size() > kLongest) {
        size_t cut = title.find_last_of(L" \t", kLongest);
        if (cut == std::wstring::npos || cut < 40) {
            cut = kLongest;
        }
        title.resize(cut);
        while (!title.empty() && (title.back() == L' ' || title.back() == L'\t')) {
            title.pop_back();
        }
        title.push_back(L'\u2026');
    }
    return title;
}

// A chat the CLI never titled is named after its first message: its first line, shortened.
std::wstring FirstPrompt(std::string_view text) {
    std::wstring found;
    ForEachLine(text, [&](std::string_view line) {
        if (!found.empty() || line.find("\"type\":\"user\"") == std::string_view::npos) {
            return;
        }
        const Json entry = Json::parse(line, nullptr, false);
        if (JsonStr(entry, "type") != "user" || (JsonAt(entry, "isMeta").is_boolean() && JsonAt(entry, "isMeta").get<bool>())) {
            return;
        }
        const std::wstring prompt = TextOf(JsonAt(JsonAt(entry, "message"), "content"));
        const size_t first = prompt.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos || prompt[first] == L'<') {  // tool results, command and system insertions
            return;
        }
        found = PromptTitle(prompt);
    });
    return found;
}

void ScanTitles(std::string_view text, std::wstring& custom, std::wstring& generated) {
    ForEachLine(text, [&](std::string_view line) {
        if (line.find("\"custom-title\"") == std::string_view::npos &&
            line.find("\"ai-title\"") == std::string_view::npos) {
            return;
        }
        const Json entry = Json::parse(line, nullptr, false);  // a line cut by a chunk edge fails: skipped
        const std::string type = JsonStr(entry, "type");
        if (type == "custom-title" && !JsonStr(entry, "customTitle").empty()) {
            custom = ToWide(JsonStr(entry, "customTitle"));
        } else if (type == "ai-title" && !JsonStr(entry, "aiTitle").empty()) {
            generated = ToWide(JsonStr(entry, "aiTitle"));
        }
    });
}

// Titles are appended as the chat goes on, so the latest is near the end; long chats are read
// only at the head and the tail.
std::wstring ReadTitle(const fs::path& file, uint64_t size) {
    constexpr uint64_t kChunk = 512 * 1024;
    std::wstring custom;
    std::wstring generated;
    const std::string head = ReadRange(file, 0, std::min(size, 2 * kChunk));
    if (size <= 2 * kChunk) {
        ScanTitles(head, custom, generated);
    } else {
        ScanTitles(std::string_view(head).substr(0, kChunk), custom, generated);
        ScanTitles(ReadRange(file, size - kChunk, kChunk), custom, generated);
    }
    if (!custom.empty()) {
        return custom;
    }
    return !generated.empty() ? generated : FirstPrompt(head);
}

uint64_t ToTicks(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

struct CachedTitle {
    uint64_t modified = 0;
    uint64_t size = 0;
    std::wstring title;
};

// The titles' caches: the sidebar reads them, and so do list_chats and read_chat on their threads.
std::mutex g_titlesMutex;

std::wstring CachedReadTitle(const fs::path& file, const WIN32_FILE_ATTRIBUTE_DATA& data) {
    static std::map<std::wstring, CachedTitle> cache;
    const std::lock_guard lock(g_titlesMutex);
    const uint64_t modified = ToTicks(data.ftLastWriteTime);
    const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    CachedTitle& entry = cache[file.wstring()];
    if (entry.modified != modified || entry.size != size) {
        entry = {modified, size, ReadTitle(file, size)};
    }
    return entry.title;
}

std::wstring TextOf(const Json& content, bool images) {
    if (content.is_string()) {
        return ToWide(content.get<std::string>());
    }
    std::wstring text;
    if (content.is_array()) {
        for (const Json& block : content) {
            const std::string type = JsonStr(block, "type");
            std::wstring part;
            if (type == "text" && !JsonStr(block, "text").starts_with("<earlier_chat>")) {  // a switch's handoff
                part = ToWide(MarkAttachedFiles(JsonStr(block, "text"), true));
            } else if (type == "image" && images) {
                part = L"📎 фото";
            } else if (type == "document") {
                part = L"📎 " + (JsonStr(block, "title").empty() ? L"документ" : ToWide(JsonStr(block, "title")));
            }
            if (!part.empty()) {
                text += (text.empty() ? L"" : L"\n\n") + part;
            }
        }
    }
    return text;
}

bool IsTrue(const Json& value) {
    return value.is_boolean() && value.get<bool>();
}

// The last `limit` entries; `truncated` says there were more.
void KeepLatest(std::vector<TranscriptEntry>& entries, size_t limit, bool& truncated) {
    truncated = entries.size() > limit;
    if (truncated) {
        entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(limit));
    }
}

// The summary a CLI wrote for itself at a compaction goes with its mark, the newest one, for a switch to the other
// CLI. Kept from "Summary:" (Claude's own notes come before it) up to where it tells itself to go on.
void KeepSummary(std::vector<TranscriptEntry>& entries, std::wstring text) {
    const auto mark = std::ranges::find(entries.rbegin(), entries.rend(), EntryKind::Compacted, &TranscriptEntry::kind);
    if (mark == entries.rend()) {
        return;
    }
    if (const size_t start = text.find(L"Summary:"); start != std::wstring::npos) {
        text.erase(0, start);
    }
    for (const wchar_t* tail : {L"\nContinue the conversation from where", L"\nPlease continue the conversation"}) {
        if (const size_t end = text.find(tail); end != std::wstring::npos) {
            text.erase(end);
        }
    }
    const size_t first = text.find_first_not_of(L" \t\r\n");
    const size_t last = text.find_last_not_of(L" \t\r\n");
    mark->text = first == std::wstring::npos ? std::wstring() : text.substr(first, last - first + 1);
}

// ---- Grok's chats (grok 1.0.62, 27.10.2026): a folder each, its messages a line each in chat_history.jsonl:
// {"type":"user","content":[blocks],"prompt_index":N} what the user sent (without prompt_index: Grok's rules and
// reminders), {"type":"assistant","content":"…","tool_calls":[{id,name,arguments}]}, {"type":"tool_result",
// "tool_call_id":…,"content":"…"}.

constexpr const wchar_t* kGrokHistory = L"chat_history.jsonl";
constexpr const wchar_t* kGrokTitle = L"aicontrols-title.txt";  // a title given here; Grok's own files stay as they are

// The folder's name as Grok makes it: the path in UTF-8, all but [A-Za-z0-9-._~] as %XX.
std::wstring PercentEncode(const std::wstring& text) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::wstring encoded;
    for (const char c : ToUtf8(text)) {
        const auto byte = static_cast<unsigned char>(c);
        const bool keep = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
                          byte == '-' || byte == '.' || byte == '_' || byte == '~';
        if (keep) {
            encoded += static_cast<wchar_t>(byte);
        } else {
            encoded += L'%';
            encoded += static_cast<wchar_t>(kHex[byte >> 4]);
            encoded += static_cast<wchar_t>(kHex[byte & 0xF]);
        }
    }
    return encoded;
}

std::wstring PercentDecode(const std::wstring& name) {
    const auto hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    const std::string text = ToUtf8(name);
    std::string decoded;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            decoded += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else {
            decoded += text[i];
        }
    }
    return ToWide(decoded);
}

fs::path GrokProjectDir(const std::wstring& workspace) {
    const fs::path root = fs::path(KnownFolder(FOLDERID_Profile)) / L".grok" / L"sessions";
    const fs::path named = root / PercentEncode(workspace);
    std::error_code error;
    if (fs::is_directory(named, error)) {
        return named;
    }
    for (const fs::directory_entry& item : fs::directory_iterator(root, error)) {  // the path written in another case
        const std::wstring path = PercentDecode(item.path().filename().wstring());
        if (CompareStringOrdinal(path.c_str(), -1, workspace.c_str(), -1, TRUE) == CSTR_EQUAL) {
            return item.path();
        }
    }
    return named;
}

fs::path GrokSessionDir(const std::wstring& workspace, const std::string& id) {
    if (id.empty() || id.find_first_of("/\\.:") != std::string::npos) {
        return {};  // an id is a plain name; anything else could reach outside the chats' folder
    }
    return GrokProjectDir(workspace) / ToWide(id);
}

// What the user sent. The prompt Grok's compaction keeps loses its prompt_index; its <user_query> still tells it from
// Grok's rules, which have neither.
bool IsGrokPrompt(const Json& entry) {
    if (JsonStr(entry, "type") != "user" || entry.contains("synthetic_reason")) {
        return false;
    }
    if (JsonAt(entry, "prompt_index").is_number()) {
        return true;
    }
    const Json& content = JsonAt(entry, "content");
    if (content.is_string()) {
        return content.get<std::string>().find("<user_query>") != std::string::npos;
    }
    return content.is_array() && std::ranges::any_of(content, [](const Json& block) {
        return JsonStr(block, "type") == "text" && JsonStr(block, "text").find("<user_query>") != std::string::npos;
    });
}

// The words as they were typed: Grok puts them in <user_query>, after its list of the pictures it saved; a switch's
// handoff (<earlier_chat>) comes first among them.
std::wstring GrokPromptText(const Json& content) {
    std::string text = content.is_string() ? content.get<std::string>() : std::string();
    if (content.is_array()) {
        for (const Json& block : content) {
            if (JsonStr(block, "type") == "text") {
                text += JsonStr(block, "text");
            }
        }
    }
    constexpr std::string_view kOpen = "<user_query>";
    constexpr std::string_view kClose = "</user_query>";
    const size_t open = text.find(kOpen);
    const size_t close = text.rfind(kClose);
    if (open != std::string::npos && close != std::string::npos && close > open) {
        text = text.substr(open + kOpen.size(), close - open - kOpen.size());
    }
    constexpr std::string_view kHandoffEnd = "</earlier_chat>";
    if (const size_t start = text.find_first_not_of(" \t\r\n");
        start != std::string::npos && std::string_view(text).substr(start).starts_with("<earlier_chat>")) {
        const size_t end = text.find(kHandoffEnd);
        text.erase(0, end == std::string::npos ? text.size() : end + kHandoffEnd.size());
    }
    text = MarkAttachedFiles(std::move(text), false);
    const size_t first = text.find_first_not_of(" \t\r\n");
    const size_t last = text.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::wstring() : ToWide(std::string_view(text).substr(first, last - first + 1));
}

// The pictures sent with it: {"type":"image","url":"data:image/png;base64,…"}.
std::vector<TranscriptImage> GrokPromptImages(const Json& content) {
    std::vector<TranscriptImage> images;
    if (!content.is_array()) {
        return images;
    }
    for (const Json& block : content) {
        if (JsonStr(block, "type") != "image") {
            continue;
        }
        const std::string url = JsonStr(block, "url");
        const size_t data = url.find(";base64,");
        if (url.starts_with("data:") && data != std::string::npos) {
            images.push_back({url.substr(5, data - 5), url.substr(data + 8)});
        }
    }
    return images;
}

// ---- What Grok's compaction took out of the history (grok 1.0.62, 29.10.2026): compaction\segment_NNN.md, one per
// compaction, its turns verbatim. "### Turn N (Human)" holds the words in <user_query>…</user_query> (Grok's own
// insertions have none) and "[image]" after them if pictures came; "### Turn N (Assistant)" its words, then a
// "[tool_request: …]" line per call; "(Function)" and "(System)" the rest. The part after a compaction opens with the
// prompt it fell into once more.

std::vector<fs::path> GrokSegments(const fs::path& folder) {
    std::vector<fs::path> segments;
    std::error_code error;
    for (const fs::directory_entry& item : fs::directory_iterator(folder / L"compaction", error)) {
        const std::wstring name = item.path().filename().wstring();
        if (name.starts_with(L"segment_") && item.path().extension() == L".md") {
            segments.push_back(item.path());
        }
    }
    std::ranges::sort(segments);
    return segments;
}

// "### Turn 12 (Assistant)" -> "Assistant"; anything else, empty.
std::string_view SegmentTurn(std::string_view line) {
    constexpr std::string_view kTurn = "### Turn ";
    if (!line.starts_with(kTurn) || !line.ends_with(")")) {
        return {};
    }
    const size_t open = line.find(" (", kTurn.size());
    const std::string_view number = line.substr(kTurn.size(), open == std::string_view::npos ? 0 : open - kTurn.size());
    if (number.empty() || !std::ranges::all_of(number, [](char c) { return c >= '0' && c <= '9'; })) {
        return {};
    }
    return line.substr(open + 2, line.size() - open - 3);
}

// The user's words and Grok's in one segment; the calls are left out, the segment keeps them as text only. firstOnly:
// up to the first prompt, for the title.
std::vector<TranscriptEntry> ReadGrokSegment(const fs::path& file, bool firstOnly) {
    std::vector<TranscriptEntry> entries;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    std::string_view turn;
    std::string text;
    bool query = false;     // inside <user_query>
    bool asked = false;     // the turn had one
    bool pictures = false;  // "[image]"
    bool calls = false;     // past the turn's first call
    const auto flush = [&] {
        if (turn == "Human" && asked) {
            std::wstring words = GrokPromptText(Json(text));
            if (pictures) {
                words += (words.empty() ? L"" : L"\n\n") + std::wstring(L"📎 фото");
            }
            if (!words.empty()) {
                entries.push_back({EntryKind::User, std::move(words)});
            }
        } else if (turn == "Assistant") {
            const size_t first = text.find_first_not_of(" \t\r\n");
            const size_t last = text.find_last_not_of(" \t\r\n");
            if (first != std::string::npos) {
                entries.push_back({EntryKind::Assistant, ToWide(std::string_view(text).substr(first, last - first + 1))});
            }
        }
        text.clear();
        query = asked = pictures = calls = false;
    };
    while (std::getline(in, line)) {
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        if (const std::string_view next = SegmentTurn(line); !next.empty()) {
            flush();
            if (firstOnly && !entries.empty() && entries.back().kind == EntryKind::User) {
                return entries;
            }
            turn = next == "Human" ? "Human" : next == "Assistant" ? "Assistant" : "Other";
            continue;
        }
        if (turn == "Human") {
            if (line == "<user_query>") {
                query = asked = true;
                text.clear();
            } else if (line == "</user_query>") {
                query = false;
            } else if (query) {
                text += line + '\n';
            } else if (line == "[image]") {
                pictures = true;
            }
        } else if (turn == "Assistant" && !calls) {
            if (line.starts_with("[tool_request:")) {
                calls = true;
            } else {
                text += line + '\n';
            }
        }
    }
    flush();
    return entries;
}

// A Grok chat is named after its first message (Grok's own titles come in English); after a compaction that is in
// its first segment. nullopt: nothing was sent, the session is a start of grok.exe and no chat. The lines before it
// are Grok's rules, a few.
std::optional<std::wstring> ReadGrokTitle(const fs::path& folder) {
    const std::vector<fs::path> segments = GrokSegments(folder);
    if (!segments.empty()) {
        for (const TranscriptEntry& entry : ReadGrokSegment(segments.front(), true)) {
            if (entry.kind == EntryKind::User) {
                return PromptTitle(entry.text);
            }
        }
    }
    std::ifstream in(folder / kGrokHistory, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("\"prompt_index\"") == std::string::npos && line.find("<user_query>") == std::string::npos) {
            continue;
        }
        const Json entry = Json::parse(line, nullptr, false);
        if (!IsGrokPrompt(entry)) {
            continue;
        }
        const Json& content = JsonAt(entry, "content");
        std::wstring words = GrokPromptText(content);
        const bool pictures = content.is_array() && std::ranges::any_of(content, [](const Json& block) {
            return JsonStr(block, "type") == "image";
        });
        if (pictures) {
            words += (words.empty() ? L"" : L"\n\n") + std::wstring(L"📎 фото");
        }
        return PromptTitle(words);
    }
    if (!segments.empty()) {
        return std::wstring(L"Чат Grok");  // compacted, so a chat, though its words were not found
    }
    return std::nullopt;
}

struct CachedGrokTitle {
    uint64_t modified = 0;
    uint64_t size = 0;
    std::optional<std::wstring> title;
};

// data: the history's; a compaction rewrites it too.
std::optional<std::wstring> GrokTitle(const fs::path& folder, const WIN32_FILE_ATTRIBUTE_DATA& data) {
    static std::map<std::wstring, CachedGrokTitle> cache;
    const std::lock_guard lock(g_titlesMutex);
    const uint64_t modified = ToTicks(data.ftLastWriteTime);
    const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    CachedGrokTitle& entry = cache[folder.wstring()];
    if (entry.modified != modified || entry.size != size) {
        entry = {modified, size, ReadGrokTitle(folder)};
    }
    return entry.title;
}

// The title given in the sidebar, if one was.
std::wstring GrokOwnTitle(const fs::path& folder) {
    std::ifstream in(folder / kGrokTitle, std::ios::binary);
    if (!in) {
        return {};
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return ToWide(text);
}

// After a compaction: the mark, and the prompt it fell into is not said twice.
void MarkGrokCompaction(std::vector<TranscriptEntry>& entries, std::wstring& repeated) {
    repeated.clear();
    for (auto said = entries.rbegin(); said != entries.rend(); ++said) {
        if (said->kind == EntryKind::User) {
            repeated = said->text;
            break;
        }
    }
    entries.push_back({EntryKind::Compacted});
}

std::vector<TranscriptEntry> LoadGrokTranscript(const fs::path& folder) {
    std::vector<TranscriptEntry> entries;
    std::wstring repeated;  // the first prompt after a compaction, if it is the one before it
    const auto add = [&](TranscriptEntry said) {
        if (!repeated.empty() && said.kind == EntryKind::User) {
            // A segment marks pictures in the words ("📎 фото"); the history keeps them apart.
            const bool again = !said.text.empty() && repeated.starts_with(said.text);
            repeated.clear();
            if (again) {
                return;
            }
        }
        entries.push_back(std::move(said));
    };
    for (const fs::path& segment : GrokSegments(folder)) {
        if (!entries.empty()) {
            MarkGrokCompaction(entries, repeated);
        }
        for (TranscriptEntry& said : ReadGrokSegment(segment, false)) {
            add(std::move(said));
        }
    }
    bool compacted = false;  // the history's own mark: its rules come back with synthetic_reason "compaction_meta"
    std::ifstream in(folder / kGrokHistory, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const Json entry = Json::parse(line, nullptr, false);
        const std::string type = JsonStr(entry, "type");
        if (JsonStr(entry, "synthetic_reason") == "compaction_meta") {
            if (!compacted) {
                compacted = true;
                MarkGrokCompaction(entries, repeated);
            }
            // Its rules come in tags (<user_info>, <rules>); its summary is words.
            const std::wstring text = TextOf(JsonAt(entry, "content"), false);
            if (const size_t first = text.find_first_not_of(L" \t\r\n");
                first != std::wstring::npos && text[first] != L'<') {
                KeepSummary(entries, text);
            }
        } else if (type == "user" && IsGrokPrompt(entry)) {
            const Json& content = JsonAt(entry, "content");
            TranscriptEntry said{EntryKind::User, GrokPromptText(content), GrokPromptImages(content)};
            if (!said.text.empty() || !said.images.empty()) {
                add(std::move(said));
            }
        } else if (type == "assistant") {
            if (std::wstring text = ToWide(JsonStr(entry, "content")); !text.empty()) {
                entries.push_back({EntryKind::Assistant, std::move(text)});
            }
            const Json& calls = JsonAt(entry, "tool_calls");
            if (!calls.is_array()) {
                continue;
            }
            for (const Json& call : calls) {
                std::string name = JsonStr(call, "name");
                if (name == "search_tool") {
                    continue;  // Grok looking up our tools' descriptions: nothing was done
                }
                Json input = Json::parse(JsonStr(call, "arguments"), nullptr, false);
                if (name == "use_tool" && JsonAt(input, "tool_input").is_object()) {
                    name = "mcp__" + JsonStr(input, "tool_name");  // as it comes live: aic__read_file -> mcp__aic__read_file
                    Json inner = JsonAt(input, "tool_input");
                    input = std::move(inner);
                } else if (GrokOwnTool(name)) {
                    name = kGrokToolPrefix + name;  // image_gen -> grok__image_gen
                }
                TranscriptEntry tool{EntryKind::Tool, ToWide(name)};
                tool.toolId = JsonStr(call, "id");
                if (!input.is_discarded()) {
                    tool.toolInput = input.dump(-1, ' ', false, Json::error_handler_t::replace);
                }
                entries.push_back(std::move(tool));
            }
        } else if (type == "tool_result") {
            const std::string id = JsonStr(entry, "tool_call_id");
            for (auto call = entries.rbegin(); call != entries.rend(); ++call) {
                if (call->kind != EntryKind::Tool || call->toolId != id) {
                    continue;
                }
                std::wstring output = ToWide(JsonStr(entry, "content"));
                const size_t end = output.find_last_not_of(L" \t\r\n");
                output.resize(end == std::wstring::npos ? 0 : end + 1);
                // How Grok keeps a call that failed: "Tool `image_gen` failed: ...".
                call->toolFailed = output.starts_with(L"Tool `") && output.find(L"` failed: ") != std::wstring::npos;
                call->toolOutput = std::move(output);
                call->toolAnswered = true;
                break;
            }
        } else if (type == "backend_tool_call") {  // a web search, done on xAI's side
            const Json& kind = JsonAt(entry, "kind");
            if (JsonStr(kind, "tool_type") == "web_search") {
                TranscriptEntry search{EntryKind::Tool, ToWide(std::string(kGrokToolPrefix) + "web_search")};
                search.toolInput = JsonAt(kind, "action").dump(-1, ' ', false, Json::error_handler_t::replace);
                search.toolAnswered = true;
                entries.push_back(std::move(search));
            }
        }
    }
    return entries;
}

fs::path SettingsFile() {
    return fs::path(KnownFolder(FOLDERID_RoamingAppData)) / L"AIControls" / L"settings.json";
}

bool g_settingsReadOnly = false;

// ---- chats.json: {"chats":[{"sessions":[id…],"title":"…","active":ticks}]}. Kept by session id alone (a UUID either
// CLI gives), so a chat moved to another project keeps its line.

struct ChatRecord {
    std::vector<std::string> sessions;  // oldest first
    std::wstring title;                 // given in the sidebar
    uint64_t active = 0;                // the last message sent, FILETIME ticks; 0: not known
};

struct ChatIndex {
    std::vector<ChatRecord> records;
    uint64_t stamp = 0;  // the file's, as last read or written
    uint64_t size = 0;
    bool read = false;
};

std::mutex g_chatsMutex;  // list_chats and read_chat list the chats on their threads
ChatIndex g_chats;
std::wstring g_chatsFile;  // tests: their own

fs::path ChatIndexFile() {
    return !g_chatsFile.empty() ? fs::path(g_chatsFile) : SettingsFile().parent_path() / L"chats.json";
}

// The index as the file has it now. Under g_chatsMutex.
ChatIndex& Chats() {
    const fs::path file = ChatIndexFile();
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) {
        return g_chats;  // none yet: what this run has noted, if anything
    }
    const uint64_t stamp = ToTicks(data.ftLastWriteTime);
    const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (g_chats.read && g_chats.stamp == stamp && g_chats.size == size) {
        return g_chats;
    }
    g_chats = {};
    g_chats.read = true;
    g_chats.stamp = stamp;
    g_chats.size = size;
    std::ifstream in(file, std::ios::binary);
    const Json parsed = Json::parse(in, nullptr, false);
    for (const Json& chat : JsonAt(parsed, "chats")) {
        ChatRecord record;
        for (const Json& id : JsonAt(chat, "sessions")) {
            if (id.is_string() && !id.get<std::string>().empty()) {
                record.sessions.push_back(id.get<std::string>());
            }
        }
        record.title = ToWide(JsonStr(chat, "title"));
        if (const Json& active = JsonAt(chat, "active"); active.is_number_unsigned()) {
            record.active = active.get<uint64_t>();
        }
        if (!record.sessions.empty()) {
            g_chats.records.push_back(std::move(record));
        }
    }
    return g_chats;
}

// Under g_chatsMutex. Written whole to a file beside it, then put in its place: a crash leaves the old one.
void SaveChats(ChatIndex& index) {
    if (g_settingsReadOnly) {
        return;
    }
    Json chats = Json::array();
    for (const ChatRecord& record : index.records) {
        Json item{{"sessions", record.sessions}};
        if (!record.title.empty()) {
            item["title"] = ToUtf8(record.title);
        }
        if (record.active != 0) {
            item["active"] = record.active;
        }
        chats.push_back(std::move(item));
    }
    const fs::path file = ChatIndexFile();
    fs::path temp = file;
    temp += L".tmp";
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << Json{{"chats", std::move(chats)}}.dump(1, '\t', false, Json::error_handler_t::replace) << "\n";
        if (!out.good()) {
            return;
        }
    }
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return;
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) {
        index.stamp = ToTicks(data.ftLastWriteTime);
        index.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }
}

ChatRecord* RecordOf(ChatIndex& index, const std::string& id) {
    for (ChatRecord& record : index.records) {
        if (std::ranges::find(record.sessions, id) != record.sessions.end()) {
            return &record;
        }
    }
    return nullptr;
}

FILETIME FromTicks(uint64_t ticks) {
    return FILETIME{static_cast<DWORD>(ticks & 0xFFFFFFFF), static_cast<DWORD>(ticks >> 32)};
}

// The sessions found in a workspace's folders, each its own chat, made into the chats the index says they are.
std::vector<SessionInfo> JoinChats(std::vector<SessionInfo> found) {
    std::vector<ChatRecord> records;
    {
        const std::lock_guard lock(g_chatsMutex);
        records = Chats().records;
    }
    std::map<std::string, size_t> recordOf;
    for (size_t r = 0; r < records.size(); ++r) {
        for (const std::string& id : records[r].sessions) {
            recordOf.emplace(id, r);
        }
    }
    std::vector<SessionInfo> chats;
    std::map<size_t, std::vector<SessionInfo>> parts;  // a record's sessions found here
    for (SessionInfo& session : found) {
        if (const auto r = recordOf.find(session.id); r != recordOf.end()) {
            parts[r->second].push_back(std::move(session));
            continue;
        }
        session.parts = {{session.id, session.grok}};
        session.linked = {session.id};
        chats.push_back(std::move(session));
    }
    for (auto& [r, list] : parts) {
        const ChatRecord& record = records[r];
        const auto place = [&](const SessionInfo& session) { return std::ranges::find(record.sessions, session.id); };
        std::ranges::sort(list, [&](const SessionInfo& a, const SessionInfo& b) { return place(a) < place(b); });
        std::wstring title = record.title;
        for (const SessionInfo& session : list) {  // else the first part's: the later ones open with the handoff
            if (title.empty()) {
                title = session.title;
            }
        }
        SessionInfo chat = list.back();
        for (const SessionInfo& session : list) {
            chat.parts.push_back({session.id, session.grok});
        }
        chat.linked = record.sessions;
        chat.title = std::move(title);
        if (record.active != 0) {
            chat.modified = FromTicks(record.active);
        }
        chats.push_back(std::move(chat));
    }
    std::ranges::sort(chats, [](const SessionInfo& a, const SessionInfo& b) {
        return ToTicks(a.modified) > ToTicks(b.modified);
    });
    return chats;
}

}  // namespace

std::wstring DefaultWorkspace() {
    const fs::path old = fs::path(KnownFolder(FOLDERID_RoamingAppData)) / L"ssh-desktop-app" / L"claude-workspace";
    std::error_code error;
    if (fs::is_directory(old, error)) {
        return old.wstring();
    }
    return KnownFolder(FOLDERID_Profile);
}

std::wstring MemoryFolder(const std::wstring& workspace) {
    return (ProjectDir(workspace) / L"memory").wstring();
}

std::wstring SharedMemoryFolder() {
    return MemoryFolder(DefaultWorkspace());
}

std::vector<SessionInfo> ListSessions(const std::wstring& workspace) {
    std::vector<SessionInfo> sessions;
    std::error_code error;
    for (const fs::directory_entry& item : fs::directory_iterator(ProjectDir(workspace), error)) {
        if (!item.is_regular_file(error) || item.path().extension() != L".jsonl") {
            continue;
        }
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(item.path().c_str(), GetFileExInfoStandard, &data)) {
            continue;
        }
        SessionInfo session;
        session.id = ToUtf8(item.path().stem().wstring());
        session.title = CachedReadTitle(item.path(), data);
        session.modified = data.ftLastWriteTime;
        sessions.push_back(std::move(session));
    }
    for (const fs::directory_entry& item : fs::directory_iterator(GrokProjectDir(workspace), error)) {
        const fs::path history = item.path() / kGrokHistory;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!item.is_directory(error) || !GetFileAttributesExW(history.c_str(), GetFileExInfoStandard, &data)) {
            continue;
        }
        const std::optional<std::wstring> prompt = GrokTitle(item.path(), data);
        if (!prompt) {
            continue;
        }
        SessionInfo session;
        session.id = ToUtf8(item.path().filename().wstring());
        session.title = GrokOwnTitle(item.path());
        if (session.title.empty()) {
            session.title = *prompt;
        }
        session.modified = data.ftLastWriteTime;
        session.grok = true;
        sessions.push_back(std::move(session));
    }
    return JoinChats(std::move(sessions));
}

void NoteChatActive(const std::string& id, uint64_t ticks) {
    if (id.empty() || ticks == 0) {
        return;
    }
    const std::lock_guard lock(g_chatsMutex);
    ChatIndex& index = Chats();
    if (ChatRecord* record = RecordOf(index, id)) {
        if (record->active == ticks) {
            return;
        }
        record->active = ticks;
    } else {
        index.records.push_back({{id}, {}, ticks});
    }
    SaveChats(index);
}

void NoteChatOpened(const std::string& id, uint64_t ticks) {
    if (id.empty() || ticks == 0) {
        return;
    }
    const std::lock_guard lock(g_chatsMutex);
    ChatIndex& index = Chats();
    if (ChatRecord* record = RecordOf(index, id)) {
        if (record->active != 0) {
            return;
        }
        record->active = ticks;
    } else {
        index.records.push_back({{id}, {}, ticks});
    }
    SaveChats(index);
}

void LinkChats(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) {
        return;
    }
    const std::lock_guard lock(g_chatsMutex);
    ChatIndex& index = Chats();
    std::erase_if(index.records, [&](const ChatRecord& record) {  // `to` is new; a line of its own would be stale
        return record.sessions.size() == 1 && record.sessions.front() == to;
    });
    ChatRecord* record = RecordOf(index, from);
    if (record == nullptr) {
        index.records.push_back({{from}, {}, 0});
        record = &index.records.back();
    }
    const auto at = std::ranges::find(record->sessions, from);
    record->sessions.erase(at + 1, record->sessions.end());
    record->sessions.push_back(to);
    SaveChats(index);
}

void SetChatIndexFile(const std::wstring& file) {
    const std::lock_guard lock(g_chatsMutex);
    g_chatsFile = file;
    g_chats = {};
}

bool DeleteSession(const std::wstring& workspace, const std::string& id) {
    if (id.empty() || id.find_first_of("/\\.:") != std::string::npos) {
        return false;  // an id is a plain name; anything else could reach outside the chats' folder
    }
    const fs::path file = SessionFile(workspace, id);
    const fs::path folder = ProjectDir(workspace) / ToWide(id);
    const fs::path grok = GrokSessionDir(workspace, id);
    std::error_code error;
    std::wstring paths;  // SHFileOperation's list: each path ends in \0, the list in one more
    for (const fs::path& path : {file, folder, grok}) {
        if (fs::exists(path, error)) {
            paths += path.wstring();
            paths += L'\0';
        }
    }
    if (paths.empty()) {
        return false;
    }
    paths += L'\0';
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = paths.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const bool deleted = SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted &&
                         !fs::exists(file, error) && !fs::exists(grok, error);
    if (deleted) {
        const std::lock_guard lock(g_chatsMutex);
        ChatIndex& index = Chats();
        if (const ChatRecord* record = RecordOf(index, id)) {
            index.records.erase(index.records.begin() + (record - index.records.data()));
            SaveChats(index);
        }
    }
    return deleted;
}

namespace {

std::wstring g_archiveRoot;
int g_archiveFailAfter = -1;  // tests: fail after this many parts; -1 never

bool SafeId(const std::string& id) {
    return !id.empty() && id.find_first_of("/\\.:") == std::string::npos;
}

bool SameSize(const fs::path& left, const fs::path& right) {
    std::error_code a, b;
    return fs::file_size(left, a) == fs::file_size(right, b) && !a && !b;
}

// NTFS compression for a folder: files made in it afterwards are stored packed, and read back as they were. Archived
// transcripts are text in thousands of near-equal lines (Grok's phase changes, tool output), about ten to one. Where
// the volume cannot (FAT, a share) nothing happens and the copy is as big as before.
void CompressFolder(const fs::path& folder) {
    const HANDLE handle = CreateFileW(folder.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    USHORT format = COMPRESSION_FORMAT_DEFAULT;
    DWORD returned = 0;
    DeviceIoControl(handle, FSCTL_SET_COMPRESSION, &format, sizeof(format), nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
}

// A file, or a folder and everything in it. False if any of it did not arrive the same size.
bool CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code error;
    if (fs::is_regular_file(from, error)) {
        fs::create_directories(to.parent_path(), error);
        if (error) {
            return false;
        }
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
        return !error && SameSize(from, to);
    }
    if (!fs::is_directory(from, error)) {
        return false;
    }
    fs::create_directories(to, error);
    if (error) {
        return false;
    }
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(from, error)) {
        if (error) {
            return false;
        }
        const fs::path rel = fs::relative(entry.path(), from, error);
        if (error) {
            return false;
        }
        const fs::path dest = to / rel;
        if (entry.is_directory(error)) {
            fs::create_directories(dest, error);
        } else if (entry.is_regular_file(error)) {
            fs::create_directories(dest.parent_path(), error);
            if (!error) {
                fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, error);
            }
            if (!error && !SameSize(entry.path(), dest)) {
                return false;
            }
        }
        if (error) {
            return false;
        }
    }
    return !error;
}

void NoteTime(const fs::path& path, uint64_t& first, uint64_t& last) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return;
    }
    const uint64_t created = ToTicks(data.ftCreationTime);
    const uint64_t written = ToTicks(data.ftLastWriteTime);
    if (created != 0) {
        first = first == 0 ? created : std::min(first, created);
    }
    last = std::max(last, written);
}

// What DeleteSession would take for one part: the transcript, the folder beside it, Grok's folder. False if
// none of them is there, or one of them did not copy.
bool CopyPartFiles(const std::wstring& workspace, const std::string& id, const fs::path& dest) {
    std::error_code error;
    bool any = false;
    bool failed = false;
    if (const fs::path file = SessionFile(workspace, id); fs::is_regular_file(file, error)) {
        any = true;
        failed = !CopyTree(file, dest / L"session.jsonl") || failed;
    }
    if (const fs::path side = ProjectDir(workspace) / ToWide(id); fs::is_directory(side, error)) {
        any = true;
        failed = !CopyTree(side, dest / L"side") || failed;
    }
    if (const fs::path grok = GrokSessionDir(workspace, id); !grok.empty() && fs::is_directory(grok, error)) {
        any = true;
        failed = !CopyTree(grok, dest / L"grok") || failed;
    }
    return any && !failed;
}

fs::path ArchiveDir(const std::string& id) {
    return fs::path(ArchiveRoot()) / ToWide(id);
}

std::optional<ArchivedChat> ReadManifest(const fs::path& dir) {
    std::ifstream in(dir / L"manifest.json", std::ios::binary);
    const Json parsed = Json::parse(in, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    ArchivedChat chat;
    chat.id = ToUtf8(dir.filename().wstring());
    if (!SafeId(chat.id)) {
        return std::nullopt;
    }
    chat.title = ToWide(JsonStr(parsed, "title"));
    chat.folder = ToWide(JsonStr(parsed, "folder"));
    if (const Json& first = JsonAt(parsed, "first"); first.is_number_unsigned()) {
        chat.first = first.get<uint64_t>();
    }
    if (const Json& last = JsonAt(parsed, "last"); last.is_number_unsigned()) {
        chat.last = last.get<uint64_t>();
    }
    for (const Json& part : JsonAt(parsed, "parts")) {
        const std::string id = JsonStr(part, "id");
        if (!SafeId(id)) {
            continue;
        }
        chat.parts.push_back({id, IsTrue(JsonAt(part, "grok"))});
    }
    if (chat.parts.empty()) {
        return std::nullopt;
    }
    return chat;
}

}  // namespace

void SetArchiveRoot(const std::wstring& folder) {
    g_archiveRoot = folder;
}

void SetArchiveFailAfter(int copiedParts) {
    g_archiveFailAfter = copiedParts;
}

std::wstring ArchiveRoot() {
    if (!g_archiveRoot.empty()) {
        return g_archiveRoot;
    }
    return (fs::path(KnownFolder(FOLDERID_RoamingAppData)) / L"AIControls" / L"archive").wstring();
}

bool ArchiveChat(const std::wstring& workspace, const SessionInfo& session) {
    if (session.parts.empty() || !SafeId(session.id)) {
        return false;
    }
    for (const SessionPart& part : session.parts) {
        if (!SafeId(part.id)) {
            return false;
        }
    }
    const fs::path finalDir = ArchiveDir(session.id);
    std::error_code error;
    if (fs::exists(finalDir, error)) {
        return false;  // a copy is already there: leave both it and the live chat alone
    }
    const fs::path partial = fs::path(ArchiveRoot()) / (ToWide(session.id) + L".partial");
    fs::remove_all(partial, error);
    fs::create_directories(partial, error);
    if (error || !fs::is_directory(partial, error)) {
        return false;
    }
    CompressFolder(partial);  // what goes in is stored packed: transcripts shrink about tenfold
    const auto rollback = [&] {
        std::error_code gone;
        fs::remove_all(partial, gone);
    };
    uint64_t first = 0;
    uint64_t last = 0;
    int copied = 0;
    for (const SessionPart& part : session.parts) {
        if (g_archiveFailAfter >= 0 && copied >= g_archiveFailAfter) {
            rollback();
            return false;
        }
        if (!CopyPartFiles(workspace, part.id, partial / L"parts" / ToWide(part.id))) {
            rollback();
            return false;
        }
        NoteTime(SessionFile(workspace, part.id), first, last);
        NoteTime(ProjectDir(workspace) / ToWide(part.id), first, last);
        if (const fs::path grok = GrokSessionDir(workspace, part.id); !grok.empty()) {
            NoteTime(grok, first, last);
        }
        ++copied;
    }
    Json parts = Json::array();
    for (const SessionPart& part : session.parts) {
        parts.push_back(Json{{"id", part.id}, {"grok", part.grok}});
    }
    {
        std::ofstream out(partial / L"manifest.json", std::ios::binary | std::ios::trunc);
        out << Json{{"title", ToUtf8(session.title)},
                    {"folder", ToUtf8(workspace)},
                    {"first", first},
                    {"last", last},
                    {"parts", std::move(parts)}}
                   .dump(1, '\t', false, Json::error_handler_t::replace)
            << "\n";
        if (!out.good()) {
            rollback();
            return false;
        }
    }
    fs::rename(partial, finalDir, error);
    if (error) {
        rollback();
        return false;
    }
    return true;
}

bool DiscardArchive(const std::string& id) {
    if (!SafeId(id)) {
        return false;
    }
    const fs::path dir = ArchiveDir(id);
    std::error_code error;
    if (!fs::is_directory(dir, error)) {
        return false;
    }
    fs::remove_all(dir, error);
    return !error && !fs::exists(dir, error);
}

bool RestoreArchivedChat(const std::string& id) {
    if (!SafeId(id)) {
        return false;
    }
    const fs::path dir = ArchiveDir(id);
    const std::optional<ArchivedChat> chat = ReadManifest(dir);
    if (!chat) {
        return false;
    }
    struct Item {
        fs::path from;
        fs::path to;
    };
    std::vector<Item> items;
    std::error_code error;
    for (const SessionPart& part : chat->parts) {
        if (!SafeId(part.id)) {
            return false;
        }
        const fs::path partDir = dir / L"parts" / ToWide(part.id);
        if (fs::is_regular_file(partDir / L"session.jsonl", error)) {
            const fs::path to = SessionFile(chat->folder, part.id);
            if (fs::exists(to, error)) {
                return false;
            }
            items.push_back({partDir / L"session.jsonl", to});
        }
        if (fs::is_directory(partDir / L"side", error)) {
            const fs::path to = ProjectDir(chat->folder) / ToWide(part.id);
            if (fs::exists(to, error)) {
                return false;
            }
            items.push_back({partDir / L"side", to});
        }
        if (fs::is_directory(partDir / L"grok", error)) {
            const fs::path to = GrokSessionDir(chat->folder, part.id);
            if (to.empty() || fs::exists(to, error)) {
                return false;
            }
            items.push_back({partDir / L"grok", to});
        }
    }
    if (items.empty()) {
        return false;
    }
    for (const Item& item : items) {
        fs::create_directories(item.to.parent_path(), error);
        if (error || !CopyTree(item.from, item.to)) {
            return false;  // the archive stays, so a failed return can be tried again
        }
    }
    {
        const std::lock_guard lock(g_chatsMutex);
        ChatIndex& index = Chats();
        std::erase_if(index.records, [&](const ChatRecord& record) {
            return std::ranges::any_of(chat->parts, [&](const SessionPart& part) {
                return std::ranges::find(record.sessions, part.id) != record.sessions.end();
            });
        });
        ChatRecord record;
        for (const SessionPart& part : chat->parts) {
            record.sessions.push_back(part.id);
        }
        record.title = chat->title;
        record.active = chat->last;
        index.records.push_back(std::move(record));
        SaveChats(index);
    }
    fs::remove_all(dir, error);
    return !error && !fs::exists(dir, error);
}

std::vector<ArchivedChat> ListArchivedChats() {
    std::vector<ArchivedChat> chats;
    std::error_code error;
    for (const fs::directory_entry& item : fs::directory_iterator(fs::path(ArchiveRoot()), error)) {
        if (error || !item.is_directory(error)) {
            continue;
        }
        if (item.path().filename().wstring().ends_with(L".partial")) {
            continue;
        }
        if (const std::optional<ArchivedChat> chat = ReadManifest(item.path())) {
            chats.push_back(*chat);
        }
    }
    std::ranges::sort(chats, [](const ArchivedChat& a, const ArchivedChat& b) { return a.last > b.last; });
    return chats;
}

std::vector<std::string> ChatsNamedInDigest() {
    std::ifstream in(fs::path(ArchiveRoot()) / L"digest.md", std::ios::binary);
    if (!in) {
        return {};
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    constexpr std::string_view kOpen = "```archive";
    const size_t at = text.find(kOpen);
    if (at == std::string::npos) {
        return {};
    }
    const size_t line = text.find('\n', at);
    if (line == std::string::npos) {
        return {};
    }
    const size_t end = text.find("```", line + 1);
    const std::string body = text.substr(line + 1, end == std::string::npos ? std::string::npos : end - (line + 1));
    std::vector<std::string> ids;
    size_t start = 0;
    while (start < body.size()) {
        size_t stop = body.find('\n', start);
        if (stop == std::string::npos) {
            stop = body.size();
        }
        std::string id = body.substr(start, stop - start);
        if (!id.empty() && id.back() == '\r') {
            id.pop_back();
        }
        const size_t first = id.find_first_not_of(" \t");
        const size_t last = id.find_last_not_of(" \t");
        id = first == std::string::npos ? std::string() : id.substr(first, last - first + 1);
        if (SafeId(id)) {
            ids.push_back(std::move(id));
        }
        start = stop + 1;
    }
    return ids;
}

bool PurgeArchivedChat(const std::string& id, bool force) {
    if (!SafeId(id)) {
        return false;
    }
    const std::optional<ArchivedChat> chat = ReadManifest(ArchiveDir(id));
    if (!chat) {
        return false;
    }
    if (!force) {
        const std::vector<std::string> named = ChatsNamedInDigest();
        const auto namedAs = [&](const std::string& one) { return std::ranges::find(named, one) != named.end(); };
        const bool allowed = namedAs(chat->id) || std::ranges::any_of(chat->parts, [&](const SessionPart& part) {
                                  return namedAs(part.id);
                              });
        if (!allowed) {
            return false;
        }
    }
    std::wstring paths = ArchiveDir(id).wstring();
    paths.push_back(L'\0');
    paths.push_back(L'\0');
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = paths.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    std::error_code error;
    return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted && !fs::exists(ArchiveDir(id), error);
}

std::vector<TranscriptEntry> LoadClaudeFile(const fs::path& file, size_t limit, bool& truncated);

std::vector<TranscriptEntry> LoadArchivedPart(const std::string& archiveId, const std::string& partId, bool grok,
                                              size_t limit, bool& truncated) {
    if (!SafeId(archiveId) || !SafeId(partId)) {
        truncated = false;
        return {};
    }
    const fs::path dir = ArchiveDir(archiveId) / L"parts" / ToWide(partId);
    std::error_code error;
    if (grok && fs::is_directory(dir / L"grok", error)) {
        std::vector<TranscriptEntry> entries = LoadGrokTranscript(dir / L"grok");
        KeepLatest(entries, limit, truncated);
        return entries;
    }
    return LoadClaudeFile(dir / L"session.jsonl", limit, truncated);
}

bool MoveSession(const std::wstring& from, const std::wstring& to, const std::string& id) {
    if (id.empty() || id.find_first_of("/\\.:") != std::string::npos || ProjectDir(from) == ProjectDir(to)) {
        return false;
    }
    const fs::path file = SessionFile(from, id);
    const fs::path target = SessionFile(to, id);
    std::error_code error;
    if (!fs::exists(file, error) || fs::exists(target, error)) {
        return false;
    }
    fs::create_directories(ProjectDir(to), error);
    fs::rename(file, target, error);
    if (error) {
        return false;
    }
    // The folder the CLI keeps beside it (tool results and the like); the chat goes on without it if it stays.
    const fs::path folder = ProjectDir(from) / ToWide(id);
    if (fs::is_directory(folder, error) && !fs::exists(ProjectDir(to) / ToWide(id), error)) {
        fs::rename(folder, ProjectDir(to) / ToWide(id), error);
    }
    return true;
}

bool RenameSession(const std::wstring& workspace, const std::string& id, const std::wstring& title) {
    if (id.empty() || id.find_first_of("/\\.:") != std::string::npos || title.empty()) {
        return false;
    }
    {
        const std::lock_guard lock(g_chatsMutex);
        ChatIndex& index = Chats();
        if (ChatRecord* record = RecordOf(index, id); record != nullptr && record->title != title) {
            record->title = title;
            SaveChats(index);
        }
    }
    const fs::path file = SessionFile(workspace, id);
    std::error_code error;
    if (!fs::exists(file, error)) {
        const fs::path grok = GrokSessionDir(workspace, id);
        if (!fs::exists(grok / kGrokHistory, error)) {
            return false;
        }
        const std::string text = ToUtf8(title);
        std::ofstream out(grok / kGrokTitle, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return out.good();
    }
    std::string line = Json{{"type", "custom-title"}, {"customTitle", ToUtf8(title)}, {"sessionId", id}}.dump(
        -1, ' ', false, Json::error_handler_t::replace);
    line += '\n';
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out.write(line.data(), static_cast<std::streamsize>(line.size()));  // one write: the CLI may append too
    return out.good();
}

bool HasOwnTitle(const std::wstring& workspace, const std::string& id) {
    if (id.empty() || id.find_first_of("/\\.:") != std::string::npos) {
        return true;  // nothing to name
    }
    {
        const std::lock_guard lock(g_chatsMutex);
        if (RecordOf(Chats(), id) != nullptr) {
            return true;  // a chat in parts: its record's title or its first part's
        }
    }
    const fs::path file = SessionFile(workspace, id);
    std::error_code error;
    if (fs::exists(file, error)) {
        const uint64_t size = fs::file_size(file, error);
        if (error) {
            return true;
        }
        constexpr uint64_t kChunk = 512 * 1024;
        std::wstring custom;
        std::wstring generated;
        ScanTitles(ReadRange(file, 0, std::min(size, kChunk)), custom, generated);
        if (size > kChunk) {
            ScanTitles(ReadRange(file, size - kChunk, kChunk), custom, generated);
        }
        return !custom.empty() || !generated.empty();
    }
    const fs::path grok = GrokSessionDir(workspace, id);
    return grok.empty() || !fs::exists(grok / kGrokHistory, error) || fs::exists(grok / kGrokTitle, error);
}

bool SetGeneratedTitle(const std::wstring& workspace, const std::string& id, const std::wstring& title) {
    if (id.empty() || id.find_first_of("/\\.:") != std::string::npos || title.empty() ||
        HasOwnTitle(workspace, id)) {
        return false;  // a title given meanwhile stays
    }
    const fs::path file = SessionFile(workspace, id);
    std::error_code error;
    if (!fs::exists(file, error)) {
        const fs::path grok = GrokSessionDir(workspace, id);
        const std::string text = ToUtf8(title);
        std::ofstream out(grok / kGrokTitle, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return out.good();
    }
    // As the CLI's own generated title: a custom-title (a rename) is read above it.
    std::string line = Json{{"type", "ai-title"}, {"aiTitle", ToUtf8(title)}, {"sessionId", id}}.dump(
        -1, ' ', false, Json::error_handler_t::replace);
    line += '\n';
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    return out.good();
}

std::wstring SessionTitle(const std::wstring& workspace, const std::string& id) {
    const fs::path file = SessionFile(workspace, id);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) {
        return CachedReadTitle(file, data);
    }
    const fs::path grok = GrokSessionDir(workspace, id);
    if (grok.empty() || !GetFileAttributesExW((grok / kGrokHistory).c_str(), GetFileExInfoStandard, &data)) {
        return {};
    }
    std::wstring title = GrokOwnTitle(grok);
    return !title.empty() ? title : GrokTitle(grok, data).value_or(std::wstring());
}

std::wstring GrokHistoryFolder(const std::wstring& workspace) {
    return GrokProjectDir(workspace).wstring();
}

// A Claude transcript file, wherever it sits (the live history, or a copy in the archive).
std::vector<TranscriptEntry> LoadClaudeFile(const fs::path& file, size_t limit, bool& truncated) {
    std::vector<TranscriptEntry> entries;
    std::error_code error;
    const uint64_t size = fs::file_size(file, error);
    if (error) {
        KeepLatest(entries, limit, truncated);
        return entries;
    }
    std::string lastAssistant;  // one API message arrives as a line per content block
    ForEachLine(ReadRange(file, 0, size), [&](std::string_view line) {
        const Json entry = Json::parse(line, nullptr, false);
        if (entry.is_discarded() || IsTrue(JsonAt(entry, "isSidechain")) || IsTrue(JsonAt(entry, "isMeta"))) {
            return;
        }
        const std::string type = JsonStr(entry, "type");
        const Json& message = JsonAt(entry, "message");
        if (type == "system" && JsonStr(entry, "subtype") == "compact_boundary") {
            lastAssistant.clear();
            entries.push_back({EntryKind::Compacted});
            return;
        }
        if (type == "user" && IsTrue(JsonAt(entry, "isCompactSummary"))) {
            KeepSummary(entries, TextOf(JsonAt(message, "content"), false));  // the CLI's for itself, not the user's words
            return;
        }
        if (type == "user") {
            lastAssistant.clear();
            const Json& content = JsonAt(message, "content");
            std::wstring text = TextOf(content, false);
            std::vector<TranscriptImage> images;
            if (content.is_array()) {
                for (const Json& block : content) {
                    if (JsonStr(block, "type") == "tool_result") {  // the answer to a call listed above
                        const std::string id = JsonStr(block, "tool_use_id");
                        for (auto call = entries.rbegin(); call != entries.rend(); ++call) {
                            if (call->kind != EntryKind::Tool || call->toolId != id) {
                                continue;
                            }
                            const Json& result = JsonAt(block, "content");
                            std::wstring output;
                            if (result.is_string()) {
                                output = ToWide(result.get<std::string>());
                            } else if (result.is_array()) {
                                for (const Json& part : result) {
                                    if (JsonStr(part, "type") == "text") {
                                        output += (output.empty() ? L"" : L"\n") + ToWide(JsonStr(part, "text"));
                                    }
                                }
                            }
                            const size_t end = output.find_last_not_of(L" \t\r\n");
                            output.resize(end == std::wstring::npos ? 0 : end + 1);
                            call->toolOutput = std::move(output);
                            call->toolFailed = IsTrue(JsonAt(block, "is_error"));
                            call->toolAnswered = true;
                            break;
                        }
                    }
                    const Json& source = JsonAt(block, "source");
                    if (JsonStr(block, "type") == "image" && JsonStr(source, "type") == "base64") {
                        images.push_back({JsonStr(source, "media_type"), JsonStr(source, "data")});
                    }
                }
            }
            // Tool results carry no text; "<command-…>" and the like are the CLI's own bookkeeping, and so is the
            // "[Request interrupted by user…]" it writes after a Stop.
            if ((!text.empty() && text.front() != L'<' && !text.starts_with(L"[Request interrupted by user")) ||
                (text.empty() && !images.empty())) {
                entries.push_back({EntryKind::User, std::move(text), std::move(images)});
            }
        } else if (type == "assistant") {
            const std::string messageId = JsonStr(message, "id");
            const Json& content = JsonAt(message, "content");
            if (!content.is_array()) {
                return;
            }
            for (const Json& block : content) {
                const std::string blockType = JsonStr(block, "type");
                if (blockType == "text") {
                    std::wstring text = ToWide(JsonStr(block, "text"));
                    if (text.empty()) {
                        continue;
                    }
                    if (!entries.empty() && entries.back().kind == EntryKind::Assistant &&
                        lastAssistant == messageId && !messageId.empty()) {
                        entries.back().text += L"\n\n" + text;
                    } else {
                        entries.push_back({EntryKind::Assistant, std::move(text)});
                    }
                    lastAssistant = messageId;
                } else if (blockType == "tool_use") {
                    TranscriptEntry call{EntryKind::Tool, ToWide(JsonStr(block, "name"))};
                    call.toolId = JsonStr(block, "id");
                    call.toolInput = JsonAt(block, "input").dump();
                    entries.push_back(std::move(call));
                    lastAssistant.clear();
                }
            }
        }
    });
    KeepLatest(entries, limit, truncated);
    return entries;
}

std::vector<TranscriptEntry> LoadTranscript(const std::wstring& workspace, const std::string& id, size_t limit,
                                            bool& truncated) {
    std::error_code error;
    const fs::path file = SessionFile(workspace, id);
    if (!fs::is_regular_file(file, error)) {
        std::vector<TranscriptEntry> entries;
        if (const fs::path grok = GrokSessionDir(workspace, id); !grok.empty() && fs::is_directory(grok, error)) {
            entries = LoadGrokTranscript(grok);
        }
        KeepLatest(entries, limit, truncated);
        return entries;
    }
    return LoadClaudeFile(file, limit, truncated);
}

std::wstring ChatSoFar(const std::vector<std::pair<bool, std::vector<TranscriptEntry>>>& parts, size_t longest) {
    std::wstring summary;
    std::wstring said;
    for (const auto& [grok, entries] : parts) {
        for (const TranscriptEntry& entry : entries) {
            if (entry.kind == EntryKind::Compacted && !entry.text.empty()) {
                summary = entry.text;  // all before it is in it
                said.clear();
            } else if (entry.kind == EntryKind::User || entry.kind == EntryKind::Assistant) {
                said += (entry.kind == EntryKind::User ? L"\n\nUser: " : grok ? L"\n\nGrok: " : L"\n\nClaude: ") +
                        entry.text;
            }
        }
    }
    if (said.size() > longest) {
        said = L"\n\n…" + said.substr(said.size() - longest);
    }
    if (summary.empty()) {
        return said;
    }
    return L"\n\nA summary of the chat up to a compaction, as the assistant wrote it for itself:\n\n" + summary +
           (said.empty() ? L"" : L"\n\nWhat was said after it:" + said);
}

std::vector<ProjectPlace> OrderProjects(std::vector<ProjectPlace> projects, const std::wstring& general,
                                        ProjectOrder order) {
    const size_t count = projects.size();
    std::vector<size_t> index(count);
    for (size_t i = 0; i < count; ++i) {
        index[i] = i;
    }
    std::ranges::stable_sort(index, [&](size_t a, size_t b) {
        const bool aGeneral = !general.empty() && projects[a].folder == general;
        const bool bGeneral = !general.empty() && projects[b].folder == general;
        if (aGeneral != bGeneral) {
            return bGeneral;  // the general chats stay a section under the projects
        }
        if (aGeneral) {
            return false;
        }
        if (order == ProjectOrder::Name) {
            const int compared = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, projects[a].name.c_str(), -1,
                                                 projects[b].name.c_str(), -1);
            return compared == CSTR_LESS_THAN;
        }
        if (order == ProjectOrder::Recent && projects[a].activity != projects[b].activity) {
            return projects[a].activity > projects[b].activity;
        }
        return false;  // Added, and any tie: the order they were given
    });
    std::vector<ProjectPlace> ordered;
    ordered.reserve(count);
    for (const size_t i : index) {
        ordered.push_back(std::move(projects[i]));
    }
    return ordered;
}

void OrderChats(std::vector<SessionInfo>& chats, const std::vector<std::string>& ids) {
    if (ids.empty() || chats.size() < 2) {
        return;
    }
    std::map<std::string, size_t> place;
    for (size_t i = 0; i < ids.size(); ++i) {
        place.emplace(ids[i], i);  // the first time an id is named
    }
    std::vector<SessionInfo> fresh;  // not dragged yet: they stay in front, in the order they were given
    std::vector<SessionInfo> kept;
    fresh.reserve(chats.size());
    for (SessionInfo& chat : chats) {
        const std::string id = ChatOrderId(chat);
        if (!id.empty() && place.find(id) != place.end()) {
            kept.push_back(std::move(chat));
        } else {
            fresh.push_back(std::move(chat));
        }
    }
    std::ranges::sort(kept, [&](const SessionInfo& a, const SessionInfo& b) {
        return place.find(ChatOrderId(a))->second < place.find(ChatOrderId(b))->second;
    });
    fresh.insert(fresh.end(), std::make_move_iterator(kept.begin()), std::make_move_iterator(kept.end()));
    chats = std::move(fresh);
}

ProjectOrder ParseProjectOrder(const std::string& text) {
    if (text == "added") {
        return ProjectOrder::Added;
    }
    if (text == "name") {
        return ProjectOrder::Name;
    }
    return ProjectOrder::Recent;
}

const char* ProjectOrderId(ProjectOrder order) {
    switch (order) {
    case ProjectOrder::Added:
        return "added";
    case ProjectOrder::Name:
        return "name";
    case ProjectOrder::Recent:
        return "recent";
    }
    return "recent";
}

Settings LoadSettings() {
    Settings settings;
    std::ifstream in(SettingsFile(), std::ios::binary);
    if (!in) {
        return settings;
    }
    const Json data = Json::parse(in, nullptr, false);
    settings.model = JsonStr(data, "model");
    settings.effort = JsonStr(data, "effort");
    settings.permissions = JsonStr(data, "permissions");
    settings.grokPermissions = JsonStr(data, "grokPermissions");
    settings.claudeModels = JsonStr(data, "claudeModels");
    settings.claudeModel = JsonStr(data, "claudeModel");
    settings.connectors = JsonAt(data, "connectors").is_boolean() && JsonAt(data, "connectors").get<bool>();
    settings.grokFast = JsonAt(data, "grokFast").is_boolean() && JsonAt(data, "grokFast").get<bool>();
    settings.connectLater = JsonAt(data, "connectLater").is_boolean() && JsonAt(data, "connectLater").get<bool>();
    const auto number = [&](const char* key, long long fallback) {
        const Json& value = JsonAt(data, key);
        return value.is_number() ? value.get<long long>() : fallback;
    };
    settings.sessionPercent = static_cast<int>(number("sessionPercent", -1));
    settings.sessionResets = number("sessionResets", 0);
    settings.weekPercent = static_cast<int>(number("weekPercent", -1));
    settings.weekResets = number("weekResets", 0);
    const auto real = [&](const char* key) {
        const Json& value = JsonAt(data, key);
        return value.is_number() ? value.get<float>() : 0.f;
    };
    settings.sidebarWidth = real("sidebarWidth");
    settings.terminalWidth = real("terminalWidth");
    settings.workspace = ToWide(JsonStr(data, "workspace"));
    settings.reopen = JsonStr(data, "reopen");
    const auto folders = [&](const char* key) {
        std::vector<std::wstring> list;
        if (const Json& value = JsonAt(data, key); value.is_array()) {
            for (const Json& folder : value) {
                if (folder.is_string()) {
                    list.push_back(ToWide(folder.get<std::string>()));
                }
            }
        }
        return list;
    };
    // Before projects there were the recent folders (newest first): they become the projects, oldest first.
    settings.projects = JsonAt(data, "projects").is_array() ? folders("projects") : folders("recentFolders");
    if (!JsonAt(data, "projects").is_array()) {
        std::ranges::reverse(settings.projects);
    }
    settings.closedProjects = folders("closedProjects");
    settings.projectOrder = ParseProjectOrder(JsonStr(data, "projectOrder"));
    if (const Json& orders = JsonAt(data, "chatOrder"); orders.is_array()) {
        for (const Json& item : orders) {
            if (!item.is_object()) {
                continue;
            }
            ChatOrder order;
            order.folder = ToWide(JsonStr(item, "folder"));
            if (const Json& ids = JsonAt(item, "ids"); ids.is_array()) {
                for (const Json& id : ids) {
                    if (id.is_string() && !id.get<std::string>().empty()) {
                        order.ids.push_back(id.get<std::string>());
                    }
                }
            }
            if (!order.folder.empty() && !order.ids.empty()) {
                settings.chatOrder.push_back(std::move(order));
            }
        }
    }
    return settings;
}

void SetSettingsReadOnly(bool readOnly) {
    g_settingsReadOnly = readOnly;
}

void SaveSettings(const Settings& settings) {
    if (g_settingsReadOnly) {
        return;
    }
    const fs::path file = SettingsFile();
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    Json chatOrder = Json::array();
    for (const ChatOrder& order : settings.chatOrder) {
        Json ids = Json::array();
        for (const std::string& id : order.ids) {
            if (!id.empty()) {
                ids.push_back(id);
            }
        }
        if (!order.folder.empty() && !ids.empty()) {
            chatOrder.push_back(Json{{"folder", ToUtf8(order.folder)}, {"ids", std::move(ids)}});
        }
    }
    const auto folders = [](const std::vector<std::wstring>& list) {
        Json array = Json::array();
        for (const std::wstring& folder : list) {
            array.push_back(ToUtf8(folder));
        }
        return array;
    };
    out << Json{{"model", settings.model},
                {"effort", settings.effort},
                {"permissions", settings.permissions},
                {"grokPermissions", settings.grokPermissions},
                {"claudeModels", settings.claudeModels},
                {"claudeModel", settings.claudeModel},
                {"connectors", settings.connectors},
                {"grokFast", settings.grokFast},
                {"connectLater", settings.connectLater},
                {"sessionPercent", settings.sessionPercent},
                {"sessionResets", settings.sessionResets},
                {"weekPercent", settings.weekPercent},
                {"weekResets", settings.weekResets},
                {"sidebarWidth", settings.sidebarWidth},
                {"terminalWidth", settings.terminalWidth},
                {"workspace", ToUtf8(settings.workspace)},
                {"projects", folders(settings.projects)},
                {"closedProjects", folders(settings.closedProjects)},
                {"projectOrder", ProjectOrderId(settings.projectOrder)},
                {"chatOrder", std::move(chatOrder)},
                {"reopen", settings.reopen}}
               .dump(2)
        << "\n";
}

}  // namespace aic
