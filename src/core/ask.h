#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "core/tools.h"

namespace aic {

// ask: a question to another model through its own API, for work that is cheaper to hand over than to do (a long
// text to sum up, a draft, a translation, a second opinion). Every provider here speaks the OpenAI chat format, so
// one request covers them. The key is a stored secret (never shown, cut out of every output); the prompt goes out
// with the stored values already replaced by ***. What comes back is the other model's word, not a fact: it is
// checked before it is used.
struct AskProvider {
    std::string name;          // what the model passes as `provider`
    std::wstring host;
    std::string basePath;      // before /chat/completions and /models
    std::string secret;        // the stored secret that holds the key
    std::string defaultModel;  // empty: the model must be named (see action "models")
    std::string keyPage;       // where a key is had, for the error that says there is none
};

class AskModels {
public:
    explicit AskModels(SecretStore& secrets) : secrets_(secrets) {}

    // action "ask" (default): provider, prompt, and optionally model, system, max_tokens.
    // action "models": provider; the model ids it lists.
    ToolResult Ask(const Json& input, const std::atomic<bool>& cancel) const;

    static const std::vector<AskProvider>& Providers();
    // The provider of that name (any case); nullptr when there is none.
    static const AskProvider* Find(std::string_view name);
    // The chat/completions body. max_tokens is held to 1..16000.
    static std::string RequestBody(std::string_view model, std::string_view system, std::string_view prompt,
                                   long long maxTokens);
    // The answer's text out of a chat/completions reply; empty with `error` set when there is none.
    // `usage` gets "N in / M out" when the reply says so.
    static std::string ParseReply(const std::string& body, std::string& usage, std::string& error);
    // The message of an error reply ({"error":{"message"}} or a one-item array of it), cut short; the body's start
    // when it is neither.
    static std::string ErrorMessage(const std::string& body);
    // The model ids of a /models reply, one per line.
    static std::string ParseModels(const std::string& body);

private:
    SecretStore& secrets_;
};

}  // namespace aic
