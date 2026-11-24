// ParseGrokAsk pulls the headless JSON object out of stdout mixed with stderr. It does not run grok.exe.
#include <iostream>
#include <string>

#include "core/grok_process.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

}  // namespace

int main() {
    {
        const aic::GrokAskJson parsed = aic::ParseGrokAsk(
            R"({"text":"Sidebar scroll","stopReason":"end_turn","sessionId":"abc","cwd":"C:\\Temp\\aicontrols-title"})");
        Expect(parsed.ok && parsed.text == "Sidebar scroll" && parsed.sessionId == "abc" &&
                   parsed.cwd == "C:\\Temp\\aicontrols-title" && parsed.error.empty(),
               "a clean end_turn is the answer");
    }
    {
        const aic::GrokAskJson parsed =
            aic::ParseGrokAsk("warning: updater skipped\n" R"({"text":"Memory note","stopReason":"end_turn"})" "\n");
        Expect(parsed.ok && parsed.text == "Memory note", "stderr before the object is ignored");
    }
    {
        const aic::GrokAskJson parsed = aic::ParseGrokAsk(
            R"({"text":"{\"append\":[],\"new\":[{\"file\":\"a.md\",\"body\":\"x\"}]}","stopReason":"end_turn"})");
        Expect(parsed.ok && parsed.text == "{\"append\":[],\"new\":[{\"file\":\"a.md\",\"body\":\"x\"}]}",
               "braces and quotes inside the text stay part of the text");
    }
    {
        const aic::GrokAskJson parsed = aic::ParseGrokAsk(
            R"({"text":"first","stopReason":"end_turn"})" "\n"
            R"({"warning":"skipped"})" "\n"
            R"({"text":"second","stopReason":"end_turn","sessionId":"s2"})");
        Expect(parsed.ok && parsed.text == "second" && parsed.sessionId == "s2", "the last answer object wins");
    }
    {
        const aic::GrokAskJson parsed = aic::ParseGrokAsk(R"({"text":"","stopReason":"tool_use"})");
        Expect(!parsed.ok && parsed.error == "tool_use", "tool_use is not an answer");
    }
    {
        const aic::GrokAskJson parsed = aic::ParseGrokAsk(R"({"text":"partial","stopReason":"hook_blocked"})");
        Expect(!parsed.ok && parsed.error == "hook_blocked", "hook_blocked is not a success");
    }
    {
        const aic::GrokAskJson parsed = aic::ParseGrokAsk(R"({"text":"cut","stopReason":"length"})");
        Expect(!parsed.ok && parsed.error == "length", "an unknown stop reason is not a success");
    }
    {
        const aic::GrokAskJson parsed = aic::ParseGrokAsk("no json here");
        Expect(!parsed.ok && parsed.text.empty() && parsed.error == "no answer", "no object is no answer");
    }
    {
        const aic::GrokAskJson empty = aic::ParseGrokAsk(R"({"text":"","stopReason":"end_turn"})");
        const aic::GrokAskJson spaces = aic::ParseGrokAsk(R"({"text":"  \n\t","stopReason":"end_turn"})");
        Expect(!empty.ok && empty.error == "empty answer", "an empty end_turn is an empty answer");
        Expect(!spaces.ok && spaces.error == "empty answer", "a blank end_turn is an empty answer");
    }
    {
        const aic::GrokAskJson parsed = aic::ParseGrokAsk(R"({"text":"orphan","sessionId":"x"})");
        Expect(!parsed.ok && parsed.error == "no answer" && parsed.sessionId == "x",
               "a missing stop reason is not a success");
    }
    {
        const aic::GrokAskJson parsed =
            aic::ParseGrokAsk(R"({"error":"model overloaded","stopReason":"error"})");
        Expect(!parsed.ok && parsed.error == "model overloaded", "an error string wins over the stop reason");
    }
    return failures == 0 ? 0 : 1;
}
