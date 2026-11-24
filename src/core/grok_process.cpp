#include "core/grok_process.h"

#include "core/auth.h"
#include "core/store.h"
#include "core/tools.h"

#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

namespace aic {
namespace {

constexpr size_t kStderrTail = 8192;
constexpr auto kMcpWait = std::chrono::seconds(15);  // how long a session waits for Grok to connect to our tools
constexpr int64_t kTicksPerDollar = 10'000'000'000;  // costUsdTicks

namespace fs = std::filesystem;

std::string RandomToken() {
    std::random_device device;
    std::string token;
    for (int i = 0; i < 4; ++i) {
        char part[9];
        snprintf(part, sizeof(part), "%08x", device());
        token += part;
    }
    return token;
}

// What Grok may use. A YAML list: one comma-separated line is a single string, and grok.exe then ignores it and
// sends every built-in tool (measured 09.11.2026: 27 schemas, video and workflow the largest). The names are the
// ones the model is shown. Left out: image_to_video, reference_to_video, workflow, the schedulers, monitor, plan
// mode and ask_user_question. disallowedTools strips those even if the allowlist is not applied.
// Native read_file, grep, list_dir and web_search run inside grok.exe before can_use_tool, so they are disallowed
// too: file reads go through mcp__aic__read_file and mcp__aic__list_directory, and the web through web_fetch.
// search_replace is disallowed with them. Without a Read tool in the toolset grok 1.0.77 refuses to build the agent
// ("skip_read_before_edit=false requires a Read tool"): session/new answers a bare "Internal error" and every chat
// on 0.1.120 failed at its first message (16.11.2026, journal; reproduced with grok agent stdio). It is not enough to
// leave it out of the allowlist: the allowlist is not applied, only disallowedTools removes it. Edits go through
// mcp__aic__edit_file, which stays inside the chat's project.
// A helper (02.11.2026, his: "субагента себе сделай") is the only type spawn_subagent can name, from the plugin
// written beside this profile. The hand has no shell and cannot start another agent.
constexpr const char* kProfile =
    "---\n"
    "name: aic\n"
    "description: AIControls chat\n"
    "tools:\n"
    "  - write\n"
    "  - run_terminal_command\n"
    "  - web_fetch\n"
    "  - search_tool\n"
    "  - use_tool\n"
    "  - todo_write\n"
    "  - spawn_subagent\n"
    "  - get_command_or_subagent_output\n"
    "  - wait_commands_or_subagents\n"
    "  - kill_command_or_subagent\n"
    "  - image_gen\n"
    "  - image_edit\n"
    "disallowedTools:\n"
    "  - image_to_video\n"
    "  - reference_to_video\n"
    "  - workflow\n"
    "  - scheduler_create\n"
    "  - scheduler_delete\n"
    "  - scheduler_list\n"
    "  - monitor\n"
    "  - enter_plan_mode\n"
    "  - exit_plan_mode\n"
    "  - ask_user_question\n"
    "  - search_replace\n"
    "  - read_file\n"
    "  - grep\n"
    "  - list_dir\n"
    "  - web_search\n"
    "---\n";

constexpr const char* kHand =
    "---\n"
    "name: hand\n"
    "description: Helper for this AIControls chat. Files, commands and writes go through search_tool and use_tool.\n"
    "tools:\n"
    "  - search_tool\n"
    "  - use_tool\n"
    "  - web_fetch\n"
    "  - image_gen\n"
    "  - image_edit\n"
    "disallowedTools:\n"
    "  - image_to_video\n"
    "  - reference_to_video\n"
    "  - run_terminal_command\n"
    "  - spawn_subagent\n"
    "  - read_file\n"
    "  - grep\n"
    "  - list_dir\n"
    "  - web_search\n"
    "mcpInheritance: all\n"
    "---\n"
    "\n"
    "You are a helper inside an AIControls chat. To read or list files, search_tool then use_tool.\n"
    "Commands, writes and servers go through search_tool and use_tool only. Those calls wait for the user.\n"
    "Report a short result to the parent. Do not spawn agents.\n";

constexpr const char* kPluginJson = "{\"name\":\"aicontrols\"}\n";

struct GrokLaunchFiles {
    std::wstring profile;
    std::wstring plugin;
};

void WriteText(const fs::path& file, const char* text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
        throw std::runtime_error("could not write " + ToUtf8(file.wstring()));
    }
}

GrokLaunchFiles WriteGrokFiles() {
    std::array<wchar_t, 2048> buffer{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    fs::path dir = length > 0 && length < buffer.size() ? fs::path(std::wstring(buffer.data(), length)) : fs::temp_directory_path();
    dir /= L"AIControls";
    const fs::path plugin = dir / L"aicontrols";
    std::error_code error;
    fs::create_directories(plugin / L"agents", error);
    if (error) {
        throw std::runtime_error("could not create the Grok plugin folder");
    }
    WriteText(dir / L"grok-agent.md", kProfile);
    WriteText(plugin / L"plugin.json", kPluginJson);
    WriteText(plugin / L"agents" / L"hand.md", kHand);
    return {(dir / L"grok-agent.md").wstring(), plugin.wstring()};
}

bool SpawnTool(const std::string& name) {
    return name == "spawn_subagent" || name == "get_command_or_subagent_output" ||
           name == "wait_commands_or_subagents" || name == "kill_command_or_subagent";
}

Json RpcError(const Json& id, int code, const char* message) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

// The text of a finished tool call, whatever shape Grok wraps it in. A picture or a video it made stays JSON: the
// window takes the file's "path" from it, as from the history.
std::string OutputText(const Json& rawOutput) {
    if (JsonStr(rawOutput, "type") == "WebFetch") {
        return JsonStr(JsonAt(rawOutput, "Content"), "content");
    }
    const Json& output = JsonAt(rawOutput, "output");
    for (const char* key : {"OkayOutput", "ErrorOutput", "Output"}) {
        if (output.is_object() && output.contains(key)) {
            const Json& inner = output[key];
            return inner.is_string() ? inner.get<std::string>()
                                     : inner.dump(-1, ' ', false, Json::error_handler_t::replace);
        }
    }
    if (output.is_string()) {
        return output.get<std::string>();
    }
    if (rawOutput.is_string()) {
        return rawOutput.get<std::string>();
    }
    return rawOutput.is_null() ? std::string() : rawOutput.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// read_file and grep announce themselves on the first update that names them. A later update often keeps only
// the id and a title like "Read `path`".
std::string NativeToolName(const Json& update) {
    const std::string named = JsonStr(JsonAt(JsonAt(update, "_meta"), "x.ai/tool"), "name");
    if (GrokNativeRead(named)) {
        return named;
    }
    if (const std::string title = JsonStr(update, "title"); GrokNativeRead(title)) {
        return title;
    }
    const std::string variant = JsonStr(JsonAt(update, "rawInput"), "variant");
    if (variant == "ReadFile") {
        return "read_file";
    }
    if (variant == "Grep") {
        return "grep";
    }
    return {};
}

Json NativeReadInput(const Json& update) {
    Json input = JsonAt(update, "rawInput");
    if (!input.is_object()) {
        input = Json::object();
    }
    const Json& extra = JsonAt(JsonAt(JsonAt(update, "_meta"), "x.ai/tool"), "input");
    if (!extra.is_object()) {
        return input;
    }
    for (const auto& [key, value] : extra.items()) {
        if (!input.contains(key) || input[key].is_null()) {
            input[key] = value;
        }
    }
    return input;
}

// A JSON-RPC error's message with its data. grok answers a failed session/new with the bare message "Internal error"
// and puts the reason in data ("session initialization failed: tool error: Requirements unsatisfied: ..."): on
// 16.11.2026 the journal said only "Grok: Internal error" seven times and the cause was found by running grok by hand.
std::string ErrorText(const Json& error) {
    std::string text = JsonStr(error, "message");
    const Json& data = JsonAt(error, "data");
    std::string detail = data.is_string() ? data.get<std::string>()
                         : data.is_null() ? std::string()
                                          : data.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (detail.size() > 600) {
        detail.resize(600);
        detail += "...";
    }
    return detail.empty() || detail == text ? text : text + ": " + detail;
}

long long Number(const Json& object, const char* key) {
    const Json& value = JsonAt(object, key);
    return value.is_number() ? value.get<long long>() : 0;
}

}  // namespace

GrokProcess::~GrokProcess() {
    Stop();
}

void GrokProcess::Start(const ClaudeOptions& options, MessageHandler onMessage, ExitHandler onExit) {
    Stop();
    stopping_ = false;
    options_ = options;
    onMessage_ = std::move(onMessage);
    onExit_ = std::move(onExit);
    {
        std::lock_guard lock(stderrMutex_);
        stderrTail_.clear();
    }
    {
        std::lock_guard lock(stateMutex_);
        nextId_ = 0;
        calls_.clear();
        sessionId_.clear();
        sessionOpen_ = false;
        mcpReady_ = false;
        turnActive_ = false;
        queue_.clear();
        pendingModel_.clear();
        textOpen_ = false;
        permissions_.clear();
        nativeReads_.clear();
        refusals_.clear();
        mcp_.clear();
    }

    const std::wstring exe = options.exe.empty() ? FindGrokExe() : options.exe;
    if (exe.empty()) {
        throw std::runtime_error("grok.exe not found");
    }
    token_ = RandomToken();
    const GrokLaunchFiles files = WriteGrokFiles();
    profilePath_ = files.profile;
    serverPort_ = server_.Start(token_, [this](const std::string& body) { return HandleMcpRequest(body); });

    std::wstring cmd = QuoteArg(exe) + L" agent --no-leader --agent-profile " + QuoteArg(profilePath_) +
                       L" --plugin-dir " + QuoteArg(files.plugin);
    if (!options.model.empty()) {
        cmd += L" --model " + QuoteArg(ToWide(options.model));
    }
    // An empty effort is the chip "Auto". grok.exe with no --effort reasons at high (measured on grok-4.7,
    // 09.11.2026), which is most of a turn's time. Auto for this client is low; a chosen level is sent as it is.
    cmd += L" --effort " + QuoteArg(ToWide(options.effort.empty() ? "low" : options.effort));
    cmd += L" stdio";

    // web_fetch is off in grok.exe unless asked for.
    // Compact at 40% of the 256k window (~100k tokens), not the CLI's 85% (~205k). Measured 09.11.2026: a call
    // stays near 6–12 s through ~55k tokens and is 15–21 s by 76–90k, and a week of chats spent most of its
    // prompt mass above 80k. Claude's 150k cap would have cut only ~7% of that. grok 1.0.76 reads this variable
    // (0–100; anything else is ignored). The model's real window stays 256k.
    std::vector<std::pair<std::wstring, std::wstring>> overrides = {{L"NO_COLOR", L"1"},
                                                                    {L"GROK_WEB_FETCH", L"1"},
                                                                    {L"GROK_AUTO_COMPACT_THRESHOLD_PERCENT", L"40"}};
    if (!options.proxy.empty()) {
        overrides.insert(overrides.end(), {{L"HTTPS_PROXY", options.proxy},
                                           {L"HTTP_PROXY", options.proxy},
                                           {L"NO_PROXY", L"127.0.0.1,localhost,::1"}});
    }
    SpawnedProcess spawned;
    try {
        spawned = SpawnPiped(exe, std::move(cmd), overrides, options.cwd);
    } catch (...) {
        server_.Stop();
        throw;
    }
    process_ = std::move(spawned.process);
    job_ = std::move(spawned.job);
    stdoutRead_ = std::move(spawned.stdoutRead);
    stderrRead_ = std::move(spawned.stderrRead);
    {
        std::lock_guard lock(writeMutex_);
        stdinWrite_ = std::move(spawned.stdinWrite);
    }
    errorReader_ = std::thread(&GrokProcess::ReadErrors, this);
    reader_ = std::thread(&GrokProcess::ReadLoop, this);
    watchdog_ = std::thread(&GrokProcess::Watchdog, this);

    Call("initialize",
         Json{{"protocolVersion", 1},
              {"clientCapabilities", {{"terminal", false}, {"fs", {{"readTextFile", false}, {"writeTextFile", false}}}}}},
         [this](const Json& result, const Json& error) {
             if (!error.is_null()) {
                 Emit(Json{{"type", "result"},
                           {"subtype", "error_during_execution"},
                           {"is_error", true},
                           {"result", "Grok: " + ErrorText(error)}});
                 return;
             }
             OpenSession(result);
         });
}

void GrokProcess::Stop() {
    stopping_ = true;
    {
        std::lock_guard lock(stateMutex_);
        for (auto& [id, wait] : mcp_) {
            wait.done = true;  // the HTTP handlers return, so the server can close
        }
        queue_.clear();
    }
    stateChanged_.notify_all();
    {
        std::lock_guard lock(writeMutex_);
        stdinWrite_.reset();  // the agent's cue to finish
    }
    if (process_) {
        WaitForSingleObject(process_.get(), 800);
        TerminateJobObject(job_.get(), 1);
    }
    server_.Stop();
    if (reader_.joinable()) {
        reader_.join();
    }
    if (watchdog_.joinable()) {
        watchdog_.join();
    }
    job_.reset();
    process_.reset();
    stdoutRead_.reset();
    stderrRead_.reset();
}

void GrokProcess::SendUserText(std::string_view text) {
    SendUserContent(Json(std::string(text)));
}

void GrokProcess::SendUserContent(Json content) {
    // Claude's content blocks as ACP's.
    Json blocks = Json::array();
    if (content.is_string()) {
        blocks.push_back(Json{{"type", "text"}, {"text", content.get<std::string>()}});
    } else {
        for (const Json& block : content) {
            const std::string type = JsonStr(block, "type");
            const Json& source = JsonAt(block, "source");
            if (type == "image") {
                blocks.push_back(Json{{"type", "image"},
                                      {"data", JsonStr(source, "data")},
                                      {"mimeType", JsonStr(source, "media_type")}});
            } else if (type == "document") {
                blocks.push_back(Json{{"type", "resource"},
                                      {"resource",
                                       {{"uri", "file:///" + JsonStr(block, "title")},
                                        {"mimeType", JsonStr(source, "media_type")},
                                        {"blob", JsonStr(source, "data")}}}});
            } else {
                blocks.push_back(block);
            }
        }
    }
    {
        std::lock_guard lock(stateMutex_);
        queue_.push_back(std::move(blocks));
    }
    Pump();
}

void GrokProcess::Interrupt() {
    std::string session;
    {
        std::lock_guard lock(stateMutex_);
        queue_.clear();  // what waited for its turn goes with the one that is stopped
        session = sessionId_;
    }
    if (!session.empty()) {
        Notify("session/cancel", Json{{"sessionId", session}});
    }
}

void GrokProcess::SetModel(const std::string& model) {
    std::string session;
    {
        std::lock_guard lock(stateMutex_);
        if (!sessionOpen_) {
            pendingModel_ = model;
            return;
        }
        session = sessionId_;
    }
    Call("session/set_config_option",
         Json{{"sessionId", session}, {"configId", "model"}, {"value", {{"value", model}}}},
         [](const Json&, const Json&) {});
}

void GrokProcess::Respond(const std::string& requestId, Json response) {
    if (requestId.starts_with("mcp_")) {
        {
            std::lock_guard lock(stateMutex_);
            if (const auto it = mcp_.find(requestId); it != mcp_.end()) {
                it->second.reply = JsonAt(response, "mcp_response").dump(-1, ' ', false, Json::error_handler_t::replace);
                it->second.done = true;
            }
        }
        stateChanged_.notify_all();
        return;
    }
    Json entry;
    {
        std::lock_guard lock(stateMutex_);
        const auto it = permissions_.find(requestId);
        if (it == permissions_.end()) {
            return;
        }
        entry = std::move(it->second);
        permissions_.erase(it);
    }
    const bool allow = JsonStr(response, "behavior") == "allow";
    const bool stopped = JsonAt(response, "interrupt").is_boolean() && JsonAt(response, "interrupt").get<bool>();
    // A "no" to one of our tools is let through and answered by our MCP server with the reason: refused there,
    // Grok cancels every other call of the step and ends the turn without a word (28.10.2026, his: "оборвалось").
    const std::string tool = JsonStr(entry, "tool");
    const bool refuseInside = !allow && !stopped && !tool.empty();
    if (refuseInside) {
        std::string message = JsonStr(response, "message");
        std::lock_guard lock(stateMutex_);
        refusals_.push_back(Refusal{tool, JsonAt(entry, "input"),
                                    message.empty() ? std::string("The user declined this call.") : std::move(message)});
    }
    const char* wanted = allow || refuseInside ? "allow_once" : "reject_once";
    std::string optionId;
    for (const Json& option : JsonAt(entry, "options")) {
        if (JsonStr(option, "kind") == wanted) {
            optionId = JsonStr(option, "optionId");
        }
    }
    if (optionId.empty() || stopped) {  // ACP's answer to a prompt left when the turn is cancelled
        Reply(JsonAt(entry, "id"), Json{{"outcome", {{"outcome", "cancelled"}}}});
        return;
    }
    Reply(JsonAt(entry, "id"), Json{{"outcome", {{"outcome", "selected"}, {"optionId", optionId}}}});
}

void GrokProcess::RespondError(const std::string& requestId, std::string_view error) {
    if (requestId.starts_with("mcp_")) {
        {
            std::lock_guard lock(stateMutex_);
            if (const auto it = mcp_.find(requestId); it != mcp_.end()) {
                it->second.reply = RpcError(nullptr, -32603, std::string(error).c_str()).dump();
                it->second.done = true;
            }
        }
        stateChanged_.notify_all();
        return;
    }
    Respond(requestId, Json{{"behavior", "deny"}});
}

void GrokProcess::RequestUsage() {
    {
        std::lock_guard lock(stateMutex_);
        if (!sessionOpen_) {
            return;
        }
    }
    Call("_x.ai/billing", Json::object(), [this](const Json& result, const Json& error) {
        if (error.is_null() && result.is_object()) {
            Emit(Json{{"type", "aic_usage"}, {"billing", result}});
        }
    });
}

// ---- JSON-RPC out ----

void GrokProcess::Call(const char* method, Json params, Callback done) {
    int64_t id = 0;
    {
        std::lock_guard lock(stateMutex_);
        id = ++nextId_;
        calls_[id] = std::move(done);
    }
    WriteLine(Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}});
}

void GrokProcess::Notify(const char* method, Json params) {
    WriteLine(Json{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
}

void GrokProcess::Reply(const Json& id, Json result) {
    WriteLine(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

void GrokProcess::WriteLine(const Json& message) {
    const std::string line = message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
    std::lock_guard lock(writeMutex_);
    if (!stdinWrite_) {
        return;
    }
    const char* data = line.data();
    size_t left = line.size();
    while (left > 0) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(std::min<size_t>(left, 1 << 20));
        if (!WriteFile(stdinWrite_.get(), data, chunk, &written, nullptr)) {
            return;  // the agent is gone; the reader reports the exit
        }
        data += written;
        left -= written;
    }
}

// ---- in ----

void GrokProcess::ReadLoop() {
    std::string buffer;
    std::vector<char> chunk(16384);
    DWORD got = 0;
    while (ReadFile(stdoutRead_.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) && got > 0) {
        buffer.append(chunk.data(), got);
        size_t start = 0;
        for (size_t newline = buffer.find('\n'); newline != std::string::npos;
             newline = buffer.find('\n', start)) {
            std::string_view line(buffer.data() + start, newline - start);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (!line.empty()) {
                HandleLine(line);
            }
            start = newline + 1;
        }
        buffer.erase(0, start);
    }

    DWORD code = STILL_ACTIVE;
    if (WaitForSingleObject(process_.get(), 3000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(process_.get(), &code);
    } else {
        TerminateJobObject(job_.get(), 1);
    }
    if (errorReader_.joinable()) {
        errorReader_.join();
    }
    std::string tail;
    {
        std::lock_guard lock(stderrMutex_);
        tail = stderrTail_;
    }
    if (!stopping_ && onExit_) {
        onExit_(code, std::move(tail));
    }
}

void GrokProcess::ReadErrors() {
    std::array<char, 4096> chunk{};
    DWORD got = 0;
    while (ReadFile(stderrRead_.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) && got > 0) {
        std::lock_guard lock(stderrMutex_);
        stderrTail_.append(chunk.data(), got);
        if (stderrTail_.size() > kStderrTail) {
            stderrTail_.erase(0, stderrTail_.size() - kStderrTail);
        }
    }
}

void GrokProcess::HandleLine(std::string_view line) {
    const Json message = Json::parse(line, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        Emit(Json{{"type", "aic_raw"}, {"text", std::string(line)}});
        return;
    }
    const std::string method = JsonStr(message, "method");
    if (method.empty()) {
        OnResponse(message);
    } else if (message.contains("id")) {
        OnRequest(message["id"], method, JsonAt(message, "params"));
    } else {
        OnNotification(method, JsonAt(message, "params"));
    }
}

void GrokProcess::OnResponse(const Json& message) {
    const Json& id = JsonAt(message, "id");
    if (!id.is_number_integer()) {
        return;
    }
    Callback done;
    {
        std::lock_guard lock(stateMutex_);
        const auto it = calls_.find(id.get<int64_t>());
        if (it == calls_.end()) {
            return;
        }
        done = std::move(it->second);
        calls_.erase(it);
    }
    if (done) {
        done(JsonAt(message, "result"), message.contains("error") ? message["error"] : Json());
    }
}

void GrokProcess::OnNotification(const std::string& method, const Json& params) {
    if (method == "session/update") {
        OnUpdate(JsonAt(params, "update"));
    } else if (method == "_x.ai/session/update") {
        // The real reason of a failed turn ("API error (status 402 Payment Required): Grok Build usage balance
        // exhausted") comes here; the prompt's own error is only "Internal error" (04.11.2026, his journal).
        // Auto-compact is the same channel (SessionUpdate), not session/update.
        const Json& update = JsonAt(params, "update");
        const std::string kind = JsonStr(update, "sessionUpdate");
        if (kind.starts_with("auto_compact")) {
            OnUpdate(update);
        } else if (kind == "retry_state" && JsonStr(update, "type") == "failed") {
            std::lock_guard lock(stateMutex_);
            apiError_ = JsonStr(update, "message");
        }
    } else if (method == "_x.ai/mcp_initialized") {
        MarkReady();
    }
}

void GrokProcess::OnRequest(const Json& id, const std::string& method, const Json& params) {
    if (method != "session/request_permission") {
        WriteLine(RpcError(id, -32601, "Method not found"));
        return;
    }
    const Json& call = JsonAt(params, "toolCall");
    const Json& raw = JsonAt(call, "rawInput");
    std::string name;
    Json input;
    std::string ours;  // our MCP server's name of the tool, when it is one of ours
    const std::string own = JsonStr(JsonAt(JsonAt(call, "_meta"), "x.ai/tool"), "name");
    if (SpawnTool(own)) {
        // The helper's own tools still arrive as their own requests. This one only starts, watches or stops it.
        std::string optionId;
        for (const Json& option : JsonAt(params, "options")) {
            if (JsonStr(option, "kind") == "allow_once") {
                optionId = JsonStr(option, "optionId");
            }
        }
        if (optionId.empty()) {
            Reply(id, Json{{"outcome", {{"outcome", "cancelled"}}}});
        } else {
            Reply(id, Json{{"outcome", {{"outcome", "selected"}, {"optionId", optionId}}}});
        }
        if (own == "spawn_subagent" || own == "kill_command_or_subagent") {
            Emit(Json{{"type", "aic_tool"}, {"name", own}, {"input", raw}});
        }
        return;
    }
    if (JsonStr(raw, "variant") == "UseTool") {
        const std::string tool = JsonStr(raw, "tool_name");
        name = "mcp__" + tool;  // aic__run_command -> mcp__aic__run_command
        input = JsonAt(raw, "tool_input");
        if (const std::string prefix = std::string(kToolServer) + "__"; tool.starts_with(prefix)) {
            ours = tool.substr(prefix.size());
        }
    } else if (!own.empty()) {
        // Its own file edits and shell too, not only the web and Imagine. Refusing them as "not ours" was
        // reject_once, and that cancels every other call of the step (28.10.2026, his: "оборвалось").
        name = std::string(kGrokToolPrefix) + own;
        input = raw;
    } else {
        name = JsonStr(call, "title");
        input = raw;
        if (GrokClientTool(name)) {
            name = std::string(kGrokToolPrefix) + name;
        }
    }
    std::string requestId;
    {
        std::lock_guard lock(stateMutex_);
        requestId = "perm_" + std::to_string(++permissionCounter_);
        permissions_[requestId] = Json{{"id", id}, {"options", JsonAt(params, "options")}, {"tool", ours}, {"input", input}};
        textOpen_ = false;
    }
    Emit(Json{{"type", "control_request"},
              {"request_id", requestId},
              {"request",
               {{"subtype", "can_use_tool"},
                {"tool_name", name},
                {"input", input},
                {"tool_use_id", JsonStr(call, "toolCallId")}}}});
}

void GrokProcess::OnUpdate(const Json& update) {
    const std::string type = JsonStr(update, "sessionUpdate");
    if (type == "auto_compact_started") {
        // The window already says this for Claude's --autocompact.
        Emit(Json{{"type", "system"}, {"subtype", "status"}, {"status", "compacting"}});
        return;
    }
    if (type == "auto_compact_completed") {
        Json meta = Json::object();
        auto tokens = [](const Json& value) -> long long {
            if (value.is_number_integer()) {
                return value.get<long long>();
            }
            if (value.is_number_unsigned()) {
                return static_cast<long long>(value.get<unsigned long long>());
            }
            if (value.is_number_float()) {
                return static_cast<long long>(value.get<double>());
            }
            return -1;
        };
        const long long before = tokens(JsonAt(update, "tokens_before"));
        const long long after = tokens(JsonAt(update, "tokens_after"));
        if (before >= 0) {
            meta["pre_tokens"] = before;
        }
        if (after >= 0) {
            meta["post_tokens"] = after;
        }
        Emit(Json{{"type", "system"}, {"subtype", "compact_boundary"}, {"compact_metadata", std::move(meta)}});
        return;
    }
    if (type == "auto_compact_failed" || type == "auto_compact_cancelled") {
        Emit(Json{{"type", "system"}, {"subtype", "status"}, {"status", "compact_failed"}});
        return;
    }
    if (type == "agent_message_chunk") {
        const Json& content = JsonAt(update, "content");
        if (JsonStr(content, "type") == "text") {
            EmitText(JsonStr(content, "text"));
        }
        return;
    }
    if (type != "tool_call" && type != "tool_call_update") {
        return;
    }
    const std::string id = JsonStr(update, "toolCallId");
    const std::string own = NativeToolName(update);
    bool show = false;
    bool native = false;
    {
        std::lock_guard lock(stateMutex_);
        textOpen_ = false;  // what is said after the call is a new message
        const auto known = std::ranges::find(nativeReads_, id);
        native = GrokNativeRead(own) || (!id.empty() && known != nativeReads_.end());
        if (GrokNativeRead(own) && (id.empty() ? type == "tool_call" : known == nativeReads_.end())) {
            if (!id.empty()) {
                nativeReads_.push_back(id);
            }
            show = true;
        }
        if (native && type == "tool_call_update" && !id.empty()) {
            const std::string status = JsonStr(update, "status");
            if (status == "completed" || status == "failed") {
                std::erase(nativeReads_, id);
            }
        }
    }
    if (show) {  // one line, when the call is named; the result has no card to land on
        Emit(Json{{"type", "aic_tool"}, {"name", own}, {"input", NativeReadInput(update)}});
    }
    if (native || type != "tool_call_update") {
        return;
    }
    const std::string status = JsonStr(update, "status");
    const Json& action = JsonAt(JsonAt(update, "rawOutput"), "action");
    if (JsonStr(action, "type") == "search") {  // a web search, done on xAI's side: no card, a line in the feed
        if (status == "completed") {
            Emit(Json{{"type", "aic_tool"}, {"name", "web_search"}, {"input", action}});
        }
    } else if (status == "completed" || status == "failed") {
        EmitToolResult(update);
    }
}

// ---- the handshake and the turns ----

void GrokProcess::OpenSession(const Json& initResult) {
    // The models, in the shape the window reads Claude's.
    Json models = Json::array();
    for (const Json& model : JsonAt(JsonAt(JsonAt(initResult, "_meta"), "modelState"), "availableModels")) {
        Json efforts = Json::array();
        for (const Json& effort : JsonAt(JsonAt(model, "_meta"), "reasoningEfforts")) {
            efforts.push_back(JsonStr(effort, "value"));
        }
        const std::string description = JsonStr(model, "description");
        models.push_back(Json{{"value", JsonStr(model, "modelId")},
                              {"displayName", JsonStr(model, "name")},
                              {"description", description},
                              {"supportedEffortLevels", std::move(efforts)}});
    }
    Emit(Json{{"type", "control_response"},
              {"response",
               {{"subtype", "success"},
                {"request_id", "aic_init"},
                {"response", {{"models", std::move(models)}, {"account", Json::object()}}}}}});

    const std::string cwd = options_.cwd.empty() ? ToUtf8(fs::current_path().wstring()) : ToUtf8(options_.cwd);
    const Json servers = Json::array(
        {Json{{"type", "http"},
              {"name", kToolServer},
              {"url", "http://127.0.0.1:" + std::to_string(serverPort_) + "/mcp"},
              {"headers", Json::array({Json{{"name", "Authorization"}, {"value", "Bearer " + token_}}})}}});
    Json meta = Json::object();
    if (!options_.appendSystemPrompt.empty()) {
        meta["rules"] = options_.appendSystemPrompt;
    }

    const auto opened = [this](const std::string& session) {
        std::string model;
        {
            std::lock_guard lock(stateMutex_);
            sessionId_ = session;
            sessionOpen_ = true;
            model = std::move(pendingModel_);
            pendingModel_.clear();
        }
        Emit(Json{{"type", "system"}, {"subtype", "init"}, {"session_id", session}});
        RequestUsage();  // the ring is right before the first message
        if (!model.empty()) {
            SetModel(model);
        }
        Pump();
    };
    const auto fresh = [this, cwd, servers, meta, opened] {
        Call("session/new", Json{{"cwd", cwd}, {"mcpServers", servers}, {"_meta", meta}},
             [this, opened](const Json& result, const Json& error) {
                 if (!error.is_null() || JsonStr(result, "sessionId").empty()) {
                     Emit(Json{{"type", "result"},
                               {"subtype", "error_during_execution"},
                               {"is_error", true},
                               {"result", "Grok: " + (error.is_null() ? std::string("no session") : ErrorText(error))}});
                     return;
                 }
                 opened(JsonStr(result, "sessionId"));
             });
    };
    if (options_.resume.empty()) {
        fresh();
        return;
    }
    Call("session/resume", Json{{"sessionId", options_.resume}, {"cwd", cwd}, {"mcpServers", servers}, {"_meta", meta}},
         [this, fresh, opened](const Json& /*result*/, const Json& error) {
             if (!error.is_null()) {
                 Emit(Json{{"type", "aic_raw"},
                           {"text", "Grok не смог продолжить прошлую сессию (" + JsonStr(error, "message") +
                                        "): начат новый чат."}});
                 fresh();
                 return;
             }
             opened(options_.resume);
         });
}

void GrokProcess::MarkReady() {
    {
        std::lock_guard lock(stateMutex_);
        mcpReady_ = true;
    }
    stateChanged_.notify_all();
    Pump();
}

void GrokProcess::Watchdog() {
    std::unique_lock lock(stateMutex_);
    const bool ready = stateChanged_.wait_for(lock, kMcpWait, [this] { return mcpReady_ || stopping_; });
    lock.unlock();
    if (!ready) {
        MarkReady();  // Grok never said it connected: ask anyway, a tool call will say what is wrong
    }
}

void GrokProcess::Pump() {
    Json prompt;
    std::string session;
    {
        std::lock_guard lock(stateMutex_);
        if (!sessionOpen_ || !mcpReady_ || turnActive_ || queue_.empty()) {
            return;
        }
        prompt = std::move(queue_.front());
        queue_.pop_front();
        turnActive_ = true;
        textOpen_ = false;
        apiError_.clear();
        session = sessionId_;
    }
    EmitState("running");
    Call("session/prompt", Json{{"sessionId", session}, {"prompt", std::move(prompt)}},
         [this](const Json& result, const Json& error) { FinishTurn(result, error); });
}

void GrokProcess::FinishTurn(const Json& result, const Json& error) {
    bool more = false;
    std::string apiError;
    {
        std::lock_guard lock(stateMutex_);
        apiError = std::move(apiError_);
        apiError_.clear();
        turnActive_ = false;
        textOpen_ = false;
        refusals_.clear();  // a "no" whose call never came: not carried to the next turn
        more = !queue_.empty();
    }
    Json out{{"type", "result"}, {"subtype", "success"}, {"is_error", false}};
    if (!error.is_null()) {
        out["subtype"] = "error_during_execution";
        out["is_error"] = true;
        out["result"] = apiError.empty() ? ErrorText(error) : "Grok: " + apiError;
    } else {
        // The last request of the turn is the chat's size now ("Auto" and the context ring read it).
        const Json& meta = JsonAt(result, "_meta");
        const long long input = Number(meta, "inputTokens");
        const long long cached = Number(meta, "cachedReadTokens");
        out["usage"] = Json{{"input_tokens", std::max(0LL, input - cached)},
                            {"cache_read_input_tokens", cached},
                            {"cache_creation_input_tokens", 0},
                            {"output_tokens", Number(meta, "outputTokens")}};
        const Json& ticks = JsonAt(JsonAt(meta, "usage"), "costUsdTicks");
        if (ticks.is_number()) {
            out["total_cost_usd"] = ticks.get<double>() / static_cast<double>(kTicksPerDollar);
        }
    }
    Emit(std::move(out));
    RequestUsage();  // the turn spent some
    if (more) {
        Pump();
    } else {
        EmitState("idle");
    }
}

void GrokProcess::Emit(Json message) {
    if (!stopping_ && onMessage_) {
        onMessage_(std::move(message));
    }
}

void GrokProcess::EmitState(const char* state) {
    Emit(Json{{"type", "system"}, {"subtype", "session_state_changed"}, {"state", state}});
}

void GrokProcess::EmitText(const std::string& text) {
    if (text.empty()) {
        return;
    }
    bool opening = false;
    {
        std::lock_guard lock(stateMutex_);
        if (!textOpen_) {
            textOpen_ = true;
            opening = true;
            messageId_ = "grok_" + std::to_string(++messageCounter_);
        }
    }
    if (opening) {
        Emit(Json{{"type", "stream_event"}, {"event", {{"type", "message_start"}, {"message", {{"id", messageId_}}}}}});
        Emit(Json{{"type", "stream_event"},
                  {"event",
                   {{"type", "content_block_start"}, {"index", 0}, {"content_block", {{"type", "text"}, {"text", ""}}}}}});
    }
    Emit(Json{{"type", "stream_event"},
              {"event",
               {{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", "text_delta"}, {"text", text}}}}}});
}

void GrokProcess::EmitToolResult(const Json& update) {
    const std::string text = OutputText(JsonAt(update, "rawOutput"));
    Emit(Json{{"type", "user"},
              {"message",
               {{"role", "user"},
                {"content",
                 Json::array({Json{{"type", "tool_result"},
                                   {"tool_use_id", JsonStr(update, "toolCallId")},
                                   {"content", Json::array({Json{{"type", "text"}, {"text", text}}})},
                                   {"is_error", JsonStr(update, "status") == "failed"}}})}}}});
}

// Grok's requests to our MCP server, on a connection's thread: handed to the window as Claude's mcp_message and
// held until ToolHost has the answer.
std::string GrokProcess::HandleMcpRequest(const std::string& body) {
    const Json request = Json::parse(body, nullptr, false);
    if (request.is_discarded() || !request.is_object()) {
        return RpcError(nullptr, -32700, "Parse error").dump();
    }
    if (!request.contains("id")) {
        return {};  // a notification
    }
    const Json& id = request["id"];
    if (JsonStr(request, "method") == "server/discover") {
        return RpcError(id, -32601, "Method not found").dump();  // Grok falls back to initialize
    }
    std::string requestId;
    {
        std::lock_guard lock(stateMutex_);
        if (JsonStr(request, "method") == "tools/call" && !refusals_.empty()) {
            // A call the user said no to: the same tool with the same input, or, failing that, the same tool
            // (when in doubt it is refused, never run).
            const Json& params = JsonAt(request, "params");
            const std::string tool = JsonStr(params, "name");
            auto it = std::ranges::find_if(refusals_, [&](const Refusal& refusal) {
                return refusal.tool == tool && refusal.input == JsonAt(params, "arguments");
            });
            if (it == refusals_.end()) {
                it = std::ranges::find_if(refusals_, [&](const Refusal& refusal) { return refusal.tool == tool; });
            }
            if (it != refusals_.end()) {
                Json result{{"content", Json::array({Json{{"type", "text"}, {"text", it->message}}})}, {"isError", true}};
                refusals_.erase(it);
                return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}.dump(
                    -1, ' ', false, Json::error_handler_t::replace);
            }
        }
        requestId = "mcp_" + std::to_string(++mcpCounter_);
        mcp_[requestId] = McpWait{};
    }
    Emit(Json{{"type", "control_request"},
              {"request_id", requestId},
              {"request", {{"subtype", "mcp_message"}, {"server_name", kToolServer}, {"message", request}}}});
    std::string reply;
    {
        std::unique_lock lock(stateMutex_);
        stateChanged_.wait(lock, [&] { return mcp_[requestId].done || stopping_; });
        reply = std::move(mcp_[requestId].reply);
        mcp_.erase(requestId);
    }
    return reply.empty() ? RpcError(id, -32603, "The client is closing").dump() : reply;
}

std::wstring FindGrokExe() {
    std::array<wchar_t, 2048> buffer{};
    const auto size = static_cast<DWORD>(buffer.size());
    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer.data(), size);
    if (length > 0 && length < size) {
        std::wstring candidate = std::wstring(buffer.data(), length) + L"\\.grok\\bin\\grok.exe";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    length = SearchPathW(nullptr, L"grok.exe", nullptr, size, buffer.data(), nullptr);
    if (length > 0 && length < size) {
        return std::wstring(buffer.data(), length);
    }
    return {};
}

namespace {

// grok 1.0.92, 24.11.2026: `grok models` lists grok-4.7 (the default), this one, grok-4.6 and grok-4.5.
// A title and a memory note are one short call; the fast model and low effort keep it cheap.
constexpr const char* kAskModel = "grok-4.7-build-fast";

// `--tools` cannot empty the set: a name that is not a tool leaves the others in place. Deny each built-in
// instead. The id and the name the model is shown differ for five of them; either spelling removes the tool.
// hashline_* is the same file tool as read_file / grep / search_replace. search_replace stays on the list
// beside read_file: grok 1.0.77 refuses to build an agent that can edit and cannot read (0.1.120).
// search_tool and use_tool are the MCP bridge; left in, a one-shot could call the client's own tools.
constexpr const char* kNoTools =
    "read_file,hashline_read,grep,hashline_grep,list_dir,search_replace,hashline_edit,write,"
    "run_terminal_cmd,run_terminal_command,task,spawn_subagent,"
    "get_task_output,get_command_or_subagent_output,wait_tasks,wait_commands_or_subagents,"
    "kill_task,kill_command_or_subagent,web_search,web_fetch,"
    "image_gen,image_edit,image_to_video,reference_to_video,workflow,"
    "scheduler_create,scheduler_delete,scheduler_list,monitor,enter_plan_mode,exit_plan_mode,"
    "ask_user_question,todo_write,search_tool,use_tool,Agent";

bool Blank(std::string_view text) {
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

std::string Snip(std::string_view text) {
    std::string out(text.substr(0, std::min(text.size(), size_t{200})));
    for (char& c : out) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return out;
}

std::wstring NewSessionId() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) {
        std::random_device device;
        id.Data1 = device();
        id.Data2 = static_cast<unsigned short>(device());
        id.Data3 = static_cast<unsigned short>(0x4000 | (device() & 0x0FFF));
        for (unsigned char& byte : id.Data4) {
            byte = static_cast<unsigned char>(device());
        }
        id.Data4[0] = static_cast<unsigned char>(0x80 | (id.Data4[0] & 0x3F));
    }
    char text[40];
    snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             static_cast<unsigned long>(id.Data1), static_cast<unsigned>(id.Data2), static_cast<unsigned>(id.Data3),
             static_cast<unsigned>(id.Data4[0]), static_cast<unsigned>(id.Data4[1]),
             static_cast<unsigned>(id.Data4[2]), static_cast<unsigned>(id.Data4[3]),
             static_cast<unsigned>(id.Data4[4]), static_cast<unsigned>(id.Data4[5]),
             static_cast<unsigned>(id.Data4[6]), static_cast<unsigned>(id.Data4[7]));
    return ToWide(text);
}

bool SameFolder(std::wstring left, std::wstring right) {
    auto norm = [](std::wstring path) {
        for (wchar_t& c : path) {
            if (c == L'/') {
                c = L'\\';
            }
        }
        while (!path.empty() && path.back() == L'\\') {
            path.pop_back();
        }
        return path;
    };
    left = norm(std::move(left));
    right = norm(std::move(right));
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// The session of this call only, and only when its folder is the scratch one. Never a project chat.
void DropScratchSession(const std::wstring& scratch, const std::string& sessionId, const std::wstring& reportedCwd) {
    if (sessionId.empty() || sessionId.find_first_of("/\\.:") != std::string::npos) {
        return;
    }
    const auto drop = [&](const std::wstring& workspace) {
        if (workspace.empty() || !SameFolder(workspace, scratch)) {
            return;
        }
        std::error_code error;
        const fs::path parent = GrokHistoryFolder(workspace);
        fs::remove_all(parent / ToWide(sessionId), error);
        if (fs::is_directory(parent, error) && fs::is_empty(parent, error)) {
            fs::remove(parent, error);
        }
    };
    drop(scratch);
    drop(reportedCwd);
}

bool WriteUtf8(const fs::path& file, std::string_view text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

}  // namespace

GrokAskJson ParseGrokAsk(std::string_view output) {
    GrokAskJson result;
    // Top-level objects only. The answer's text is itself JSON often (a memory note), so a `{` inside a string
    // is not another result. stderr may sit on either side; the last object that carries an answer wins.
    bool inString = false;
    bool escape = false;
    int depth = 0;
    size_t start = std::string_view::npos;
    Json chosen = nullptr;
    bool any = false;
    for (size_t i = 0; i < output.size(); ++i) {
        const char c = output[i];
        if (inString) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
            continue;
        }
        if (c == '{') {
            if (depth == 0) {
                start = i;
            }
            ++depth;
        } else if (c == '}' && depth > 0) {
            --depth;
            if (depth == 0 && start != std::string_view::npos) {
                const Json object = Json::parse(output.substr(start, i - start + 1), nullptr, false);
                if (object.is_object() && (object.contains("text") || object.contains("stopReason") ||
                                           object.contains("error") || object.contains("sessionId"))) {
                    chosen = object;
                    any = true;
                }
                start = std::string_view::npos;
            }
        }
    }
    if (!any) {
        result.error = "no answer";
        return result;
    }
    result.sessionId = JsonStr(chosen, "sessionId");
    result.cwd = JsonStr(chosen, "cwd");
    const std::string stop = JsonStr(chosen, "stopReason");
    const Json& text = JsonAt(chosen, "text");
    const std::string words = text.is_string() ? text.get<std::string>() : std::string();
    if (stop == "end_turn" && !Blank(words)) {
        result.ok = true;
        result.text = words;
        return result;
    }
    const Json& failure = JsonAt(chosen, "error");
    if (failure.is_string() && !failure.get<std::string>().empty()) {
        result.error = Snip(failure.get<std::string>());
    } else if (!stop.empty() && stop != "end_turn") {
        result.error = stop;
    } else {
        result.error = Blank(words) ? "empty answer" : "no answer";
    }
    return result;
}

GrokAsk AskGrok(const std::string& prompt, std::string_view systemPrompt, int timeoutSeconds,
                const std::atomic<bool>* cancel, std::wstring_view scratchName) {
    GrokAsk result;
    if (scratchName.empty() || scratchName.find_first_of(L"\\/:") != std::wstring_view::npos) {
        result.error = "bad scratch";
        return result;
    }
    const std::wstring exe = FindGrokExe();
    if (exe.empty()) {
        result.error = "grok.exe not found";
        return result;
    }
    wchar_t temp[MAX_PATH]{};
    const DWORD tempLen = GetTempPathW(MAX_PATH, temp);
    if (tempLen == 0 || tempLen >= MAX_PATH) {
        result.error = "no temp folder";
        return result;
    }
    const fs::path scratch = fs::path(std::wstring(temp, tempLen)) / scratchName;
    std::error_code error;
    fs::create_directories(scratch, error);
    const std::wstring suffix = std::to_wstring(GetCurrentThreadId());
    const fs::path promptFile = scratch / (L"prompt-" + suffix + L".txt");
    const fs::path systemFile = scratch / (L"system-" + suffix + L".txt");
    if (!WriteUtf8(promptFile, prompt) || !WriteUtf8(systemFile, systemPrompt)) {
        fs::remove(promptFile, error);
        fs::remove(systemFile, error);
        result.error = "could not write the prompt";
        return result;
    }
    const std::wstring sessionId = NewSessionId();
    // No -p: with no prompt of its own it would read stdin. --prompt-file is the whole prompt and starts the
    // headless run. --max-turns 1 stops a turn that still finds a tool. No --always-approve.
    const std::wstring command =
        QuoteArg(exe) + L" --prompt-file " + QuoteArg(promptFile.wstring()) + L" --system-prompt-file " +
        QuoteArg(systemFile.wstring()) + L" --output-format json --model " + QuoteArg(ToWide(kAskModel)) +
        L" --effort low --max-turns 1 --no-memory --disable-web-search --no-subagents --no-plan --verbatim "
        L"--no-auto-update --session-id " +
        QuoteArg(sessionId) + L" --disallowed-tools " + QuoteArg(ToWide(kNoTools));
    const CliResult run = RunCli(command, PickProxy(), timeoutSeconds, cancel,
                                 {}, {{L"NO_COLOR", L"1"}, {L"GROK_DISABLE_AUTOUPDATER", L"1"}, {L"GROK_MEMORY", L"0"}},
                                 scratch.wstring());
    fs::remove(promptFile, error);
    fs::remove(systemFile, error);
    const GrokAskJson parsed = ParseGrokAsk(run.output);
    DropScratchSession(scratch.wstring(), ToUtf8(sessionId), ToWide(parsed.cwd));
    if (!parsed.sessionId.empty()) {
        DropScratchSession(scratch.wstring(), parsed.sessionId, ToWide(parsed.cwd));
    }
    if (!run.started || run.timedOut || run.cancelled) {
        result.error = run.timedOut ? "timed out" : "did not run";
        return result;
    }
    if (run.exitCode == 0 && parsed.ok) {
        result.ok = true;
        result.text = parsed.text;
        return result;
    }
    if (!parsed.error.empty() && parsed.error != "no answer") {
        result.error = parsed.error;
    } else {
        result.error = run.output.empty() ? "no answer" : Snip(run.output);
    }
    return result;
}

}  // namespace aic
