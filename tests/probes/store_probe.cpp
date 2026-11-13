// By hand: how the chats of the default workspace are titled (how many still have none); --grok <folder>: a project's
// Grok chats; --md <folder> <id>: a chat as Markdown; --open <folder> <id>: the time to open a chat (the last 200).
#include <chrono>
#include <iostream>
#include <string>

#include "core/chat_export.h"
#include "core/claude_process.h"
#include "core/store.h"

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--delete") {  // deletes one chat by id (to the Recycle Bin)
        const bool ok = argc > 2 && aic::DeleteSession(aic::DefaultWorkspace(), aic::ToUtf8(argv[2]));
        std::cout << (ok ? "deleted\n" : "not deleted\n");
        return ok ? 0 : 1;
    }
    if (argc > 3 && std::wstring(argv[1]) == L"--own") {  // --own <folder> <id>: does the chat count as named already
        std::cout << (aic::HasOwnTitle(argv[2], aic::ToUtf8(argv[3])) ? "has a title of its own\n" : "to be named\n");
        return 0;
    }
    if (argc > 3 && std::wstring(argv[1]) == L"--open") {  // --open <folder> <id>: how long opening a chat takes
        const auto begin = std::chrono::steady_clock::now();
        bool truncated = false;
        const auto entries = aic::LoadTranscript(argv[2], aic::ToUtf8(argv[3]), 200, truncated);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
        size_t text = 0;
        for (const auto& entry : entries) {
            text += entry.text.size() + entry.toolOutput.size() + entry.toolInput.size();
        }
        std::cout << entries.size() << " entries" << (truncated ? ", truncated" : "") << ", " << text << " chars, "
                  << ms.count() << " ms\n";
        return 0;
    }
    if (argc > 3 && std::wstring(argv[1]) == L"--md") {  // a chat as its Markdown file would be
        const auto markdown = aic::ChatMarkdown(argv[2], aic::ToUtf8(argv[3]));
        std::cout << (markdown ? *markdown : std::string("no such chat\n"));
        return markdown ? 0 : 1;
    }
    if (argc > 3 && std::wstring(argv[1]) == L"--handoff") {  // what a switch of CLI would pass on: its size, its start
        for (const auto& session : aic::ListSessions(argv[2])) {
            if (!aic::SameChat(session, aic::ToUtf8(argv[3]))) {
                continue;
            }
            auto parts = session.parts;
            if (parts.empty()) {
                parts.push_back({session.id, session.grok});
            }
            std::vector<std::pair<bool, std::vector<aic::TranscriptEntry>>> loaded;
            for (const auto& part : parts) {
                bool truncated = false;
                loaded.emplace_back(part.grok, aic::LoadTranscript(argv[2], part.id, SIZE_MAX, truncated));
            }
            const std::string said = aic::ToUtf8(aic::ChatSoFar(loaded, 40'000));
            std::cout << said.size() << " bytes\n" << said.substr(0, 600) << "\n…\n" << said.substr(said.size() - 300) << "\n";
            return 0;
        }
        std::cout << "no such chat\n";
        return 1;
    }
    if (argc > 2 && std::wstring(argv[1]) == L"--grok") {  // a project's Grok chats, and what opening one shows
        for (const auto& session : aic::ListSessions(argv[2])) {
            if (!session.grok) {
                continue;
            }
            bool truncated = false;
            const auto entries = aic::LoadTranscript(argv[2], session.id, 500, truncated);
            size_t tools = 0;
            for (const auto& entry : entries) {
                tools += entry.kind == aic::EntryKind::Tool ? 1 : 0;
            }
            std::cout << session.id << "  " << aic::ToUtf8(session.title) << "  [" << entries.size() << " entries, "
                      << tools << " tools" << (truncated ? ", truncated" : "") << "]\n";
        }
        return 0;
    }
    const auto sessions = aic::ListSessions(aic::DefaultWorkspace());
    size_t untitled = 0;
    for (const auto& session : sessions) {
        if (session.title.empty()) {
            ++untitled;
        }
    }
    std::cout << "chats: " << sessions.size() << ", untitled: " << untitled << "\n";
    for (size_t i = 0; i < sessions.size() && i < 12; ++i) {
        std::cout << "  " << aic::ToUtf8(sessions[i].title.empty() ? L"(none)" : sessions[i].title) << "\n";
    }
    return 0;
}
