// Chats moved between projects (their history folders under ~\.claude\projects, made for two scratch
// workspaces and removed after). Exit code 0: all passed.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "core/ask.h"
#include "core/bookmarks.h"
#include "core/chat_export.h"
#include "core/journal.h"
#include "core/store.h"
#include "core/title_writer.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path temp = fs::temp_directory_path();
    const std::wstring a = (temp / L"aic_store_test_a").wstring();
    const std::wstring b = (temp / L"aic_store_test_b").wstring();
    // Where the CLI keeps their chats: the memory folder's parent.
    const fs::path historyA = fs::path(aic::MemoryFolder(a)).parent_path();
    const fs::path historyB = fs::path(aic::MemoryFolder(b)).parent_path();
    std::error_code error;
    fs::remove_all(historyA, error);
    fs::remove_all(historyB, error);
    const fs::path chatIndex = temp / L"aic_store_test_chats.json";  // not the user's chats.json
    fs::remove(chatIndex, error);
    aic::SetChatIndexFile(chatIndex.wstring());
    fs::create_directories(historyA / L"chat-1", error);
    std::ofstream(historyA / L"chat-1.jsonl") << R"({"type":"user","message":{"role":"user","content":"hello"}})" << "\n";
    std::ofstream(historyA / L"chat-1" / L"result.txt") << "x";

    Expect(aic::ListSessions(a).size() == 1 && aic::ListSessions(b).empty(), "the chat is in the first project");
    Expect(aic::MoveSession(a, b, "chat-1"), "moved");
    Expect(aic::ListSessions(a).empty() && aic::ListSessions(b).size() == 1 && aic::ListSessions(b)[0].id == "chat-1",
           "now it is in the second");
    Expect(fs::exists(historyB / L"chat-1" / L"result.txt") && !fs::exists(historyA / L"chat-1"),
           "its folder went with it");
    Expect(aic::ListSessions(b)[0].title == L"hello", "its title as before");
    {
        std::string sentence = "word0";
        for (int i = 1; i < 16; ++i) {
            sentence += " word" + std::to_string(i);
        }
        std::ofstream(historyA / L"chat-long.jsonl")
            << "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"" << sentence << "\"}}\n";
        const std::wstring whole(sentence.begin(), sentence.end());
        const auto listed = aic::ListSessions(a);
        const auto found = std::ranges::find_if(listed, [](const aic::SessionInfo& s) { return s.id == "chat-long"; });
        Expect(found != listed.end() && whole.size() > 80 && found->title.size() <= 81 &&
                   found->title.back() == 0x2026 && whole.starts_with(found->title.substr(0, found->title.size() - 1)) &&
                   found->title[found->title.size() - 2] != L' ',
               "a long first line is cut on a word, one ellipsis");
        std::ofstream(historyA / L"chat-sentence.jsonl")
            << "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"Fix the sidebar scrolling. It jumps "
               "when a chat is added and the list is long.\"}}\n";
        const auto again = aic::ListSessions(a);
        const auto sentence1 = std::ranges::find_if(again, [](const aic::SessionInfo& s) { return s.id == "chat-sentence"; });
        Expect(sentence1 != again.end() && sentence1->title == L"Fix the sidebar scrolling",
               "the first line's first sentence, without its full stop");
        Expect(!aic::HasOwnTitle(a, "chat-sentence") &&
                   aic::SetGeneratedTitle(a, "chat-sentence", L"Прокрутка списка чатов") &&
                   aic::SessionTitle(a, "chat-sentence") == L"Прокрутка списка чатов" &&
                   aic::HasOwnTitle(a, "chat-sentence") &&
                   !aic::SetGeneratedTitle(a, "chat-sentence", L"Другое"),
               "a generated title is kept once and read");
        Expect(aic::RenameSession(a, "chat-sentence", L"Моё") && aic::SessionTitle(a, "chat-sentence") == L"Моё",
               "a rename is above the generated title");
    }
    Expect(aic::CleanTitle("«Прокрутка списка чатов».\n\nПояснение") == L"Прокрутка списка чатов" &&
               aic::CleanTitle("Название: Сборка установщика") == L"Сборка установщика" &&
               aic::CleanTitle("").empty() &&
               aic::CleanTitle(std::string(200, 'a')).size() == aic::kTitleLongest + 1,
           "the model's answer as a title");
    {  // the failures journal, in a scratch folder (not the user's)
        const fs::path folder = temp / L"aic_journal_test";
        fs::remove_all(folder, error);
        aic::SetJournalFolder(folder.wstring());
        aic::JournalWrite("чат", L"Первый сбой");
        aic::JournalWrite("браузер", L"Второй");
        const auto entries = aic::ReadJournal();
        Expect(entries.size() == 2 && entries.front().source == "чат" && entries.back().text == L"Второй",
               "the journal reads back, in the order written");
        Expect(aic::JournalUnseen() == 2, "both are not seen");
        aic::MarkJournalSeen();
        Expect(aic::JournalUnseen() == 0, "seen after a look");
        Expect(aic::WriteJournalPage() && fs::exists(aic::JournalPage()) &&
                   ReadAll(aic::JournalPage()).find("Первый сбой") != std::string::npos,
               "its page has the entries");
        const std::wstring report =
            L"EventType=BEX64\r\nSig[0].Value=aicontrols.exe\r\nSig[1].Value=0.1.98.0\r\nSig[2].Value=6ae8eed4\r\n"
            L"Sig[3].Value=aicontrols.exe\r\nSig[4].Value=0.1.98.0\r\nSig[5].Value=6ae8eed4\r\n"
            L"Sig[6].Value=00000000001a822d\r\nSig[7].Value=c0000409\r\nSig[8].Value=0000000000000007\r\n"
            L"AppPath=C:\\Users\\user\\AppData\\Local\\Programs\\AIControls\\aicontrols.exe\r\n";
        const std::wstring summary = aic::CrashSummary(report);
        Expect(summary.find(L"c0000409") != std::wstring::npos && summary.find(L"0x1a822d") != std::wstring::npos,
               "a crash report is summed up");
        Expect(aic::CrashSummary(L"EventType=WindowsUpdateFailure\r\n").empty(), "not a crash: nothing");
        fs::remove_all(folder, error);
    }
    {  // the browser's bookmarks, in a scratch folder
        const fs::path folder = temp / L"aic_bookmarks_test";
        fs::remove_all(folder, error);
        aic::SetBookmarksFolder(folder.wstring());
        auto marks = aic::LoadBookmarks();
        Expect(marks.size() == aic::DefaultBookmarks().size() && marks.front().name == L"ChatGPT",
               "no file: the cloud AIs to start with");
        marks.erase(marks.begin());
        marks.push_back({L"Мой сайт", L"https://example.org/a"});
        Expect(aic::SaveBookmarks(marks), "bookmarks saved");
        const auto back = aic::LoadBookmarks();
        Expect(back.size() == marks.size() && back.back().name == L"Мой сайт" && back.front().name == L"Claude",
               "bookmarks read back in their order");
        Expect(aic::SaveBookmarks({}) && aic::LoadBookmarks().empty(), "an emptied list stays empty");
        std::vector<aic::Bookmark> some{{L"Qwen", L"https://chat.qwen.ai/"}};
        Expect(aic::RestoreDefaultBookmarks(some) == aic::DefaultBookmarks().size() - 1 && some.front().name == L"Qwen",
               "the missing defaults come back after the ones kept");
        Expect(aic::BookmarkHost(L"https://WWW.Perplexity.ai/search?q=1") == L"perplexity.ai" &&
                   aic::BookmarkHost(L"http://user@chat.qwen.ai:8080/c/1") == L"chat.qwen.ai" &&
                   aic::BookmarkHost(L"about:blank").empty(),
               "a bookmark's host");
        Expect(aic::SameAddress(L"HTTPS://Chat.Qwen.ai/", L"https://chat.qwen.ai#top") &&
                   !aic::SameAddress(L"https://chat.qwen.ai/c/1", L"https://chat.qwen.ai/c/2"),
               "one address written two ways");
        Expect(aic::BookmarkName(L"Qwen Chat - Alibaba Cloud", L"https://chat.qwen.ai/") == L"Qwen Chat" &&
                   aic::BookmarkName(L"", L"https://www.example.org/x") == L"example.org" &&
                   aic::BookmarkName(L"Очень длинное название страницы про всё на свете", L"https://a.b/").size() <= 25,
               "a page's short name");
        fs::remove_all(folder, error);
    }
    Expect(!aic::MoveSession(a, b, "chat-1"), "a chat that is not there is not moved");
    Expect(!aic::MoveSession(b, b, "chat-1"), "nor to its own project");
    Expect(!aic::MoveSession(b, a, "..\\chat-1"), "nor by a path");

    std::ofstream(historyA / L"chat-1.jsonl") << "{}\n";  // the same id waiting there: nothing is overwritten
    Expect(!aic::MoveSession(b, a, "chat-1") && fs::exists(historyB / L"chat-1.jsonl"), "no chat is overwritten");

    // A compacted chat: a mark where it was summed up, and the CLI's summary is not shown as the user's words.
    std::ofstream(historyA / L"chat-2.jsonl")
        << R"({"type":"user","message":{"role":"user","content":"remember pelican"}})" << "\n"
        << R"({"type":"system","subtype":"compact_boundary","compact_metadata":{"pre_tokens":3972,"post_tokens":743}})" << "\n"
        << R"({"type":"user","isCompactSummary":true,"message":{"role":"user","content":"This session is being continued )"
        << R"(from a previous conversation.\n\n<system-reminder>x</system-reminder>\nSummary:\n1. the word is pelican\n\n)"
        << R"(Continue the conversation from where it left off without asking."}})" << "\n"
        << R"({"type":"user","message":{"role":"user","content":"which word?"}})" << "\n";
    bool truncated = false;
    const std::vector<aic::TranscriptEntry> entries = aic::LoadTranscript(a, "chat-2", 100, truncated);
    Expect(entries.size() == 3 && entries[0].kind == aic::EntryKind::User && entries[1].kind == aic::EntryKind::Compacted &&
               entries[2].kind == aic::EntryKind::User && entries[2].text == L"which word?",
           "a compacted chat: the mark, no summary");
    Expect(entries.size() == 3 && entries[1].text == L"Summary:\n1. the word is pelican",
           "the summary kept on the mark, without the CLI's words to itself");
    // The CLI's notes after a Stop are bookkeeping, not the user's words.
    std::ofstream(historyA / L"chat-3.jsonl")
        << R"({"type":"user","message":{"role":"user","content":"go"}})" << "\n"
        << R"({"type":"user","message":{"role":"user","content":[{"type":"text","text":"[Request interrupted by user]"}]}})" << "\n"
        << R"({"type":"user","message":{"role":"user","content":"again"}})" << "\n";
    const std::vector<aic::TranscriptEntry> stopped = aic::LoadTranscript(a, "chat-3", 100, truncated);
    Expect(stopped.size() == 2 && stopped[1].text == L"again", "the CLI's note after a Stop is not a user's message");
    // A switch after it: the summary and what came after, not what it sums up; each part's words by its CLI.
    const std::wstring handoff = aic::ChatSoFar({{false, entries}, {true, {{aic::EntryKind::Assistant, L"pelican"}}}}, 100);
    Expect(handoff.find(L"remember pelican") == std::wstring::npos && handoff.find(L"the word is pelican") != std::wstring::npos &&
               handoff.ends_with(L"User: which word?\n\nGrok: pelican"),
           "the chat so far: the summary, then the words after it");
    Expect(aic::ChatSoFar({{false, {{aic::EntryKind::User, L"0123456789"}}}}, 5) == L"\n\n…56789",
           "without a summary: the latest words");

    // Grok's chats: a folder each under ~\.grok\sessions\<workspace, %-encoded>, listed beside Claude's.
    const fs::path grokA = aic::GrokHistoryFolder(a);
    Expect(grokA.filename().wstring().ends_with(L"%5Caic_store_test_a"), "grok's folder name: the path %-encoded");
    fs::remove_all(grokA, error);
    fs::create_directories(grokA / L"g-1", error);
    fs::create_directories(grokA / L"g-empty", error);
    std::ofstream(grokA / L"g-1" / L"chat_history.jsonl")
        << R"({"type":"user","content":[{"type":"text","text":"<rules>be brief</rules>"}]})" << "\n"
        << R"({"type":"user","content":[{"type":"text","text":"<image_files>\n1.png\n</image_files>\n\n<user_query>\n)"
        << R"(сделай навигацию\nи логику\n</user_query>"},{"type":"image","url":"data:image/png;base64,iVBO"}],"prompt_index":0})" << "\n"
        << R"({"type":"reasoning","content":"thinking"})" << "\n"
        << R"({"type":"assistant","content":"","tool_calls":[{"id":"c1","name":"search_tool","arguments":"{}"},)"
        << R"({"id":"c2","name":"use_tool","arguments":"{\"tool_name\":\"aic__read_file\",\"tool_input\":{\"path\":\"a.txt\"}}"}]})" << "\n"
        << R"({"type":"tool_result","tool_call_id":"c2","content":"hello\n"})" << "\n"
        << R"({"type":"user","content":[{"type":"text","text":"<system-reminder>x</system-reminder>"}],"synthetic_reason":"r"})" << "\n"
        << R"({"type":"assistant","content":"done"})" << "\n";
    fs::create_directories(grokA / L"g-2", error);
    std::ofstream(grokA / L"g-2" / L"chat_history.jsonl")
        << R"({"type":"user","content":[{"type":"text","text":"<user_query>\nнарисуй рыбу\n</user_query>"}],"prompt_index":0})" << "\n"
        << R"({"type":"backend_tool_call","kind":{"tool_type":"web_search","action":{"type":"search","query":"fish"},"status":"completed"}})" << "\n"
        << R"({"type":"assistant","content":"","tool_calls":[{"id":"i1","name":"image_gen","arguments":"{\"prompt\":\"a fish\"}"},)"
        << R"({"id":"i2","name":"image_to_video","arguments":"{\"prompt\":\"swims\"}"}]})" << "\n"
        << R"({"type":"tool_result","tool_call_id":"i1","content":"{\"path\":\"C:\\\\1.jpg\"}"})" << "\n"
        << R"({"type":"tool_result","tool_call_id":"i2","content":"Tool `image_to_video` failed: no video"})" << "\n"
        << R"({"type":"assistant","content":"","tool_calls":[{"id":"r1","name":"read_file","arguments":"{\"target_file\":\"C:\\\\a.cpp\"}"},{"id":"g1","name":"grep","arguments":"{\"pattern\":\"main\",\"path\":\"src\",\"glob\":\"*.cpp\"}"}]})" << "\n";
    std::ofstream(grokA / L"g-empty" / L"chat_history.jsonl")
        << R"({"type":"user","content":[{"type":"text","text":"<rules>be brief</rules>"}]})" << "\n";
    // Compacted twice (grok 1.0.62): the turns before each compaction in a segment, the history left with Grok's rules
    // (compaction_meta), the prompt the compaction fell into, without its prompt_index, and what came after.
    fs::create_directories(grokA / L"g-3" / L"compaction", error);
    std::ofstream(grokA / L"g-3" / L"compaction" / L"segment_000.md") << R"seg(# HISTORICAL -- DO NOT EDIT

## Summary (curated by compaction step)
<user_query>
not a turn
</user_query>

## Verbatim turns

### Turn 0 (System)
You are Grok.

### Turn 1 (Human)
<user_info>
OS Version: windows
</user_info>

### Turn 2 (Human)
<user_query>
почему пропал чат
</user_query>

### Turn 3 (Assistant)
Сейчас посмотрю.
[tool_request: use_tool]
- tool_name: aic__read_file
- tool_input: {"path":"a.txt"}

### Turn 4 (Function)
[tool_response]
hello

### Turn 5 (Human)
<image_files>
1.png
</image_files>

<user_query>
вот снимок
</user_query>
[image]
)seg";
    std::ofstream(grokA / L"g-3" / L"compaction" / L"segment_001.md") << R"seg(## Verbatim turns

### Turn 0 (System)
You are Grok.

### Turn 1 (Human)
<user_query>
вот снимок
</user_query>
[image]

### Turn 2 (Human)
This session is being continued

### Turn 3 (Assistant)
Нашёл.

### Turn 4 (Human)
<user_query>
чини
</user_query>
)seg";
    std::ofstream(grokA / L"g-3" / L"chat_history.jsonl")
        << R"({"type":"system","content":"You are Grok."})" << "\n"
        << R"({"type":"user","content":[{"type":"text","text":"<user_info>x</user_info>"}],"synthetic_reason":"compaction_meta"})" << "\n"
        << R"({"type":"user","content":[{"type":"text","text":"<user_query>\nчини\n</user_query>"}]})" << "\n"
        << R"({"type":"user","content":[{"type":"text","text":"This session is being continued"}],"synthetic_reason":"compaction_meta"})" << "\n"
        << R"({"type":"assistant","content":"починил"})" << "\n";
    fs::create_directories(grokA / L"g-4", error);  // compacted, its segments gone
    std::ofstream(grokA / L"g-4" / L"chat_history.jsonl")
        << R"({"type":"user","content":[{"type":"text","text":"<user_info>x</user_info>"}],"synthetic_reason":"compaction_meta"})" << "\n"
        << R"({"type":"user","content":[{"type":"text","text":"<user_query>\nдальше\n</user_query>"}]})" << "\n"
        << R"({"type":"assistant","content":"ок"})" << "\n";
    std::vector<aic::SessionInfo> listed = aic::ListSessions(a);
    const auto compacted = std::ranges::find(listed, std::string("g-3"), &aic::SessionInfo::id);
    Expect(compacted != listed.end() && compacted->grok && compacted->title == L"почему пропал чат",
           "a compacted grok chat: listed, named after its first message in the first segment");
    const auto lost = std::ranges::find(listed, std::string("g-4"), &aic::SessionInfo::id);
    Expect(lost != listed.end() && lost->title == L"дальше", "a compacted grok chat without segments: the prompt it kept");
    const std::vector<aic::TranscriptEntry> whole = aic::LoadTranscript(a, "g-3", 100, truncated);
    const std::vector<aic::EntryKind> kinds = {aic::EntryKind::User,      aic::EntryKind::Assistant, aic::EntryKind::User,
                                               aic::EntryKind::Compacted, aic::EntryKind::Assistant, aic::EntryKind::User,
                                               aic::EntryKind::Compacted, aic::EntryKind::Assistant};
    Expect(std::ranges::equal(whole, kinds, {}, &aic::TranscriptEntry::kind) && whole[0].text == L"почему пропал чат" &&
               whole[1].text == L"Сейчас посмотрю." && whole[2].text == L"вот снимок\n\n📎 фото" &&
               whole[4].text == L"Нашёл." && whole[5].text == L"чини" && whole[7].text == L"починил",
           "a compacted grok transcript: the segments' words, a mark at each compaction, its prompt once");
    Expect(whole.size() == 8 && whole[3].text.empty() && whole[6].text == L"This session is being continued",
           "grok's summary on the newest mark; its rules are not one");
    const std::vector<aic::TranscriptEntry> kept = aic::LoadTranscript(a, "g-4", 100, truncated);
    Expect(kept.size() == 3 && kept[0].kind == aic::EntryKind::Compacted && kept[1].text == L"дальше" && kept[2].text == L"ок",
           "a compacted grok transcript without segments: the mark, then what is left");
    const auto grokChat = std::ranges::find(listed, std::string("g-1"), &aic::SessionInfo::id);
    Expect(grokChat != listed.end() && grokChat->grok && grokChat->title == L"сделай навигацию",
           "a grok chat: listed, named after its first message");
    Expect(std::ranges::find(listed, std::string("g-empty"), &aic::SessionInfo::id) == listed.end(),
           "a grok session nothing was said in is not listed");
    const std::vector<aic::TranscriptEntry> said = aic::LoadTranscript(a, "g-1", 100, truncated);
    Expect(said.size() == 3 && said[0].kind == aic::EntryKind::User && said[0].text == L"сделай навигацию\nи логику" &&
               said[0].images.size() == 1 && said[0].images[0].mediaType == "image/png",
           "a grok transcript: the words as typed, the picture");
    Expect(said.size() == 3 && said[1].kind == aic::EntryKind::Tool && said[1].text == L"mcp__aic__read_file" &&
               said[1].toolInput == R"({"path":"a.txt"})" && said[1].toolOutput == L"hello" && said[1].toolAnswered,
           "a grok tool call as it comes live, with its answer; its tool lookups left out");
    Expect(said.size() == 3 && said[2].kind == aic::EntryKind::Assistant && said[2].text == L"done", "the answer");
    const std::vector<aic::TranscriptEntry> made = aic::LoadTranscript(a, "g-2", 100, truncated);
    Expect(made.size() == 6 && made[1].text == L"grok__web_search" && made[1].toolInput.find("\"fish\"") != std::string::npos &&
               made[2].text == L"grok__image_gen" && made[2].toolOutput == LR"({"path":"C:\\1.jpg"})" && !made[2].toolFailed &&
               made[3].text == L"grok__image_to_video" && made[3].toolFailed && made[4].text == L"read_file" &&
               made[4].toolInput.find("a.cpp") != std::string::npos && made[5].text == L"grep" &&
               made[5].toolInput.find("main") != std::string::npos,
           "grok's own tools in its history: the search, the picture, the video that failed, a native read and grep");
    Expect(aic::RenameSession(a, "g-1", L"Навигация") && aic::SessionTitle(a, "g-1") == L"Навигация" &&
               fs::exists(grokA / L"g-1" / L"chat_history.jsonl"),
           "a grok chat renamed, its history left as it is");
    Expect(!aic::RenameSession(a, "..\\g-1", L"x") && !aic::MoveSession(a, b, "g-1"), "no paths; grok's are not moved");
    const std::optional<std::string> markdown = aic::ChatMarkdown(a, "g-1");
    Expect(markdown && markdown->starts_with("# Навигация\n") && markdown->find("## Grok") != std::string::npos &&
               markdown->find("Прочитать файл") != std::string::npos && markdown->find("```\nhello\n```") != std::string::npos,
           "a chat as Markdown: its title, who said what, the tool's card and output");
    Expect(!aic::ChatMarkdown(a, "nope"), "no Markdown for a chat that is not there");

    // A chat in parts: a switch from Claude to Grok goes on in a session of Grok's, chats.json makes them one.
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    const auto chatOf = [](const std::vector<aic::SessionInfo>& list, const std::string& id) {
        return std::ranges::find_if(list, [&](const aic::SessionInfo& chat) { return aic::SameChat(chat, id); });
    };
    aic::LinkChats("chat-2", "g-new");  // nothing said in Grok's session yet
    listed = aic::ListSessions(a);
    auto chain = chatOf(listed, "g-new");
    Expect(chain != listed.end() && chain->id == "chat-2" && !chain->grok && chain->parts.size() == 1 &&
               chain->linked == std::vector<std::string>{"chat-2", "g-new"},
           "switched before anything was said: the chat as it was, its new session known");
    aic::LinkChats("chat-2", "g-2");  // switched again, and said: the empty session is dropped
    aic::NoteChatActive("g-2", 5);
    listed = aic::ListSessions(a);
    chain = chatOf(listed, "chat-2");
    Expect(chain != listed.end() && chain->id == "g-2" && chain->grok && chain->parts.size() == 2 &&
               chain->parts[0].id == "chat-2" && !chain->parts[0].grok && chain->parts[1].grok &&
               chain->linked == std::vector<std::string>{"chat-2", "g-2"} && chain->title == L"remember pelican" &&
               ticks(chain->modified) == 5,
           "one row: Grok's session goes on, the first part's title, the time of the last message");
    Expect(std::ranges::count_if(listed, [&](const aic::SessionInfo& chat) { return aic::SameChat(chat, "g-2"); }) == 1,
           "and no row of its own for either part");
    Expect(aic::RenameSession(a, "g-2", L"Пеликан") && chatOf(aic::ListSessions(a), "g-2")->title == L"Пеликан",
           "a chat in parts renamed");
    const std::optional<std::string> both = aic::ChatMarkdown(a, "chat-2");
    Expect(both && both->find("remember pelican") != std::string::npos && both->find("Дальше отвечал Grok") != std::string::npos &&
               both->find("нарисуй рыбу") != std::string::npos && both->find("Claude и Grok") != std::string::npos,
           "a chat in parts as Markdown: both, one after the other");
    aic::NoteChatOpened("g-1", 7);
    aic::NoteChatOpened("g-1", 9);  // grok.exe rewrote its files on opening: the time as it was listed stays
    Expect(ticks(chatOf(aic::ListSessions(a), "g-1")->modified) == 7, "an opened chat keeps its place");
    aic::NoteChatActive("g-1", 11);
    Expect(ticks(chatOf(aic::ListSessions(a), "g-1")->modified) == 11, "a message moves it up");
    aic::SetChatIndexFile(chatIndex.wstring());  // read back from the file
    listed = aic::ListSessions(a);
    Expect(chatOf(listed, "g-2") != listed.end() && chatOf(listed, "g-2")->parts.size() == 2 &&
               ticks(chatOf(listed, "g-1")->modified) == 11,
           "chats.json read back");
    fs::remove(chatIndex, error);
    fs::remove_all(grokA, error);

    // The sidebar's order: the general chats always last; by recent, the open project is not pulled up.
    const std::wstring general = L"general";
    const std::vector<aic::ProjectPlace> places = {
        {L"sov", L"Sovereign", 10},
        {general, L"Obshchee", 100},
        {L"dpi", L"dpiprobe", 50},
        {L"aic", L"aicontrols", 50},
        {L"old", L"Windows", 0},
    };
    const auto foldersOf = [](const std::vector<aic::ProjectPlace>& list) {
        std::vector<std::wstring> names;
        for (const aic::ProjectPlace& place : list) {
            names.push_back(place.folder);
        }
        return names;
    };
    Expect(foldersOf(aic::OrderProjects(places, general, aic::ProjectOrder::Recent)) ==
               std::vector<std::wstring>{L"dpi", L"aic", L"sov", L"old", general},
           "recent: the newest first, ties keep the added order, general last");
    Expect(foldersOf(aic::OrderProjects(places, general, aic::ProjectOrder::Name)) ==
               std::vector<std::wstring>{L"aic", L"dpi", L"sov", L"old", general},
           "name: A to Z, ignoring case, general still last");
    Expect(foldersOf(aic::OrderProjects(places, general, aic::ProjectOrder::Added)) ==
               std::vector<std::wstring>{L"sov", L"dpi", L"aic", L"old", general},
           "added: the given order, general pulled out to the end");
    Expect(aic::ParseProjectOrder("") == aic::ProjectOrder::Recent &&
               aic::ParseProjectOrder("nope") == aic::ProjectOrder::Recent &&
               aic::ParseProjectOrder("added") == aic::ProjectOrder::Added &&
               aic::ParseProjectOrder("name") == aic::ProjectOrder::Name,
           "an unknown order is recent");
    Expect(std::string(aic::ProjectOrderId(aic::ProjectOrder::Added)) == "added" &&
               std::string(aic::ProjectOrderId(aic::ProjectOrder::Name)) == "name",
           "the order is stored by those names");

    // A dragged chat list: unnamed stay in front (a new chat), named follow the saved order. A chat that went on
    // in another session is named by its first part.
    const auto idsOf = [](const std::vector<aic::SessionInfo>& list) {
        std::vector<std::string> ids;
        for (const aic::SessionInfo& chat : list) {
            ids.push_back(chat.id);
        }
        return ids;
    };
    aic::SessionInfo ended;
    ended.id = "part-2";
    ended.linked = {"part-1", "part-2"};
    std::vector<aic::SessionInfo> dragged;
    for (const char* id : {"c", "b", "a"}) {
        aic::SessionInfo chat;
        chat.id = id;
        chat.linked = {id};
        dragged.push_back(std::move(chat));
    }
    dragged.push_back(ended);
    aic::OrderChats(dragged, {});
    Expect(idsOf(dragged) == std::vector<std::string>{"c", "b", "a", "part-2"}, "no saved order leaves the list");
    aic::OrderChats(dragged, {"a", "part-1"});
    Expect(idsOf(dragged) == std::vector<std::string>{"c", "b", "a", "part-2"},
           "unnamed stay in front, then the saved order; a chat in parts by its first id");
    aic::OrderChats(dragged, {"part-1", "missing", "b", "a", "c"});
    Expect(idsOf(dragged) == std::vector<std::string>{"part-2", "b", "a", "c"},
           "a full order, and an id that is gone is skipped");

    fs::remove_all(historyA, error);
    fs::remove_all(historyB, error);

    // A chat of two parts is copied whole. A copy that stops halfway leaves the live chat and no archive
    // folder. Putting it back writes the same files into the history and the line into chats.json.
    const std::wstring arc = (temp / L"aic_store_archive").wstring();
    const fs::path historyArc = fs::path(aic::MemoryFolder(arc)).parent_path();
    const fs::path grokArc = aic::GrokHistoryFolder(arc);
    const fs::path archiveRoot = temp / L"aic_store_archive_root";
    const fs::path archiveIndex = temp / L"aic_store_archive_chats.json";
    fs::remove_all(historyArc, error);
    fs::remove_all(grokArc, error);
    fs::remove_all(archiveRoot, error);
    fs::remove(archiveIndex, error);
    aic::SetChatIndexFile(archiveIndex.wstring());
    aic::SetArchiveRoot(archiveRoot.wstring());
    aic::SetArchiveFailAfter(-1);
    fs::create_directories(historyArc / L"part-c", error);
    std::ofstream(historyArc / L"part-c.jsonl") << R"({"type":"user","message":{"role":"user","content":"archive claude"}})"
                                               << "\n";
    std::ofstream(historyArc / L"part-c" / L"note.txt") << "side-note";
    fs::create_directories(grokArc / L"part-g", error);
    std::ofstream(grokArc / L"part-g" / L"chat_history.jsonl")
        << R"({"type":"user","content":[{"type":"text","text":"<user_query>\narchive grok\n</user_query>"}],"prompt_index":0})"
        << "\n";
    std::ofstream(grokArc / L"part-g" / L"extra.bin") << "grok-extra";
    aic::LinkChats("part-c", "part-g");
    aic::RenameSession(arc, "part-g", L"Архивный");
    const auto arcList = aic::ListSessions(arc);
    const auto arcChat = std::ranges::find_if(arcList, [](const aic::SessionInfo& s) { return aic::SameChat(s, "part-g"); });
    Expect(arcChat != arcList.end() && arcChat->parts.size() == 2 && arcChat->id == "part-g",
           "archive fixture: one chat, two parts");
    if (arcChat != arcList.end()) {
        aic::SetArchiveFailAfter(1);
        Expect(!aic::ArchiveChat(arc, *arcChat), "a copy that stops after one part fails");
        Expect(fs::exists(historyArc / L"part-c.jsonl") && fs::exists(historyArc / L"part-c" / L"note.txt") &&
                   fs::exists(grokArc / L"part-g" / L"extra.bin"),
               "the live chat is untouched when the copy does not finish");
        Expect(!fs::exists(archiveRoot / L"part-g") && !fs::exists(archiveRoot / L"part-g.partial"),
               "an unfinished copy is not left behind");
        aic::SetArchiveFailAfter(-1);
        Expect(aic::ArchiveChat(arc, *arcChat), "both parts copy");
        const fs::path copied = archiveRoot / L"part-g";
        Expect(ReadAll(copied / L"parts" / L"part-c" / L"session.jsonl").find("archive claude") != std::string::npos &&
                   ReadAll(copied / L"parts" / L"part-c" / L"side" / L"note.txt") == "side-note" &&
                   ReadAll(copied / L"parts" / L"part-g" / L"grok" / L"extra.bin") == "grok-extra" &&
                   ReadAll(copied / L"parts" / L"part-g" / L"grok" / L"chat_history.jsonl").find("archive grok") != std::string::npos,
               "the copy has both parts, whole");
        Expect(fs::exists(historyArc / L"part-c.jsonl") && fs::exists(grokArc / L"part-g" / L"extra.bin"),
               "archiving does not remove the live chat");
        {  // on a volume that can pack files, the copy is packed (the live chat is not touched)
            DWORD flags = 0;
            const std::wstring volume = fs::absolute(copied).root_path().wstring();
            if (GetVolumeInformationW(volume.c_str(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) &&
                (flags & FILE_FILE_COMPRESSION) != 0) {
                const DWORD packed = GetFileAttributesW(copied.c_str());
                Expect(packed != INVALID_FILE_ATTRIBUTES && (packed & FILE_ATTRIBUTE_COMPRESSED) != 0,
                       "the archived chat's folder is stored packed");
            }
        }
        const auto listedCopy = aic::ListArchivedChats();
        Expect(listedCopy.size() == 1 && listedCopy[0].id == "part-g" && listedCopy[0].title == L"Архивный" &&
                   listedCopy[0].folder == arc && listedCopy[0].parts.size() == 2 && !listedCopy[0].parts[0].grok &&
                   listedCopy[0].parts[1].grok && listedCopy[0].first != 0 && listedCopy[0].last != 0,
               "the manifest: title, project, both parts, times");
        bool cut = false;
        const auto readBack = aic::LoadArchivedPart("part-g", "part-c", false, 20, cut);
        Expect(!readBack.empty() && readBack[0].kind == aic::EntryKind::User && readBack[0].text == L"archive claude",
               "an archived part reads back");
        Expect(aic::DeleteSession(arc, "part-c") && aic::DeleteSession(arc, "part-g"), "the live files then go");
        Expect(aic::ListSessions(arc).empty(), "nothing live remains");
        Expect(aic::RestoreArchivedChat("part-g"), "returned to the chats");
        Expect(!fs::exists(copied), "the archive copy goes once it is back");
        Expect(ReadAll(historyArc / L"part-c.jsonl").find("archive claude") != std::string::npos &&
                   ReadAll(historyArc / L"part-c" / L"note.txt") == "side-note" &&
                   ReadAll(grokArc / L"part-g" / L"extra.bin") == "grok-extra",
               "the same files are in the history again");
        const auto restored = aic::ListSessions(arc);
        const auto again =
            std::ranges::find_if(restored, [](const aic::SessionInfo& s) { return aic::SameChat(s, "part-c"); });
        Expect(again != restored.end() && again->parts.size() == 2 && again->title == L"Архивный",
               "chats.json has the chat again");
        {
            std::ofstream(archiveRoot / L"digest.md")
                << "закончен part-g, но это не список\n\n```archive\nno-such\n```\n";
        }
        Expect(aic::ChatsNamedInDigest() == std::vector<std::string>{"no-such"}, "the digest names only its fence");
        if (again != restored.end()) {
            Expect(aic::ArchiveChat(arc, *again), "copied once more");
            Expect(!aic::PurgeArchivedChat("part-g", false) && fs::exists(archiveRoot / L"part-g"),
                   "a chat the digest did not name is not erased");
        }
    }
    aic::SetArchiveFailAfter(-1);
    aic::SetArchiveRoot({});
    fs::remove_all(archiveRoot, error);
    fs::remove_all(historyArc, error);
    fs::remove_all(grokArc, error);
    fs::remove(archiveIndex, error);

    // ask: the other model's API, the pieces that need no network.
    {
        Expect(aic::AskModels::Find("Gemini") != nullptr && aic::AskModels::Find("groq")->secret == "GROQ_API_KEY" &&
                   aic::AskModels::Find("nobody") == nullptr,
               "providers are found by name, any case");
        const std::string body = aic::AskModels::RequestBody("m", "be brief", "hi \"there\"", 999999);
        const aic::Json sent = aic::Json::parse(body);
        Expect(sent["model"] == "m" && sent["max_tokens"] == 16000 && sent["messages"].size() == 2 &&
                   sent["messages"][0]["role"] == "system" && sent["messages"][1]["content"] == "hi \"there\"",
               "the request: system first, the prompt whole, the token cap held");
        Expect(aic::Json::parse(aic::AskModels::RequestBody("m", "", "q", 0))["messages"].size() == 1 &&
                   aic::Json::parse(aic::AskModels::RequestBody("m", "", "q", 0))["max_tokens"] == 1,
               "no system message when there is none; at least one token");
        std::string usage;
        std::string askError;
        const std::string answer = aic::AskModels::ParseReply(
            R"({"choices":[{"message":{"content":"  Привет!\n"}}],"usage":{"prompt_tokens":12,"completion_tokens":3}})",
            usage, askError);
        Expect(answer == "Привет!" && usage == "12 in / 3 out" && askError.empty(), "an answer is trimmed, its usage read");
        usage.clear();
        Expect(aic::AskModels::ParseReply(R"({"choices":[{"message":{"content":null},"finish_reason":"length"}]})",
                                          usage, askError).empty() &&
                   askError.find("length") != std::string::npos,
               "no text: the finish reason says why");
        Expect(aic::AskModels::ErrorMessage(R"([{"error":{"code":400,"message":"API key not valid"}}])") ==
                       "API key not valid" &&
                   aic::AskModels::ErrorMessage(R"({"error":{"message":"rate limit"}})") == "rate limit" &&
                   aic::AskModels::ErrorMessage("<html>bad gateway</html>") == "<html>bad gateway</html>",
               "an error message from either shape, or the body's start");
        Expect(aic::AskModels::ParseModels(R"({"data":[{"id":"a"},{"id":"b/c:free"}]})") == "a\nb/c:free" &&
                   aic::AskModels::ParseModels(R"([{"name":"models/x"}])") == "models/x",
               "model ids listed from both shapes");
    }
    if (failures == 0) {
        std::cout << "store: all passed\n";
    }
    return failures == 0 ? 0 : 1;
}
