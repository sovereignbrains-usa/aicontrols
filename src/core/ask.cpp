#include "core/ask.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>

#include "core/oauth.h"

namespace aic {

namespace {

constexpr size_t kAnswerCap = 60'000;    // bytes of an answer handed back
constexpr size_t kReplyCap = 1u << 20;   // bytes of a reply read at all
constexpr long long kDefaultTokens = 4096;
constexpr long long kMaxTokens = 16000;

std::string Lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string Trim(std::string text) {
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!text.empty() && space(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    size_t start = 0;
    while (start < text.size() && space(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    return text.substr(start);
}

std::string Cut(std::string text, size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;  // not in the middle of a UTF-8 character
    }
    return text.substr(0, end) + "…";
}

Json Parse(const std::string& body) {
    return Json::parse(body, nullptr, false);
}

std::string Dump(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

}  // namespace

const std::vector<AskProvider>& AskModels::Providers() {
    // The defaults are the ones in use when this was written; "models" lists what a key can reach now.
    static const std::vector<AskProvider> list = {
        {"gemini", L"generativelanguage.googleapis.com", "/v1beta/openai", "GEMINI_API_KEY", "gemini-flash-latest",
         "https://aistudio.google.com/apikey"},
        {"groq", L"api.groq.com", "/openai/v1", "GROQ_API_KEY", "openai/gpt-oss-120b",
         "https://console.groq.com/keys"},
        {"openrouter", L"openrouter.ai", "/api/v1", "OPENROUTER_API_KEY", "",
         "https://openrouter.ai/keys (free models end in :free)"},
        {"mistral", L"api.mistral.ai", "/v1", "MISTRAL_API_KEY", "mistral-small-latest",
         "https://console.mistral.ai/api-keys"},
        {"cerebras", L"api.cerebras.ai", "/v1", "CEREBRAS_API_KEY", "", "https://cloud.cerebras.ai"},
        {"deepseek", L"api.deepseek.com", "", "DEEPSEEK_API_KEY", "deepseek-chat",
         "https://platform.deepseek.com/api_keys"},
        {"qwen", L"dashscope-intl.aliyuncs.com", "/compatible-mode/v1", "DASHSCOPE_API_KEY", "qwen-plus",
         "Alibaba Cloud Model Studio (DashScope, international)"},
    };
    return list;
}

const AskProvider* AskModels::Find(std::string_view name) {
    const std::string wanted = Lower(name);
    for (const AskProvider& provider : Providers()) {
        if (provider.name == wanted) {
            return &provider;
        }
    }
    return nullptr;
}

std::string AskModels::RequestBody(std::string_view model, std::string_view system, std::string_view prompt,
                                   long long maxTokens) {
    Json messages = Json::array();
    if (!system.empty()) {
        messages.push_back(Json{{"role", "system"}, {"content", std::string(system)}});
    }
    messages.push_back(Json{{"role", "user"}, {"content", std::string(prompt)}});
    return Dump(Json{{"model", std::string(model)},
                     {"messages", messages},
                     {"max_tokens", std::clamp(maxTokens, 1ll, kMaxTokens)}});
}

std::string AskModels::ErrorMessage(const std::string& body) {
    Json parsed = Parse(body);
    if (parsed.is_array() && !parsed.empty()) {
        parsed = parsed.front();
    }
    if (parsed.is_object()) {
        const Json& error = JsonAt(parsed, "error");
        if (error.is_object()) {
            if (const std::string message = JsonStr(error, "message"); !message.empty()) {
                return Cut(message, 400);
            }
        } else if (error.is_string()) {
            return Cut(error.get<std::string>(), 400);
        }
        if (const std::string message = JsonStr(parsed, "message"); !message.empty()) {
            return Cut(message, 400);
        }
    }
    return Cut(Trim(body), 200);
}

std::string AskModels::ParseReply(const std::string& body, std::string& usage, std::string& error) {
    const Json parsed = Parse(body);
    if (!parsed.is_object()) {
        error = "The reply is not JSON: " + Cut(Trim(body), 200);
        return {};
    }
    const Json& choices = JsonAt(parsed, "choices");
    if (!choices.is_array() || choices.empty()) {
        error = "No answer in the reply: " + ErrorMessage(body);
        return {};
    }
    const Json& message = JsonAt(choices.front(), "message");
    std::string text;
    const Json& content = JsonAt(message, "content");
    if (content.is_string()) {
        text = content.get<std::string>();
    } else if (content.is_array()) {
        for (const Json& part : content) {
            text += JsonStr(part, "text");
        }
    }
    text = Trim(text);
    if (text.empty()) {
        const std::string why = JsonStr(choices.front(), "finish_reason");
        error = "The model sent no text" + (why.empty() ? std::string() : " (finish_reason: " + why + ")") +
                "; a reasoning model may need a larger max_tokens.";
        return {};
    }
    const Json& counts = JsonAt(parsed, "usage");
    if (counts.is_object() && JsonAt(counts, "prompt_tokens").is_number() &&
        JsonAt(counts, "completion_tokens").is_number()) {
        usage = std::format("{} in / {} out", JsonAt(counts, "prompt_tokens").get<long long>(),
                            JsonAt(counts, "completion_tokens").get<long long>());
    }
    return text;
}

std::string AskModels::ParseModels(const std::string& body) {
    const Json parsed = Parse(body);
    const Json& data = parsed.is_object() ? JsonAt(parsed, "data") : parsed;
    std::string out;
    if (data.is_array()) {
        for (const Json& item : data) {
            std::string id = JsonStr(item, "id");
            if (id.empty()) {
                id = JsonStr(item, "name");
            }
            if (!id.empty()) {
                out += (out.empty() ? "" : "\n") + id;
            }
        }
    }
    return out;
}

ToolResult AskModels::Ask(const Json& input, const std::atomic<bool>& cancel) const {
    const auto fail = [](std::string text) { return ToolResult{std::move(text), true}; };
    if (cancel.load()) {
        return fail("Cancelled.");
    }
    const std::string providerName = JsonStr(input, "provider");
    const AskProvider* provider = Find(providerName);
    if (provider == nullptr) {
        std::string names;
        for (const AskProvider& each : Providers()) {
            names += (names.empty() ? "" : ", ") + each.name;
        }
        return fail("Unknown provider '" + providerName + "'. Providers: " + names + ".");
    }
    const std::optional<std::string> stored = secrets_.Get(provider->secret);
    const std::string key = stored ? Trim(*stored) : std::string();
    if (key.empty()) {
        return fail(std::format("The secret {} is not stored. A key for {}: {}. The user adds it in the secrets panel "
                                "(the name exactly {}); it is never shown to you.",
                                provider->secret, provider->name, provider->keyPage, provider->secret));
    }
    const std::wstring headers = ToWide("Authorization: Bearer " + key + "\r\nContent-Type: application/json\r\n");

    if (JsonStr(input, "action") == "models") {
        const HttpsReply reply = HttpsRequest(L"GET", provider->host.c_str(), ToWide(provider->basePath + "/models"),
                                              headers, {}, kReplyCap);
        if (reply.status == 0) {
            return fail(provider->name + ": " + reply.error);
        }
        if (reply.status >= 300) {
            return fail(std::format("{}: HTTP {}: {}", provider->name, reply.status, ErrorMessage(reply.body)));
        }
        const std::string models = ParseModels(reply.body);
        return ToolResult{models.empty() ? "The provider lists no models." : models, models.empty()};
    }

    const std::string prompt = secrets_.Redact(JsonStr(input, "prompt"));
    if (Trim(prompt).empty()) {
        return fail("prompt is empty.");
    }
    std::string model = JsonStr(input, "model");
    if (model.empty()) {
        model = provider->defaultModel;
    }
    if (model.empty()) {
        return fail(provider->name + " has no default model: name one in `model` (action \"models\" lists them).");
    }
    long long maxTokens = kDefaultTokens;
    if (const Json& given = JsonAt(input, "max_tokens"); given.is_number_integer()) {
        maxTokens = given.get<long long>();
    }
    const std::string body = RequestBody(model, secrets_.Redact(JsonStr(input, "system")), prompt, maxTokens);
    const HttpsReply reply = HttpsRequest(L"POST", provider->host.c_str(),
                                          ToWide(provider->basePath + "/chat/completions"), headers, body, kReplyCap);
    if (cancel.load()) {
        return fail("Cancelled.");
    }
    if (reply.status == 0) {
        return fail(provider->name + ": " + reply.error);
    }
    if (reply.status >= 300) {
        std::string text = std::format("{} {}: HTTP {}: {}", provider->name, model, reply.status,
                                       ErrorMessage(reply.body));
        if (reply.status == 429) {
            text += " (a free tier's limit: wait, or ask another provider)";
        }
        return fail(text);
    }
    std::string usage;
    std::string error;
    const std::string answer = ParseReply(reply.body, usage, error);
    if (answer.empty()) {
        return fail(std::format("{} {}: {}", provider->name, model, error));
    }
    std::string header = std::format("[{} · {}{}]", provider->name, model, usage.empty() ? "" : " · " + usage);
    return ToolResult{header + "\n" + Cut(answer, kAnswerCap), false};
}

}  // namespace aic
