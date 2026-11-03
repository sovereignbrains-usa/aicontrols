#include "core/journal.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>

#include "core/claude_process.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace aic {
namespace {

namespace fs = std::filesystem;

constexpr uintmax_t kMostBytes = 256 * 1024;  // past it, the oldest go
constexpr size_t kKeepLines = 150;

std::mutex g_journalMutex;
std::wstring g_folder;                 // empty: %LOCALAPPDATA%\AIControls
std::optional<size_t> g_unseen;        // counted once, then kept up as lines come

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
        line.entry.time = JsonStr(parsed, "time");
        line.entry.source = JsonStr(parsed, "src");
        line.entry.build = JsonStr(parsed, "build");
        line.entry.text = ToWide(JsonStr(parsed, "text"));
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string Escape(const std::wstring& text) {
    std::string out;
    for (const char c : ToUtf8(text)) {
        switch (c) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

bool WritePageLocked();

void WriteLocked(long long ms, std::string_view source, std::wstring_view text) {
    const fs::path folder = Folder();
    std::error_code error;
    fs::create_directories(folder, error);
    const fs::path file = folder / L"journal.jsonl";
    std::string line = Json{{"ms", ms}, {"time", LocalTime(ms)}, {"src", std::string(source)}, {"build", Build()},
                            {"text", ToUtf8(std::wstring(text))}}
                           .dump(-1, ' ', false, Json::error_handler_t::replace);
    line += '\n';
    {
        std::ofstream out(file, std::ios::binary | std::ios::app);
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
    }
    if (fs::file_size(file, error) > kMostBytes) {  // the newest kept, written beside it and moved over
        std::ifstream in(file, std::ios::binary);
        std::deque<std::string> kept;
        std::string row;
        while (std::getline(in, row)) {
            kept.push_back(std::move(row));
            if (kept.size() > kKeepLines) {
                kept.pop_front();
            }
        }
        in.close();
        const fs::path fresh = folder / L"journal.jsonl.new";
        {
            std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
            for (const std::string& kept_line : kept) {
                out << kept_line << '\n';
            }
        }
        fs::rename(fresh, file, error);
    }
    if (g_unseen) {
        ++*g_unseen;
    }
    if (fs::exists(folder / L"journal.html", error)) {  // the page, if it was ever opened: the preview reloads it
        WritePageLocked();
    }
}

bool WritePageLocked() {
    std::vector<Line> lines = ReadLines();
    std::string rows;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const JournalEntry& e = it->entry;
        const bool crash = e.source == "сбой";
        rows += std::format("<article class=\"{}\"><header><time>{}</time><b>{}</b><span>{}</span></header><pre>{}</pre></article>\n",
                            crash ? "crash" : "", e.time, Escape(ToWide(e.source)), Escape(ToWide(e.build)),
                            Escape(e.text));
    }
    if (rows.empty()) {
        rows = "<p class=\"empty\">Сбоев не было.</p>";
    }
    const std::string page =
        "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\"><title>Журнал сбоев</title><style>\n"
        ":root{color-scheme:dark}*{box-sizing:border-box}\n"
        "body{margin:0;background:#0e0e0f;color:#f2f4f8;font:13px/1.5 Geist,'Segoe UI Variable Text','Segoe UI',sans-serif}\n"
        ".bar{position:sticky;top:0;display:flex;gap:8px;align-items:center;padding:12px 16px;background:#121213;"
        "border-bottom:1px solid #2c2c31}\n"
        ".bar h1{font-size:14px;font-weight:600;margin:0 auto 0 0}\n"
        "button{font:inherit;color:#f2f4f8;background:#232327;border:0;border-radius:8px;padding:6px 12px;cursor:pointer;"
        "transition:background .14s ease-out}\n"
        "button:hover{background:#2c2c31}button.primary{background:#4c92ff}button.primary:hover{background:#5d9dff}\n"
        "button:focus-visible{outline:2px solid #4c92ff;outline-offset:2px}\n"
        "main{padding:8px 16px 24px}\n"
        "article{border-left:3px solid #2c2c31;background:#18181a;border-radius:8px;padding:8px 12px;margin:8px 0}\n"
        "article.crash{border-left-color:#f06060}\n"
        "header{display:flex;gap:12px;align-items:baseline;color:#9c9ea4;font-size:12px}\n"
        "header b{color:#e8b04c;font-weight:600}article.crash header b{color:#f06060}\n"
        "header span{margin-left:auto;font-family:Consolas,monospace}\n"
        "pre{margin:4px 0 0;white-space:pre-wrap;word-break:break-word;font:12px/1.5 Consolas,'Cascadia Mono',monospace;"
        "color:#dadde4}\n"
        ".empty{color:#9c9ea4;padding:24px 0}\n"
        "</style></head><body>\n"
        "<div class=\"bar\"><h1>Журнал сбоев</h1><button id=\"copy\">Копировать</button>"
        "<button class=\"primary\" id=\"chat\">Разобрать в чате</button></div>\n<main id=\"list\">\n" +
        rows +
        "</main>\n<script>\n"
        "const post=m=>window.chrome&&chrome.webview&&chrome.webview.postMessage(JSON.stringify(m));\n"  // the window takes strings only
        "document.getElementById('chat').onclick=()=>post({aic:'journal-chat'});\n"
        "document.getElementById('copy').onclick=async e=>{const t=document.getElementById('list').innerText;"
        "try{await navigator.clipboard.writeText(t)}catch{const a=document.createElement('textarea');a.value=t;"
        "document.body.append(a);a.select();document.execCommand('copy');a.remove()}"
        "e.target.textContent='Скопировано';setTimeout(()=>e.target.textContent='Копировать',1400)};\n"
        "</script></body></html>\n";
    const fs::path folder = Folder();
    std::error_code error;
    fs::create_directories(folder, error);
    const fs::path fresh = folder / L"journal.html.new";
    {
        std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
        out << page;
        if (!out) {
            return false;
        }
    }
    fs::rename(fresh, folder / L"journal.html", error);
    return !error;
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

}  // namespace

std::wstring JournalFile() {
    return (Folder() / L"journal.jsonl").wstring();
}

std::wstring JournalPage() {
    return (Folder() / L"journal.html").wstring();
}

void SetJournalFolder(const std::wstring& folder) {
    const std::lock_guard lock(g_journalMutex);
    g_folder = folder;
    g_unseen.reset();
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

size_t JournalUnseen() {
    const std::lock_guard lock(g_journalMutex);
    if (!g_unseen) {
        const long long seen = ReadNumber(Folder() / L"journal.seen");
        const std::vector<Line> lines = ReadLines();
        g_unseen = static_cast<size_t>(std::ranges::count_if(lines, [&](const Line& l) { return l.ms > seen; }));
    }
    return *g_unseen;
}

void MarkJournalSeen() {
    const std::lock_guard lock(g_journalMutex);
    std::error_code error;
    fs::create_directories(Folder(), error);
    WriteNumber(Folder() / L"journal.seen", NowMs());
    g_unseen = 0;
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

bool WriteJournalPage() {
    const std::lock_guard lock(g_journalMutex);
    return WritePageLocked();
}

std::wstring JournalText(size_t most) {
    const std::lock_guard lock(g_journalMutex);
    const std::vector<Line> lines = ReadLines();
    std::wstring text;
    const size_t first = lines.size() > most ? lines.size() - most : 0;
    for (size_t i = first; i < lines.size(); ++i) {
        const JournalEntry& e = lines[i].entry;
        text += ToWide(e.time) + L" · " + ToWide(e.source) + L" · " + ToWide(e.build) + L"\n" + e.text + L"\n\n";
    }
    return text;
}

}  // namespace aic
