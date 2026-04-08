#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <regex>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <algorithm>
#include <cmath>

namespace hunterclaw {
namespace security {

// ============================================================================
// Safe Regex - 防 ReDoS 安全正则编译
// 严格对齐 OpenClaw: src/security/safe-regex.ts
// ============================================================================

namespace {

constexpr int SAFE_REGEX_CACHE_MAX = 256;
constexpr int SAFE_REGEX_TEST_WINDOW = 2048;

// --- Quantifier parsing ---

struct QuantifierRead {
    int consumed;
    int min_repeat;
    int max_repeat; // -1 = unbounded
};

struct TokenState {
    bool contains_repetition;
    bool has_ambiguous_alternation;
    int min_length;
    int max_length; // INT_MAX = infinity
};

struct ParseFrame {
    TokenState* last_token = nullptr;
    bool contains_repetition = false;
    bool has_alternation = false;
    int branch_min_length = 0;
    int branch_max_length = 0;
    int alt_min_length = -1; // -1 = null
    int alt_max_length = -1;
};

struct PatternToken {
    enum Kind { SimpleToken, GroupOpen, GroupClose, Alternation, Quantifier };
    Kind kind;
    QuantifierRead quantifier; // only valid for Quantifier kind
};

int add_length(int left, int right) {
    if (left == INT_MAX || right == INT_MAX) return INT_MAX;
    return left + right;
}

int multiply_length(int length, int factor) {
    if (length == INT_MAX) return factor == 0 ? 0 : INT_MAX;
    return length * factor;
}

void record_alternative(ParseFrame& frame) {
    if (frame.alt_min_length < 0 || frame.alt_max_length < 0) {
        frame.alt_min_length = frame.branch_min_length;
        frame.alt_max_length = frame.branch_max_length;
        return;
    }
    frame.alt_min_length = std::min(frame.alt_min_length, frame.branch_min_length);
    frame.alt_max_length = std::max(frame.alt_max_length, frame.branch_max_length);
}

std::optional<QuantifierRead> read_quantifier(const std::string& source, int index) {
    if (index >= (int)source.size()) return std::nullopt;
    char ch = source[index];
    int consumed = (index + 1 < (int)source.size() && source[index + 1] == '?') ? 2 : 1;

    if (ch == '*') return QuantifierRead{consumed, 0, -1};
    if (ch == '+') return QuantifierRead{consumed, 1, -1};
    if (ch == '?') return QuantifierRead{consumed, 0, 1};
    if (ch != '{') return std::nullopt;

    int i = index + 1;
    while (i < (int)source.size() && std::isdigit(source[i])) i++;
    if (i == index + 1) return std::nullopt;

    int min_repeat = std::stoi(source.substr(index + 1, i - index - 1));
    int max_repeat = min_repeat;

    if (i < (int)source.size() && source[i] == ',') {
        i++;
        int max_start = i;
        while (i < (int)source.size() && std::isdigit(source[i])) i++;
        max_repeat = (i == max_start) ? -1 : std::stoi(source.substr(max_start, i - max_start));
    }

    if (i >= (int)source.size() || source[i] != '}') return std::nullopt;
    i++;
    if (i < (int)source.size() && source[i] == '?') i++;
    if (max_repeat >= 0 && max_repeat < min_repeat) return std::nullopt;

    return QuantifierRead{i - index, min_repeat, max_repeat};
}

std::vector<PatternToken> tokenize_pattern(const std::string& source) {
    std::vector<PatternToken> tokens;
    bool in_char_class = false;

    for (int i = 0; i < (int)source.size(); i++) {
        char ch = source[i];

        if (ch == '\\') {
            i++;
            tokens.push_back({PatternToken::SimpleToken, {}});
            continue;
        }

        if (in_char_class) {
            if (ch == ']') in_char_class = false;
            continue;
        }

        if (ch == '[') {
            in_char_class = true;
            tokens.push_back({PatternToken::SimpleToken, {}});
            continue;
        }
        if (ch == '(') { tokens.push_back({PatternToken::GroupOpen, {}}); continue; }
        if (ch == ')') { tokens.push_back({PatternToken::GroupClose, {}}); continue; }
        if (ch == '|') { tokens.push_back({PatternToken::Alternation, {}}); continue; }

        auto q = read_quantifier(source, i);
        if (q) {
            tokens.push_back({PatternToken::Quantifier, *q});
            i += q->consumed - 1;
            continue;
        }

        tokens.push_back({PatternToken::SimpleToken, {}});
    }
    return tokens;
}

bool analyze_tokens_for_nested_repetition(const std::vector<PatternToken>& tokens) {
    std::vector<ParseFrame> frames;
    frames.push_back(ParseFrame{});

    // TokenState pool for ownership
    std::vector<std::unique_ptr<TokenState>> token_pool;

    auto emit_token = [&](TokenState* token) {
        auto& frame = frames.back();
        frame.last_token = token;
        if (token->contains_repetition) frame.contains_repetition = true;
        frame.branch_min_length = add_length(frame.branch_min_length, token->min_length);
        frame.branch_max_length = add_length(frame.branch_max_length, token->max_length);
    };

    auto emit_simple_token = [&]() {
        auto t = std::make_unique<TokenState>();
        t->contains_repetition = false;
        t->has_ambiguous_alternation = false;
        t->min_length = 1;
        t->max_length = 1;
        TokenState* ptr = t.get();
        token_pool.push_back(std::move(t));
        emit_token(ptr);
    };

    for (const auto& token : tokens) {
        if (token.kind == PatternToken::SimpleToken) {
            emit_simple_token();
            continue;
        }
        if (token.kind == PatternToken::GroupOpen) {
            frames.push_back(ParseFrame{});
            continue;
        }
        if (token.kind == PatternToken::GroupClose) {
            if (frames.size() > 1) {
                auto frame = frames.back();
                frames.pop_back();
                if (frame.has_alternation) record_alternative(frame);
                int group_min = frame.has_alternation ? std::max(0, frame.alt_min_length) : frame.branch_min_length;
                int group_max = frame.has_alternation ? std::max(0, frame.alt_max_length) : frame.branch_max_length;
                auto t = std::make_unique<TokenState>();
                t->contains_repetition = frame.contains_repetition;
                t->has_ambiguous_alternation = frame.has_alternation &&
                    frame.alt_min_length >= 0 && frame.alt_max_length >= 0 &&
                    frame.alt_min_length != frame.alt_max_length;
                t->min_length = group_min;
                t->max_length = group_max;
                TokenState* ptr = t.get();
                token_pool.push_back(std::move(t));
                emit_token(ptr);
            }
            continue;
        }
        if (token.kind == PatternToken::Alternation) {
            auto& frame = frames.back();
            frame.has_alternation = true;
            record_alternative(frame);
            frame.branch_min_length = 0;
            frame.branch_max_length = 0;
            frame.last_token = nullptr;
            continue;
        }

        // Quantifier
        auto& frame = frames.back();
        auto* prev = frame.last_token;
        if (!prev) continue;
        if (prev->contains_repetition) return true;
        if (prev->has_ambiguous_alternation && token.quantifier.max_repeat < 0) return true;

        prev->min_length = multiply_length(prev->min_length, token.quantifier.min_repeat);
        prev->max_length = (token.quantifier.max_repeat < 0)
            ? INT_MAX
            : multiply_length(prev->max_length, token.quantifier.max_repeat);
        prev->contains_repetition = true;
        frame.contains_repetition = true;
    }

    return false;
}

// Cache
std::mutex regex_cache_mutex;
std::unordered_map<std::string, std::optional<std::regex>> regex_cache;

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

/**
 * has_nested_repetition - 检测正则表达式是否包含嵌套重复
 * 对齐 OpenClaw: hasNestedRepetition(source)
 */
bool has_nested_repetition(const std::string& source) {
    return analyze_tokens_for_nested_repetition(tokenize_pattern(source));
}

/**
 * compile_safe_regex - 安全编译正则表达式（防 ReDoS）
 * 对齐 OpenClaw: compileSafeRegex(source, flags)
 *
 * @return nullopt 如果正则不安全（嵌套重复）或编译失败
 */
std::optional<std::regex> compile_safe_regex(
    const std::string& source,
    std::regex_constants::syntax_option_type flags
) {
    std::string trimmed = source;
    // Trim
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return std::nullopt;
    trimmed = trimmed.substr(start, end - start + 1);
    if (trimmed.empty()) return std::nullopt;

    // Cache lookup
    std::string cache_key = std::to_string(static_cast<int>(flags)) + "::" + trimmed;
    {
        std::lock_guard<std::mutex> lock(regex_cache_mutex);
        auto it = regex_cache.find(cache_key);
        if (it != regex_cache.end()) return it->second;
    }

    std::optional<std::regex> compiled;
    if (!has_nested_repetition(trimmed)) {
        try {
            compiled = std::regex(trimmed, flags);
        } catch (...) {
            compiled = std::nullopt;
        }
    }

    {
        std::lock_guard<std::mutex> lock(regex_cache_mutex);
        regex_cache[cache_key] = compiled;
        if ((int)regex_cache.size() > SAFE_REGEX_CACHE_MAX) {
            regex_cache.erase(regex_cache.begin());
        }
    }

    return compiled;
}

/**
 * test_regex_with_bounded_input - 在有限窗口内测试正则
 * 对齐 OpenClaw: testRegexWithBoundedInput(regex, input, maxWindow)
 */
bool test_regex_with_bounded_input(
    const std::regex& regex,
    const std::string& input,
    int max_window
) {
    if (max_window <= 0) return false;
    if ((int)input.size() <= max_window) {
        return std::regex_search(input, regex);
    }
    std::string head = input.substr(0, max_window);
    if (std::regex_search(head, regex)) return true;
    std::string tail = input.substr(input.size() - max_window);
    return std::regex_search(tail, regex);
}

}  // namespace security
}  // namespace hunterclaw
