#pragma once

#include <atomic>
#include <string>

namespace aic {

// A chat's title from what it is about, not its first line: one cheap call (Haiku, no tools, no session kept)
// over the first exchange, after its first answer. Stored as the CLI's own generated title would be, so a
// title the user gave stays above it.

constexpr size_t kTitleLongest = 48;  // characters; the answer is asked for 40

// The model's answer as a title: its first line, no quotes, no closing full stop, no "Название:", shortened on
// a word. Empty if nothing usable came.
std::wstring CleanTitle(const std::string& answer);

struct TitleWrite {
    bool ok = false;
    std::wstring title;
    std::string error;
};
// Asks for the title of a workspace's chat from its first user message and the start of the reply.
TitleWrite GenerateChatTitle(const std::wstring& workspace, const std::string& sessionId,
                             const std::atomic<bool>* cancel);

}  // namespace aic
