#include "core/title_writer.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/auth.h"
#include "core/claude_process.h"
#include "core/store.h"

namespace aic {
namespace {

namespace fs = std::filesystem;

constexpr size_t kPromptChars = 1'500;  // of the first message
constexpr size_t kReplyChars = 1'200;   // of the answer's start

std::string Cut(std::string text, size_t most) {
    if (text.size() > most) {
        size_t at = most;
        while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
            --at;  // not inside a UTF-8 letter
        }
        text = text.substr(0, at) + " […]";
    }
    return text;
}

bool Space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L' ';
}

}  // namespace

std::wstring CleanTitle(const std::string& answer) {
    std::wstring text = ToWide(answer);
    // The first line with words; a fence or a "Title:" line is not the title.
    std::wstring line;
    for (size_t at = 0; at < text.size();) {
        const size_t end = std::min(text.find_first_of(L"\r\n", at), text.size());
        std::wstring row = text.substr(at, end - at);
        at = end + 1;
        while (!row.empty() && Space(row.front())) {
            row.erase(row.begin());
        }
        if (row.empty() || row.starts_with(L"```")) {
            continue;
        }
        line = std::move(row);
        break;
    }
    for (const std::wstring_view lead : {L"Название:", L"Название чата:", L"Заголовок:", L"Title:"}) {
        if (line.starts_with(lead)) {
            line.erase(0, lead.size());
        }
    }
    const std::wstring_view edges = L" \t \"'«»“”„`*#.";
    const size_t first = line.find_first_not_of(edges);
    if (first == std::wstring::npos) {
        return {};
    }
    const size_t last = line.find_last_not_of(edges);
    line = line.substr(first, last - first + 1);
    std::wstring title;  // runs of spaces as one
    for (const wchar_t c : line) {
        if (Space(c)) {
            if (!title.empty() && title.back() != L' ') {
                title.push_back(L' ');
            }
        } else {
            title.push_back(c);
        }
    }
    if (title.size() > kTitleLongest) {
        size_t cut = title.find_last_of(L' ', kTitleLongest);
        if (cut == std::wstring::npos || cut < kTitleLongest / 2) {
            cut = kTitleLongest;
        }
        title.resize(cut);
        while (!title.empty() && (title.back() == L' ' || title.back() == L',' || title.back() == L'—')) {
            title.pop_back();
        }
        title.push_back(L'…');
    }
    return title;
}

TitleWrite GenerateChatTitle(const std::wstring& workspace, const std::string& sessionId,
                             const std::atomic<bool>* cancel) {
    TitleWrite result;
    bool truncated = false;
    const std::vector<TranscriptEntry> entries = LoadTranscript(workspace, sessionId, 40, truncated);
    std::string asked;
    std::string answered;
    for (const TranscriptEntry& entry : entries) {
        if (entry.kind == EntryKind::User && asked.empty() && !entry.text.empty()) {
            asked = ToUtf8(entry.text);
        } else if (entry.kind == EntryKind::Assistant && !asked.empty() && !entry.text.empty()) {
            answered += (answered.empty() ? "" : "\n") + ToUtf8(entry.text);
            if (answered.size() > kReplyChars) {
                break;
            }
        }
    }
    if (asked.empty()) {
        result.error = "no message";
        return result;
    }
    const std::string prompt =
        "Придумай короткое название для этого чата — так, как его назвал бы человек в списке чатов.\n"
        "Правила: 2–6 слов, не длиннее 40 символов, на языке пользователя; суть задачи или темы, а не пересказ "
        "первой фразы; без кавычек, без точки в конце, без слов «чат», «вопрос», «помощь». Ответь одним "
        "названием, без пояснений.\n\n"
        "Пользователь: " + Cut(asked, kPromptChars) + "\n\n" +
        (answered.empty() ? std::string() : "Ассистент: " + Cut(answered, kReplyChars) + "\n");

    // The words go in through a file on standard input, the call runs in a scratch folder and keeps no session:
    // nothing of it shows among the chats.
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const fs::path scratch = fs::path(temp) / L"aicontrols-title";
    std::error_code error;
    fs::create_directories(scratch, error);
    const fs::path input = scratch / (L"prompt-" + std::to_wstring(GetCurrentThreadId()) + L".txt");
    {
        std::ofstream out(input, std::ios::binary);
        out << prompt;
    }
    const std::wstring exe = FindClaudeExe();
    if (exe.empty()) {
        fs::remove(input, error);
        result.error = "claude.exe not found";
        return result;
    }
    const std::wstring command =
        L"\"" + exe + L"\" -p --model haiku --tools \"\" --strict-mcp-config --no-session-persistence "
        L"--output-format json --system-prompt \"You name chats. Answer with the title only.\"";
    const CliResult run = RunCli(command, L"", 90, cancel, {}, {}, scratch.wstring(), input.wstring());
    fs::remove(input, error);
    if (!run.started || run.timedOut || run.cancelled) {
        result.error = run.timedOut ? "timed out" : "did not run";
        return result;
    }
    const size_t brace = run.output.find('{');
    const Json reply = brace == std::string::npos ? Json() : Json::parse(run.output.substr(brace), nullptr, false);
    const Json& text = JsonAt(reply, "result");
    const Json& failed = JsonAt(reply, "is_error");
    if (run.exitCode != 0 || !text.is_string() || (failed.is_boolean() && failed.get<bool>())) {
        result.error = text.is_string() ? text.get<std::string>().substr(0, 200) : run.output.substr(0, 200);
        return result;
    }
    result.title = CleanTitle(text.get<std::string>());
    result.ok = !result.title.empty();
    if (!result.ok) {
        result.error = "empty title";
    }
    return result;
}

}  // namespace aic
