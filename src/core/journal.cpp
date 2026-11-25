#include "core/journal.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "core/claude_process.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace aic {
namespace {

namespace fs = std::filesystem;

constexpr uintmax_t kMostBytes = 256 * 1024;  // past it, the oldest go
constexpr size_t kKeepLines = 150;

std::mutex g_journalMutex;
std::wstring g_folder;                          // empty: %LOCALAPPDATA%\AIControls
std::optional<size_t> g_unseen;                 // counted once, then kept up as lines come
std::atomic<uint64_t> g_generation{0};

fs::path Folder() {
    if (!g_folder.empty()) {
        return g_folder;
    }
    wchar_t local[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return fs::path(local) / L"AIControls";
}

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// FILETIME ticks (UTC, 100 ns since 1601) to milliseconds since 1970.
long long TicksToMs(unsigned long long ticks) {
    return static_cast<long long>(ticks / 10'000) - 11'644'473'600'000LL;
}

std::string LocalTime(long long ms) {
    const unsigned long long ticks = static_cast<unsigned long long>(ms + 11'644'473'600'000LL) * 10'000;
    FILETIME utc{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    FILETIME local{};
    SYSTEMTIME t{};
    FileTimeToLocalFileTime(&utc, &local);
    FileTimeToSystemTime(&local, &t);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

// The exe's link stamp, the one Windows' crash reports name a build by; "dev" unless it runs from where the
// installer puts it.
std::string Build() {
    static const std::string build = [] {
        const auto* dos = &__ImageBase;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(dos) + dos->e_lfanew);
        std::string stamp = std::format("{:08x}", nt->FileHeader.TimeDateStamp);
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring lower = path;
        std::ranges::transform(lower, lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (lower.find(L"\\programs\\aicontrols\\") == std::wstring::npos) {
            stamp += " dev";
        }
        return stamp;
    }();
    return build;
}

long long ReadNumber(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    long long value = 0;
    in >> value;
    return in ? value : 0;
}

void WriteNumber(const fs::path& file, long long value) {
    std::ofstream(file, std::ios::binary | std::ios::trunc) << value;
}

struct Line {
    long long ms = 0;
    JournalEntry entry;
};

std::vector<Line> ReadLines() {
    std::vector<Line> lines;
    std::ifstream in(Folder() / L"journal.jsonl", std::ios::binary);
    std::string row;
    while (std::getline(in, row)) {
        const Json parsed = Json::parse(row, nullptr, false);
        if (!parsed.is_object()) {
            continue;
        }
        Line line;
        const Json& ms = JsonAt(parsed, "ms");
        line.ms = ms.is_number_integer() ? ms.get<long long>() : 0;
        line.entry.ms = line.ms;
        line.entry.time = JsonStr(parsed, "time");
        line.entry.source = JsonStr(parsed, "src");
        line.entry.build = JsonStr(parsed, "build");
        line.entry.text = ToWide(JsonStr(parsed, "text"));
        const Json& rev = JsonAt(parsed, "rev");
        line.entry.reviewed = rev.is_boolean() && rev.get<bool>();
        lines.push_back(std::move(line));
    }
    return lines;
}

// "rev" only when the line was reviewed, so an older file (no field) stays unreviewed.
std::string DumpLine(const Line& line) {
    Json obj{{"ms", line.ms},
             {"time", line.entry.time},
             {"src", line.entry.source},
             {"build", line.entry.build},
             {"text", ToUtf8(line.entry.text)}};
    if (line.entry.reviewed) {
        obj["rev"] = true;
    }
    return obj.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// journal.jsonl.new beside the file, then replaced. std::filesystem::rename does not replace on Windows.
bool Rewrite(const std::vector<Line>& lines) {
    const fs::path folder = Folder();
    std::error_code error;
    fs::create_directories(folder, error);
    const fs::path fresh = folder / L"journal.jsonl.new";
    {
        std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        for (const Line& line : lines) {
            const std::string dumped = DumpLine(line);
            out.write(dumped.data(), static_cast<std::streamsize>(dumped.size()));
            out.put('\n');
        }
        if (!out) {
            return false;
        }
    }
    const fs::path file = folder / L"journal.jsonl";
    return MoveFileExW(fresh.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void WriteLocked(long long ms, std::string_view source, std::wstring_view text) {
    const fs::path folder = Folder();
    std::error_code error;
    fs::create_directories(folder, error);
    const fs::path file = folder / L"journal.jsonl";
    Line line;
    line.ms = ms;
    line.entry.ms = ms;
    line.entry.time = LocalTime(ms);
    line.entry.source = std::string(source);
    line.entry.build = Build();
    line.entry.text = std::wstring(text);
    {
        std::ofstream out(file, std::ios::binary | std::ios::app);
        const std::string dumped = DumpLine(line);
        out.write(dumped.data(), static_cast<std::streamsize>(dumped.size()));
        out.put('\n');
    }
    if (fs::file_size(file, error) > kMostBytes) {  // the newest kept; "rev" survives because the lines are read back
        std::vector<Line> lines = ReadLines();
        if (lines.size() > kKeepLines) {
            lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(kKeepLines));
            Rewrite(lines);
        }
    }
    if (g_unseen) {
        ++*g_unseen;
    }
    g_generation.fetch_add(1, std::memory_order_release);
}

std::wstring ReadWide(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
    }
    return ToWide(bytes);
}

std::map<std::wstring, std::wstring> ReportFields(std::wstring_view report) {
    std::map<std::wstring, std::wstring> fields;
    size_t at = 0;
    while (at < report.size()) {
        const size_t end = std::min(report.find_first_of(L"\r\n", at), report.size());
        const std::wstring_view line = report.substr(at, end - at);
        if (const size_t equals = line.find(L'='); equals != std::wstring_view::npos) {
            fields[std::wstring(line.substr(0, equals))] = std::wstring(line.substr(equals + 1));
        }
        at = end + 1;
    }
    return fields;
}

// Newest group first. A later (older) copy moves firstMs back and adds to the counts.
std::vector<JournalGroup> GroupLines(const std::vector<Line>& lines) {
    std::vector<JournalGroup> groups;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const JournalEntry& entry = it->entry;
        auto found = std::ranges::find_if(groups, [&](const JournalGroup& group) {
            return group.source == entry.source && group.text == entry.text;
        });
        if (found == groups.end()) {
            JournalGroup group;
            group.ms = it->ms;
            group.firstMs = it->ms;
            group.time = entry.time;
            group.source = entry.source;
            group.build = entry.build;
            group.text = entry.text;
            group.count = 1;
            group.fresh = entry.reviewed ? 0 : 1;
            group.crash = entry.source == "сбой";
            groups.push_back(std::move(group));
        } else {
            found->firstMs = it->ms;
            ++found->count;
            if (!entry.reviewed) {
                ++found->fresh;
            }
        }
    }
    return groups;
}

}  // namespace

std::wstring JournalFile() {
    return (Folder() / L"journal.jsonl").wstring();
}

void SetJournalFolder(const std::wstring& folder) {
    const std::lock_guard lock(g_journalMutex);
    g_folder = folder;
    g_unseen.reset();
    g_generation.fetch_add(1, std::memory_order_release);
}

void JournalWrite(std::string_view source, std::wstring_view text) {
    if (text.empty()) {
        return;
    }
    const std::lock_guard lock(g_journalMutex);
    WriteLocked(NowMs(), source, text);
}

std::vector<JournalEntry> ReadJournal(size_t limit) {
    const std::lock_guard lock(g_journalMutex);
    std::vector<Line> lines = ReadLines();
    std::vector<JournalEntry> entries;
    const size_t first = lines.size() > limit ? lines.size() - limit : 0;
    for (size_t i = first; i < lines.size(); ++i) {
        entries.push_back(std::move(lines[i].entry));
    }
    return entries;
}

std::vector<JournalGroup> ReadJournalGroups() {
    const std::lock_guard lock(g_journalMutex);
    return GroupLines(ReadLines());
}

size_t JournalUnseen() {
    const std::lock_guard lock(g_journalMutex);
    if (!g_unseen) {
        const std::vector<Line> lines = ReadLines();
        g_unseen = static_cast<size_t>(std::ranges::count_if(lines, [](const Line& line) { return !line.entry.reviewed; }));
    }
    return *g_unseen;
}

uint64_t JournalGeneration() {
    return g_generation.load(std::memory_order_acquire);
}

std::wstring TakeNewJournal() {
    const std::lock_guard lock(g_journalMutex);
    std::vector<Line> lines = ReadLines();
    std::vector<JournalGroup> groups = GroupLines(lines);
    std::erase_if(groups, [](const JournalGroup& group) { return group.fresh == 0; });
    if (groups.empty()) {
        return {};
    }
    std::ranges::stable_sort(groups, {}, &JournalGroup::firstMs);
    std::wstring brief;
    for (size_t i = 0; i < groups.size(); ++i) {
        const JournalGroup& group = groups[i];
        if (i != 0) {
            brief += L"\n\n";
        }
        brief += ToWide(group.time) + L" · " + ToWide(group.source);
        if (group.fresh > 1) {
            brief += L" · ×" + std::to_wstring(group.fresh);
        }
        brief += L"\n" + group.text;
    }
    for (Line& line : lines) {
        if (!line.entry.reviewed) {
            line.entry.reviewed = true;
        }
    }
    if (!Rewrite(lines)) {
        return {};
    }
    g_unseen.reset();
    g_generation.fetch_add(1, std::memory_order_release);
    return brief;
}

size_t ClearJournal(bool reviewedOnly) {
    const std::lock_guard lock(g_journalMutex);
    std::vector<Line> lines = ReadLines();
    std::vector<Line> kept;
    size_t removed = 0;
    if (reviewedOnly) {
        for (Line& line : lines) {
            if (line.entry.reviewed) {
                ++removed;
            } else {
                kept.push_back(std::move(line));
            }
        }
    } else {
        removed = lines.size();
    }
    if (removed == 0) {
        return 0;
    }
    if (!Rewrite(reviewedOnly ? kept : std::vector<Line>{})) {
        return 0;
    }
    g_unseen.reset();
    g_generation.fetch_add(1, std::memory_order_release);
    return removed;
}

std::wstring CrashSummary(std::wstring_view report) {
    std::map<std::wstring, std::wstring> f = ReportFields(report);
    std::wstring app = f[L"AppPath"];
    std::ranges::transform(app, app.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (!app.ends_with(L"\\aicontrols.exe")) {
        return {};
    }
    const std::wstring type = f[L"EventType"];
    if (type.starts_with(L"AppHang")) {
        return L"Клиент завис и был закрыт (Windows: «не отвечает»), сборка " + f[L"Sig[2].Value"] + L".";
    }
    if (type != L"APPCRASH" && type != L"BEX64" && type != L"BEX") {
        return {};
    }
    std::wstring code = f[L"Sig[7].Value"];
    std::wstring offset = f[L"Sig[6].Value"];
    const std::wstring module = f[L"Sig[3].Value"];  // APPCRASH and BEX64 (a fast-fail) alike
    std::ranges::transform(code, code.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    while (offset.size() > 1 && offset.front() == L'0') {
        offset.erase(offset.begin());
    }
    std::wstring meaning;
    if (code == L"c0000409") {
        meaning = L"аварийный выход (__fastfail: std::terminate, переполнение буфера, неверный параметр)";
    } else if (code == L"c0000005") {
        meaning = L"обращение к чужой памяти";
    } else if (code == L"c00000fd") {
        meaning = L"переполнение стека";
    } else if (code == L"e06d7363") {
        meaning = L"необработанное исключение C++";
    } else if (code == L"c0000374") {
        meaning = L"повреждена куча";
    }
    std::wstring text = L"Клиент упал: " + code + (meaning.empty() ? L"" : L" — " + meaning) + L"\nв " + module +
                        L"+0x" + offset + L", сборка " + f[L"Sig[2].Value"] + L".";
    return text;
}

size_t ImportCrashReports() {
    const std::lock_guard lock(g_journalMutex);
    std::error_code error;
    fs::create_directories(Folder(), error);
    const fs::path mark = Folder() / L"journal.wer";
    const long long last = ReadNumber(mark);
    const long long now = NowMs();
    WriteNumber(mark, now);
    if (last == 0) {
        return 0;  // the first look: what is older came before the journal
    }
    wchar_t local[MAX_PATH]{};
    wchar_t data[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    GetEnvironmentVariableW(L"PROGRAMDATA", data, MAX_PATH);
    struct Found {
        long long ms;
        std::wstring text;
    };
    std::vector<Found> found;
    for (const fs::path& root : {fs::path(data), fs::path(local)}) {
        for (const wchar_t* kind : {L"ReportArchive", L"ReportQueue"}) {
            const fs::path folder = root / L"Microsoft" / L"Windows" / L"WER" / kind;
            for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
                const std::wstring name = it->path().filename().wstring();
                if (!name.starts_with(L"AppCrash_aicontrols.exe_") && !name.starts_with(L"AppHang_aicontrols.exe_") &&
                    !name.starts_with(L"BEX64_aicontrols.exe_")) {
                    continue;
                }
                const fs::path report = it->path() / L"Report.wer";
                WIN32_FILE_ATTRIBUTE_DATA attributes{};
                if (!GetFileAttributesExW(report.c_str(), GetFileExInfoStandard, &attributes)) {
                    continue;
                }
                const unsigned long long written =
                    (static_cast<unsigned long long>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
                    attributes.ftLastWriteTime.dwLowDateTime;
                if (TicksToMs(written) <= last) {
                    continue;
                }
                const std::wstring text = ReadWide(report);
                std::wstring summary = CrashSummary(text);
                if (summary.empty()) {
                    continue;
                }
                long long ms = TicksToMs(written);
                if (const auto fields = ReportFields(text); fields.contains(L"EventTime")) {
                    ms = TicksToMs(std::wcstoull(fields.at(L"EventTime").c_str(), nullptr, 10));
                }
                found.push_back({ms, summary + L"\nОтчёт Windows: " + it->path().wstring()});
            }
            error.clear();
        }
    }
    std::ranges::sort(found, {}, &Found::ms);
    for (const Found& crash : found) {
        WriteLocked(crash.ms, "сбой", crash.text);
    }
    return found.size();
}

}  // namespace aic
