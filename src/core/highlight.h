#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aic {

enum class TokenKind : unsigned char { None, Comment, String, Number, Keyword, Variable, Added, Removed };

struct Token {
    size_t start = 0;  // in UTF-16 units of the code
    size_t length = 0;
    TokenKind kind = TokenKind::None;
};

// Colors the parts of `code` that a reader scans for: comments, strings, numbers, keywords, $variables, and the
// +/- lines of a diff. `language` is a fence's info string ("cpp", "python title=x"); an unknown or empty one
// gives no tokens. "plain" colors comments, strings and numbers only. Tokens are in order and do not overlap.
// Unclosed strings and comments (a message still streaming) run to the end of the line or of the code.
std::vector<Token> Highlight(std::wstring_view code, std::wstring_view language);

// A fence with no info string still gets colors: a guess ("cpp", "python", "js", ...) or "plain".
std::wstring GuessLanguage(std::wstring_view code);

}  // namespace aic
