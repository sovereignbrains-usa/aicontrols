#include "core/browser.h"

#include <windows.h>

#include <wil/resource.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "core/agent_process.h"
#include "core/bridge.h"
#include "core/claude_process.h"

namespace aic {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr size_t kTextBytes = 60 * 1024;

// The host of an http(s) address: no user, no port, no brackets. The caller has checked the scheme.
std::string HostOf(std::string_view url) {
    url.remove_prefix(url.find("://") + 3);
    url = url.substr(0, url.find_first_of("/?#"));
    if (const size_t at = url.rfind('@'); at != std::string_view::npos) {
        url.remove_prefix(at + 1);
    }
    if (url.starts_with('[')) {
        const size_t close = url.find(']');
        return std::string(url.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
    }
    return std::string(url.substr(0, url.find(':')));
}

// A page on this computer or in the local network: the same test as the bridge's (LocalTarget).
bool LocalAddress(std::string_view url) {
    if (!(url.starts_with("http://") || url.starts_with("https://"))) {
        return false;  // about:blank and the like reach no network
    }
    return LocalTarget(HostOf(url));
}

// Refused by the page's own network stack, subresources and redirects too (Network.setBlockedURLs takes URLPattern
// strings with a scheme). Loopback in loose forms (127.1) matches "127.*" as well. A one-label name
// (the router's "http://router/") has no pattern: the navigation check catches it when it is the page itself.
Json LocalPatterns() {
    // Hosts as URLPattern wildcards: each one as "*://host/*" (no port) and "*://host:*/*" (with one).
    std::vector<std::string> hosts = {"localhost", "*.localhost", "127.*", "0.*",  "10.*", "192.168.*",
                                      "169.254.*", "*.local",     "*.lan", "[*]"};  // [*]: IPv6 literals
    for (int second = 16; second <= 31; ++second) {
        hosts.push_back("172." + std::to_string(second) + ".*");
    }
    for (int second = 64; second <= 127; ++second) {  // 100.64/10: the shared space, no public one
        hosts.push_back("100." + std::to_string(second) + ".*");
    }
    Json patterns = Json::array();
    for (const std::string& host : hosts) {
        patterns.push_back(Json{{"urlPattern", "*://" + host + "/*"}, {"block", true}});
        patterns.push_back(Json{{"urlPattern", "*://" + host + ":*/*"}, {"block", true}});
    }
    return patterns;
}
constexpr size_t kImageBytes = 3'700'000;  // the same limit as a picture the user attaches
constexpr size_t kInboxLimit = 1000;       // unread events beyond this are dropped, the oldest first

// Edge first (it is on every Windows), then Chrome.
std::optional<fs::path> FindBrowser() {
    const auto env = [](const wchar_t* name) {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(name, buffer, MAX_PATH);
        return length == 0 || length >= MAX_PATH ? fs::path() : fs::path(buffer);
    };
    const fs::path x86 = env(L"ProgramFiles(x86)");
    const fs::path files = env(L"ProgramFiles");
    const fs::path local = env(L"LOCALAPPDATA");
    const fs::path candidates[] = {
        x86 / L"Microsoft" / L"Edge" / L"Application" / L"msedge.exe",
        files / L"Microsoft" / L"Edge" / L"Application" / L"msedge.exe",
        files / L"Google" / L"Chrome" / L"Application" / L"chrome.exe",
        local / L"Google" / L"Chrome" / L"Application" / L"chrome.exe",
    };
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (!candidate.empty() && fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

fs::path LocalAppData() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    return length == 0 || length >= MAX_PATH ? fs::temp_directory_path() : fs::path(buffer);
}

// The page's text is cut at a whole character.
std::string Cut(std::string text) {
    if (text.size() <= kTextBytes) {
        return text;
    }
    size_t at = kTextBytes;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
        --at;
    }
    return text.substr(0, at) + "\n… (текст обрезан)";
}

// DevTools Protocol over two pipes: Edge reads commands on fd 3 and writes replies and events on fd 4. Each message
// is JSON ended by a NUL byte. A reader thread files what comes in; Call and WaitEvent take it from there.
class Cdp {
public:
    bool Start(const fs::path& exe, const fs::path& profile, std::string& error);
    // Ends the browser (its job dies with it) and the reader. Safe to call when nothing runs.
    void Stop();
    bool Alive() const { return alive_; }
    // A command and its reply. An empty session is the browser itself, else a page's session.
    bool Call(const std::string& method, const Json& params, const std::string& session, Json& result,
              std::string& error, const std::atomic<bool>& cancel, std::chrono::seconds timeout = 20s);
    // True when an event of this method came for this session, within the time.
    bool WaitEvent(const std::string& method, const std::string& session, std::chrono::seconds timeout,
                   const std::atomic<bool>& cancel);
    // Drops the events of this method that already came: the next WaitEvent sees only what follows.
    void Forget(const std::string& method, const std::string& session);

private:
    void ReadLoop();
    bool Write(const Json& message);
    std::optional<Json> Take(const std::function<bool(const Json&)>& match, std::chrono::milliseconds timeout,
                             const std::atomic<bool>& cancel);

    std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<Json> inbox_;  // guarded by mutex_
    std::thread reader_;
    wil::unique_handle toBrowser_;
    wil::unique_handle fromBrowser_;
    wil::unique_handle job_;
    std::atomic<bool> alive_{false};
    int nextId_ = 1;  // only from Call, which runs under the session's lock
};

bool Cdp::Start(const fs::path& exe, const fs::path& profile, std::string& error) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    // The browser's ends are inheritable, ours are not.
    HANDLE raw[2]{};
    if (!CreatePipe(&raw[0], &raw[1], &inherit, 0)) {
        error = "CreatePipe failed";
        return false;
    }
    wil::unique_handle childIn(raw[0]);
    toBrowser_.reset(raw[1]);
    if (!CreatePipe(&raw[0], &raw[1], &inherit, 0)) {
        error = "CreatePipe failed";
        return false;
    }
    fromBrowser_.reset(raw[0]);
    wil::unique_handle childOut(raw[1]);
    SetHandleInformation(toBrowser_.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(fromBrowser_.get(), HANDLE_FLAG_INHERIT, 0);

    // The C runtime's inheritance block (lpReserved2): the count, a flags byte per descriptor, then the handles.
    // Descriptors 0 to 2 stay closed; 3 is the commands, 4 the replies.
    constexpr unsigned kCount = 5;
    constexpr unsigned char kOpen = 0x01;  // FOPEN
    constexpr unsigned char kPipe = 0x08;  // FPIPE
    std::vector<unsigned char> block(sizeof(unsigned) + kCount * (1 + sizeof(intptr_t)));
    const unsigned count = kCount;
    std::memcpy(block.data(), &count, sizeof(count));
    unsigned char* flags = block.data() + sizeof(unsigned);
    unsigned char* handles = flags + kCount;
    const intptr_t values[kCount] = {-1, -1, -1, reinterpret_cast<intptr_t>(childIn.get()),
                                     reinterpret_cast<intptr_t>(childOut.get())};
    for (unsigned i = 0; i < kCount; ++i) {
        flags[i] = static_cast<unsigned char>(i < 3 ? 0 : kOpen | kPipe);
        std::memcpy(handles + i * sizeof(intptr_t), &values[i], sizeof(intptr_t));
    }

    std::wstring line = L"\"" + exe.wstring() + L"\" --remote-debugging-pipe --user-data-dir=\"" + profile.wstring() +
                        L"\" --no-first-run --no-default-browser-check about:blank";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.cbReserved2 = static_cast<WORD>(block.size());
    startup.lpReserved2 = block.data();
    PROCESS_INFORMATION info{};
    // Suspended, so it is in the job before it runs anything: a job kills it and whatever it starts.
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr,
                        &startup, &info)) {
        error = "Не запустился " + ToUtf8(exe.filename().wstring());
        return false;
    }
    wil::unique_handle process(info.hProcess);
    wil::unique_handle thread(info.hThread);
    childIn.reset();
    childOut.reset();

    job_.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job_.get(), process.get())) {
        TerminateProcess(process.get(), 1);
        error = "Браузер не попал в задание";
        job_.reset();
        return false;
    }
    ResumeThread(thread.get());

    alive_ = true;
    reader_ = std::thread([this] { ReadLoop(); });
    return true;
}

void Cdp::Stop() {
    alive_ = false;
    if (job_) {
        TerminateJobObject(job_.get(), 1);
    }
    if (reader_.joinable()) {
        reader_.join();  // the pipe breaks once the browser is gone
    }
    job_.reset();
    toBrowser_.reset();
    fromBrowser_.reset();
}

void Cdp::ReadLoop() {
    std::string pending;
    std::vector<char> chunk(64 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(fromBrowser_.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) || got == 0) {
            break;
        }
        pending.append(chunk.data(), got);
        for (size_t end = pending.find('\0'); end != std::string::npos; end = pending.find('\0')) {
            Json message = Json::parse(pending.substr(0, end), nullptr, false);
            pending.erase(0, end + 1);
            if (message.is_discarded()) {
                continue;
            }
            {
                std::lock_guard lock(mutex_);
                inbox_.push_back(std::move(message));
                if (inbox_.size() > kInboxLimit) {
                    inbox_.pop_front();
                }
            }
            arrived_.notify_all();
        }
    }
    {
        std::lock_guard lock(mutex_);
        alive_ = false;
    }
    arrived_.notify_all();
}

bool Cdp::Write(const Json& message) {
    std::string bytes = message.dump(-1, ' ', false, Json::error_handler_t::replace);
    bytes.push_back('\0');
    DWORD written = 0;
    return WriteFile(toBrowser_.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
           written == bytes.size();
}

std::optional<Json> Cdp::Take(const std::function<bool(const Json&)>& match, std::chrono::milliseconds timeout,
                              const std::atomic<bool>& cancel) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(mutex_);
    for (;;) {
        for (auto it = inbox_.begin(); it != inbox_.end(); ++it) {
            if (match(*it)) {
                Json found = std::move(*it);
                inbox_.erase(it);
                return found;
            }
        }
        if (!alive_ || cancel || std::chrono::steady_clock::now() >= until) {
            return std::nullopt;
        }
        arrived_.wait_for(lock, 100ms);
    }
}

bool Cdp::Call(const std::string& method, const Json& params, const std::string& session, Json& result,
               std::string& error, const std::atomic<bool>& cancel, std::chrono::seconds timeout) {
    if (!alive_) {
        error = "Браузер закрыт";
        return false;
    }
    const int id = nextId_++;
    Json message{{"id", id}, {"method", method}, {"params", params}};
    if (!session.empty()) {
        message["sessionId"] = session;
    }
    if (!Write(message)) {
        error = "Браузер не принимает команды";
        return false;
    }
    const std::optional<Json> reply = Take(
        [id](const Json& m) { return m.contains("id") && m["id"] == id; },
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout), cancel);
    if (!reply) {
        error = cancel ? "Остановлено" : alive_ ? method + ": браузер не ответил" : "Браузер закрыт";
        return false;
    }
    if (reply->contains("error")) {
        error = reply->at("error").value("message", std::string("ошибка DevTools"));
        return false;
    }
    result = reply->value("result", Json::object());
    return true;
}

bool Cdp::WaitEvent(const std::string& method, const std::string& session, std::chrono::seconds timeout,
                    const std::atomic<bool>& cancel) {
    return Take([&](const Json& m) { return m.value("method", std::string()) == method &&
                                            m.value("sessionId", std::string()) == session; },
                std::chrono::duration_cast<std::chrono::milliseconds>(timeout), cancel)
        .has_value();
}

void Cdp::Forget(const std::string& method, const std::string& session) {
    std::lock_guard lock(mutex_);
    std::erase_if(inbox_, [&](const Json& m) {
        return m.value("method", std::string()) == method && m.value("sessionId", std::string()) == session;
    });
}

// The page as a person sees it: its title, address and text (JSON, so the quotes in a page do no harm).
constexpr const char* kPageStateJs =
    "JSON.stringify({t: document.title, u: location.href, x: document.body ? document.body.innerText : ''})";

// Clicks the element by CSS selector, or else the first link or button whose visible text holds the text given.
constexpr const char* kClickBody =
    "let el = null;"
    "if (sel) { el = document.querySelector(sel); }"
    "else {"
    "  const want = txt.replace(/\\s+/g, ' ').trim();"
    "  for (const c of document.querySelectorAll('a,button,input,label,summary,"
    "[role=button],[role=link],[role=tab],[role=menuitem],[role=option]')) {"
    "    const have = (c.value || c.textContent || '').replace(/\\s+/g, ' ').trim();"
    "    if (want && have.includes(want)) { el = c; break; }"
    "  }"
    "}"
    "if (!el) return 'NOTFOUND';"
    "el.scrollIntoView({block: 'center'});"
    "el.click();"
    "return 'OK';";

class Session {
public:
    BrowserResult Run(const BrowserRequest& request, const std::atomic<bool>& cancel);

private:
    // Starts the browser and attaches to a page when needed.
    bool Attach(std::string& error, const std::atomic<bool>& cancel);
    // A command for the page; a page that was closed gives its session up, so the next action attaches another.
    bool Command(const std::string& method, const Json& params, Json& result, std::string& error,
                 const std::atomic<bool>& cancel);
    // The page's title, address and, when asked, its text.
    std::string PageState(bool withText, std::string& error, const std::atomic<bool>& cancel);
    bool BlockLocalPage(std::string& error, const std::atomic<bool>& cancel);
    // Runs a script in the page; its string value comes back, empty on failure.
    std::string Evaluate(const std::string& expression, std::string& error, const std::atomic<bool>& cancel);

    std::mutex mutex_;  // one action at a time
    Cdp cdp_;
    std::string page_;  // the session of the page we drive
};

bool Session::Attach(std::string& error, const std::atomic<bool>& cancel) {
    if (!cdp_.Alive()) {
        cdp_.Stop();  // a reader that ended on its own is joined first
        page_.clear();
        const std::optional<fs::path> exe = FindBrowser();
        if (!exe) {
            error = "Не найден Microsoft Edge или Chrome";
            return false;
        }
        const fs::path profile = LocalAppData() / L"AIControls" / L"browser";
        std::error_code ec;
        fs::create_directories(profile, ec);
        if (!cdp_.Start(*exe, profile, error)) {
            return false;
        }
    }
    if (!page_.empty()) {
        return true;
    }
    Json targets;
    if (!cdp_.Call("Target.getTargets", Json::object(), "", targets, error, cancel)) {
        return false;
    }
    std::string target;
    for (const Json& info : targets.value("targetInfos", Json::array())) {
        if (info.value("type", std::string()) == "page") {
            target = info.value("targetId", std::string());
            break;
        }
    }
    if (target.empty()) {
        Json created;
        if (!cdp_.Call("Target.createTarget", Json{{"url", "about:blank"}}, "", created, error, cancel)) {
            return false;
        }
        target = created.value("targetId", std::string());
    }
    Json attached;
    if (!cdp_.Call("Target.attachToTarget", Json{{"targetId", target}, {"flatten", true}}, "", attached, error,
                   cancel)) {
        return false;
    }
    page_ = attached.value("sessionId", std::string());
    if (page_.empty()) {
        error = "Страница не открылась";
        return false;
    }
    Json enabled;
    if (!Command("Page.enable", Json::object(), enabled, error, cancel)) {
        return false;
    }
    // Set before the first navigation. Without it no local page is refused, so a failure here refuses the browser.
    Json blocked;
    if (!Command("Network.setBlockedURLs", Json{{"urlPatterns", LocalPatterns()}}, blocked, error, cancel)) {
        error = "Не удалось закрыть браузер от сети: " + error;
        return false;
    }
    return true;
}

bool Session::Command(const std::string& method, const Json& params, Json& result, std::string& error,
                      const std::atomic<bool>& cancel) {
    if (cdp_.Call(method, params, page_, result, error, cancel)) {
        return true;
    }
    if (error.find("ession") != std::string::npos) {
        page_.clear();
    }
    return false;
}

std::string Session::PageState(bool withText, std::string& error, const std::atomic<bool>& cancel) {
    const std::string state = Evaluate(kPageStateJs, error, cancel);
    if (state.empty()) {
        return {};
    }
    const Json parsed = Json::parse(state, nullptr, false);
    if (parsed.is_discarded()) {
        return {};
    }
    std::string out = "Страница: " + parsed.value("t", std::string()) + "\nАдрес: " + parsed.value("u", std::string());
    if (withText) {
        out += "\n\n" + Cut(parsed.value("x", std::string()));
    }
    return out;
}

std::string Session::Evaluate(const std::string& expression, std::string& error, const std::atomic<bool>& cancel) {
    Json result;
    if (!Command("Runtime.evaluate", Json{{"expression", expression}, {"returnByValue", true}}, result, error,
                 cancel)) {
        return {};
    }
    if (result.contains("exceptionDetails")) {
        error = "Скрипт страницы упал";
        return {};
    }
    return result.value("result", Json::object()).value("value", std::string());
}

// After something that may have followed a link: a page on the local network is replaced by an empty one.
// True when that happened, or when the address could not be read (then the page is no proof of anything).
bool Session::BlockLocalPage(std::string& error, const std::atomic<bool>& cancel) {
    std::string readError;
    const std::string url = Evaluate("location.href", readError, cancel);
    if (readError.empty() && !LocalAddress(url)) {
        return false;
    }
    Json ignored;
    std::string navError;
    Command("Page.navigate", Json{{"url", "about:blank"}}, ignored, navError, cancel);
    error = readError.empty() ? "Страница ведёт на адрес внутри сети, браузер её закрыл" : readError;
    return true;
}

BrowserResult Session::Run(const BrowserRequest& request, const std::atomic<bool>& cancel) {
    std::lock_guard lock(mutex_);
    BrowserResult out;
    std::string& error = out.error;

    if (request.action == "close") {
        if (cdp_.Alive()) {
            Json ignored;
            std::string ignoredError;
            cdp_.Call("Browser.close", Json::object(), "", ignored, ignoredError, cancel, 5s);
        }
        const bool was = cdp_.Alive();
        cdp_.Stop();
        page_.clear();
        out.text = was ? "Браузер закрыт." : "Браузер не был открыт.";
        return out;
    }
    const bool known = request.action == "open" || request.action == "text" || request.action == "screenshot" ||
                       request.action == "click" || request.action == "type";
    if (!known) {
        error = "Неизвестное действие: " + request.action + " (open, text, screenshot, click, type, close)";
        return out;
    }
    if (request.action == "open" &&
        !(request.url.starts_with("http://") || request.url.starts_with("https://"))) {
        error = "Адрес должен начинаться с http:// или https://";
        return out;
    }
    if (request.action == "open" && LocalAddress(request.url)) {
        error = "Адрес внутри сети (этот компьютер или домашняя сеть) браузер не открывает";
        return out;
    }
    if (request.action == "click" && request.selector.empty() && request.text.empty()) {
        error = "Нужен selector (CSS) или text (видимый текст ссылки или кнопки)";
        return out;
    }
    if (request.action == "type" && request.selector.empty()) {
        error = "Нужен selector (CSS) поля";
        return out;
    }
    if (!Attach(error, cancel)) {
        return out;
    }

    if (request.action == "open") {
        cdp_.Forget("Page.loadEventFired", page_);
        Json nav;
        if (!Command("Page.navigate", Json{{"url", request.url}}, nav, error, cancel)) {
            return out;
        }
        if (nav.contains("errorText")) {
            error = nav.value("errorText", std::string("не открылся адрес"));
            return out;
        }
        cdp_.WaitEvent("Page.loadEventFired", page_, 30s, cancel);
        if (BlockLocalPage(error, cancel)) {
            return out;
        }
        out.text = PageState(true, error, cancel);
    } else if (request.action == "text") {
        out.text = PageState(true, error, cancel);
    } else if (request.action == "screenshot") {
        Json shot;
        if (!Command("Page.captureScreenshot", Json{{"format", "jpeg"}, {"quality", 80}}, shot, error, cancel)) {
            return out;
        }
        std::string data = shot.value("data", std::string());
        if (data.size() > kImageBytes) {
            if (!Command("Page.captureScreenshot", Json{{"format", "jpeg"}, {"quality", 50}}, shot, error, cancel)) {
                return out;
            }
            data = shot.value("data", std::string());
        }
        if (data.empty()) {
            error = "Снимок не получен";
            return out;
        }
        out.imageBase64 = data;
        out.imageType = "image/jpeg";
        out.text = "Снимок страницы.\n" + PageState(false, error, cancel);
    } else if (request.action == "click") {
        cdp_.Forget("Page.loadEventFired", page_);
        const std::string script = "(() => { const sel = " + Json(request.selector).dump() + "; const txt = " +
                                   Json(request.text).dump() + ";" + kClickBody + "})()";
        const std::string status = Evaluate(script, error, cancel);
        if (!error.empty()) {
            return out;
        }
        if (status != "OK") {
            error = request.selector.empty() ? "Нет ссылки или кнопки с текстом «" + request.text + "»"
                                             : "Нет элемента по селектору «" + request.selector + "»";
            return out;
        }
        cdp_.WaitEvent("Page.loadEventFired", page_, 2s, cancel);  // a link may open another page
        if (BlockLocalPage(error, cancel)) {
            return out;
        }
        out.text = "Нажато.\n" + PageState(true, error, cancel);
    } else {  // type
        const std::string script = "(() => { const el = document.querySelector(" + Json(request.selector).dump() +
                                   "); if (!el) return 'NOTFOUND'; el.focus();"
                                   " if (typeof el.select === 'function') el.select(); return 'OK'; })()";
        const std::string status = Evaluate(script, error, cancel);
        if (!error.empty()) {
            return out;
        }
        if (status != "OK") {
            error = "Нет поля по селектору «" + request.selector + "»";
            return out;
        }
        if (!request.text.empty()) {
            Json inserted;
            if (!Command("Input.insertText", Json{{"text", request.text}}, inserted, error, cancel)) {
                return out;
            }
        }
        if (request.key == "Enter") {
            cdp_.Forget("Page.loadEventFired", page_);
            Json down{{"type", "keyDown"}, {"key", "Enter"}, {"code", "Enter"}, {"windowsVirtualKeyCode", 13},
                      {"nativeVirtualKeyCode", 13}, {"text", "\r"}};
            Json up{{"type", "keyUp"}, {"key", "Enter"}, {"code", "Enter"}, {"windowsVirtualKeyCode", 13},
                    {"nativeVirtualKeyCode", 13}};
            Json ignored;
            if (!Command("Input.dispatchKeyEvent", down, ignored, error, cancel) ||
                !Command("Input.dispatchKeyEvent", up, ignored, error, cancel)) {
                return out;
            }
            cdp_.WaitEvent("Page.loadEventFired", page_, 3s, cancel);
            if (BlockLocalPage(error, cancel)) {
                return out;
            }
        }
        out.text = "Введено.\n" + PageState(true, error, cancel);
    }
    return out;
}

}  // namespace

BrowserResult RunBrowserAction(const BrowserRequest& request, const std::atomic<bool>& cancel) {
    // Never destroyed: the window's job kills it when the client exits, and no thread is joined on the way out.
    static Session* const session = new Session();
    return session->Run(request, cancel);
}

}  // namespace aic
