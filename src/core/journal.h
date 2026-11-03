#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aic {

// The failures journal: what went wrong (an error in a chat, the CLI gone, the browser, a chat not named, the
// client itself crashed or hung as Windows Error Reporting saw it), one JSON line each in
// %LOCALAPPDATA%\AIControls\journal.jsonl, newest last, the oldest dropped past a size. Any thread may write.
// The sidebar's bug shows it as a page (journal.html beside it) and lights while there is something not seen.

struct JournalEntry {
    std::string time;    // local, "2026-11-03 04:12:55"
    std::string source;  // "чат", "сбой", "браузер", ...
    std::string build;   // the exe's link stamp (as Windows' crash reports name it), "dev" after it for a dev build
    std::wstring text;
};

std::wstring JournalFile();      // journal.jsonl
std::wstring JournalPage();      // journal.html
void SetJournalFolder(const std::wstring& folder);  // tests: somewhere else
void JournalWrite(std::string_view source, std::wstring_view text);
std::vector<JournalEntry> ReadJournal(size_t limit = 200);
// How many came after the page was last opened.
size_t JournalUnseen();
void MarkJournalSeen();
// Windows' reports of this exe crashing or hanging that came after the last look, into the journal (the first
// look only remembers the time: older ones were before the journal). Returns how many.
size_t ImportCrashReports();
// The page: newest first, its buttons copy all or put it in the chat's input ({"aic":"journal-chat","text":...}).
bool WriteJournalPage();
// The entries as text for a message: the newest `most`, oldest first.
std::wstring JournalText(size_t most = 20);
// "aicontrols.exe+0x1a822d c0000409" from a Report.wer's text (UTF-16 already made wide). Empty if not a crash or hang
// of this client.
std::wstring CrashSummary(std::wstring_view report);

}  // namespace aic
