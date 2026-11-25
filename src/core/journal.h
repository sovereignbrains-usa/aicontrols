#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aic {

// The failures journal: what went wrong (an error in a chat, the CLI gone, the browser, a chat not named, the
// client itself crashed or hung as Windows Error Reporting saw it), one JSON line each in
// %LOCALAPPDATA%\AIControls\journal.jsonl, newest last, the oldest dropped past a size. Any thread may write.
// A line is reviewed once it was taken into the chat ("rev":true; a line without it is not). The sidebar's bug
// lights while any line is still unreviewed. The window draws the journal itself; nothing here writes a page.

struct JournalEntry {
    long long ms = 0;
    std::string time;    // local, "2026-11-03 04:12:55"
    std::string source;  // "чат", "сбой", "браузер", ...
    std::string build;   // the exe's link stamp (as Windows' crash reports name it), "dev" after it for a dev build
    std::wstring text;
    bool reviewed = false;
};

// The same source and the same text, collapsed. Newest group first.
struct JournalGroup {
    long long ms = 0;       // the newest copy
    long long firstMs = 0;  // the oldest copy, so a brief can walk oldest first
    std::string time;       // the newest copy, local
    std::string source;
    std::string build;      // the newest copy
    std::wstring text;
    int count = 0;  // every copy
    int fresh = 0;  // the copies not reviewed yet
    bool crash = false;
};

std::wstring JournalFile();  // journal.jsonl
void SetJournalFolder(const std::wstring& folder);  // tests: somewhere else. Drops the unseen cache and bumps the generation.
void JournalWrite(std::string_view source, std::wstring_view text);
std::vector<JournalEntry> ReadJournal(size_t limit = 200);  // oldest first, within the limit
std::vector<JournalGroup> ReadJournalGroups();               // newest group first
// How many lines are not reviewed yet (copies, not groups).
size_t JournalUnseen();
// Bumped on a write, a successful review or clear, and SetJournalFolder. A plain load, no lock.
uint64_t JournalGeneration();
// The unreviewed groups as text for a message (oldest group first, no path, no build) and those lines marked
// reviewed. Empty, and nothing written, when there is nothing new or the rewrite failed.
std::wstring TakeNewJournal();
// reviewedOnly drops the reviewed lines (a mixed group keeps its fresh copies); otherwise the file is emptied.
// How many lines went. A failed rewrite returns 0 and changes nothing.
size_t ClearJournal(bool reviewedOnly);
// Windows' reports of this exe crashing or hanging that came after the last look, into the journal (the first
// look only remembers the time: older ones were before the journal). Returns how many.
size_t ImportCrashReports();
// "aicontrols.exe+0x1a822d c0000409" from a Report.wer's text (UTF-16 already made wide). Empty if not a crash or hang
// of this client.
std::wstring CrashSummary(std::wstring_view report);

}  // namespace aic
