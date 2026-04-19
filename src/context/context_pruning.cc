// ============================================================================
// context_pruning.cc
// 🎯 严格对齐 OpenClaw: src/agents/pi-hooks/context-pruning/
//   - pruner.ts      (10.6KB)
//   - settings.ts    (3.5KB)
//   - tools.ts       (0.7KB)
//   - extension.ts   (挂载点，HC 在 ReactEngine::execute_loop 里直接调用)
//   - cache-ttl.ts   (provider eligibility)
// ============================================================================

#include "hunterclaw/context/context_pruning.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <unordered_set>

namespace hunterclaw {
namespace context {

namespace {

// 🎯 对齐 OC: CHARS_PER_TOKEN_ESTIMATE (utils/cjk-chars.ts)
// OC 对 ASCII 约 4 字符/token、CJK 约 1.5 字符/token 加权估算。
// HC 采用简化的 4 字符/token（与 estimate_conversation_tokens 一致）。
constexpr int kCharsPerTokenEstimate = 4;

// 🎯 对齐 OC: PRUNED_CONTEXT_IMAGE_MARKER (HC 当前 LLMMessage.content 为纯文本，
// 无 image 分块；保留常量以便未来扩展)
[[maybe_unused]] constexpr const char* kPrunedImageMarker =
    "[image removed during context pruning]";

// ────────────────────────────────────────────────────────────────────────────
// Glob matcher —— 与 sandbox/tool_policy.cc 的实现保持一致语义
// 🎯 对齐 OC: compileGlobPatterns + matchesAnyGlobPattern
// ────────────────────────────────────────────────────────────────────────────

std::string normalize_glob(const std::string& value) {
    std::string result;
    size_t s = value.find_first_not_of(" \t");
    size_t e = value.find_last_not_of(" \t");
    if (s == std::string::npos) return "";
    result = value.substr(s, e - s + 1);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

bool matches_glob(const std::string& value, const std::string& pattern) {
    if (pattern.empty()) return false;
    if (pattern == "*") return true;
    if (pattern.find('*') == std::string::npos &&
        pattern.find('?') == std::string::npos) {
        return value == pattern;
    }
    std::string regex_str;
    regex_str.reserve(pattern.size() * 2);
    for (char c : pattern) {
        switch (c) {
            case '*': regex_str += ".*"; break;
            case '?': regex_str += "."; break;
            case '.': case '+': case '(': case ')':
            case '[': case ']': case '^': case '$':
            case '{': case '}': case '|': case '\\':
                regex_str += '\\';
                regex_str += c;
                break;
            default:
                regex_str += c;
        }
    }
    try {
        std::regex re(regex_str, std::regex::icase);
        return std::regex_match(value, re);
    } catch (...) {
        return value == pattern;
    }
}

bool matches_any_glob(const std::string& value,
                      const std::vector<std::string>& patterns) {
    for (const auto& p : patterns) {
        if (matches_glob(value, normalize_glob(p))) return true;
    }
    return false;
}

// ────────────────────────────────────────────────────────────────────────────
// Char estimation —— 对齐 OC: estimateMessageChars / estimateContextChars
// HC 简化：LLMMessage.content 是单一 string，整体按长度计算。
// tool_calls（assistant 消息内）按 JSON 序列化长度累加。
// ────────────────────────────────────────────────────────────────────────────

int estimate_message_chars(const llm::LLMMessage& msg) {
    int chars = static_cast<int>(msg.content.size());
    for (const auto& tc : msg.tool_calls) {
        try {
            chars += static_cast<int>(tc.to_string().size());
        } catch (...) {
            chars += 128;
        }
    }
    return chars;
}

int estimate_context_chars(const std::vector<llm::LLMMessage>& messages) {
    int total = 0;
    for (const auto& m : messages) total += estimate_message_chars(m);
    return total;
}

// ────────────────────────────────────────────────────────────────────────────
// Protected tail anchors —— 对齐 OC: findAssistantCutoffIndex + findFirstUserIndex
// 返回 cutoff_index: 仅可剪枝 [pruneStartIndex, cutoffIndex) 区间
// ────────────────────────────────────────────────────────────────────────────

// 返回 cutoff index，找不到返回 -1（表示不剪枝）
int find_assistant_cutoff_index(const std::vector<llm::LLMMessage>& messages,
                                int keep_last_assistants) {
    if (keep_last_assistants <= 0) {
        return static_cast<int>(messages.size());
    }
    int remaining = keep_last_assistants;
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        if (messages[i].role != llm::LLMMessageRole::Assistant) continue;
        if (--remaining == 0) return i;
    }
    // 对齐 OC: assistant 消息数量不足 keep_last_assistants → 不剪枝
    return -1;
}

int find_first_user_index(const std::vector<llm::LLMMessage>& messages) {
    for (int i = 0; i < static_cast<int>(messages.size()); ++i) {
        if (messages[i].role == llm::LLMMessageRole::User) return i;
    }
    return -1;
}

// ────────────────────────────────────────────────────────────────────────────
// Soft trim —— 对齐 OC: softTrimToolResultMessage
// 保留首尾各 N 字符，中间 "..." 并追加 note
// 返回 std::nullopt 表示无需变更
// ────────────────────────────────────────────────────────────────────────────

std::optional<std::string> soft_trim_tool_result_content(
    const std::string& content,
    const EffectiveContextPruningSettings& settings) {
    const int raw_len = static_cast<int>(content.size());
    if (raw_len <= settings.soft_trim.max_chars) {
        return std::nullopt;  // 无需变更
    }
    const int head_chars = std::max(0, settings.soft_trim.head_chars);
    const int tail_chars = std::max(0, settings.soft_trim.tail_chars);
    if (head_chars + tail_chars >= raw_len) {
        return std::nullopt;
    }

    std::string head = content.substr(0, static_cast<size_t>(head_chars));
    std::string tail = content.substr(content.size() - static_cast<size_t>(tail_chars));

    std::string trimmed = head + "\n...\n" + tail;
    std::string note = "\n\n[Tool result trimmed: kept first " +
                       std::to_string(head_chars) + " chars and last " +
                       std::to_string(tail_chars) + " chars of " +
                       std::to_string(raw_len) + " chars.]";
    return trimmed + note;
}

}  // anonymous namespace

// ============================================================================
// Settings —— 对齐 OC: settings.ts
// ============================================================================

// 🎯 对齐 OC: DEFAULT_CONTEXT_PRUNING_SETTINGS
EffectiveContextPruningSettings default_context_pruning_settings() {
    EffectiveContextPruningSettings s;
    s.mode = ContextPruningMode::CACHE_TTL;
    s.ttl_ms = 5 * 60 * 1000;
    s.keep_last_assistants = 3;
    s.soft_trim_ratio = 0.3;
    s.hard_clear_ratio = 0.5;
    s.min_prunable_tool_chars = 50'000;
    s.soft_trim.max_chars = 4'000;
    s.soft_trim.head_chars = 1'500;
    s.soft_trim.tail_chars = 1'500;
    s.hard_clear.enabled = true;
    s.hard_clear.placeholder = "[Old tool result content cleared]";
    return s;
}

// 🎯 对齐 OC: computeEffectiveSettings
std::optional<EffectiveContextPruningSettings> compute_effective_settings(
    const ContextPruningConfig& config) {
    // 对齐 OC: mode !== "cache-ttl" → null
    if (config.mode != ContextPruningMode::CACHE_TTL) {
        return std::nullopt;
    }
    EffectiveContextPruningSettings s = default_context_pruning_settings();
    s.mode = config.mode;
    if (config.ttl_ms > 0) s.ttl_ms = config.ttl_ms;
    if (config.keep_last_assistants >= 0) {
        s.keep_last_assistants = config.keep_last_assistants;
    }
    s.soft_trim_ratio = std::clamp(config.soft_trim_ratio, 0.0, 1.0);
    s.hard_clear_ratio = std::clamp(config.hard_clear_ratio, 0.0, 1.0);
    if (config.min_prunable_tool_chars >= 0) {
        s.min_prunable_tool_chars = config.min_prunable_tool_chars;
    }
    s.tools = config.tools;
    if (config.soft_trim.max_chars >= 0) s.soft_trim.max_chars = config.soft_trim.max_chars;
    if (config.soft_trim.head_chars >= 0) s.soft_trim.head_chars = config.soft_trim.head_chars;
    if (config.soft_trim.tail_chars >= 0) s.soft_trim.tail_chars = config.soft_trim.tail_chars;
    s.hard_clear.enabled = config.hard_clear.enabled;
    if (!config.hard_clear.placeholder.empty()) {
        s.hard_clear.placeholder = config.hard_clear.placeholder;
    }
    return s;
}

// ============================================================================
// Tool Predicate —— 对齐 OC: tools.ts
// ============================================================================

ToolPrunablePredicate make_tool_prunable_predicate(
    const ContextPruningToolMatch& match) {
    // 捕获值语义，避免生命周期问题
    std::vector<std::string> deny = match.deny;
    std::vector<std::string> allow = match.allow;
    return [deny = std::move(deny), allow = std::move(allow)]
           (const std::string& tool_name) -> bool {
        std::string normalized = normalize_glob(tool_name);
        // 🎯 对齐 OC: deny 优先
        if (matches_any_glob(normalized, deny)) return false;
        // 🎯 对齐 OC: allow 为空 → 全部可剪
        if (allow.empty()) return true;
        return matches_any_glob(normalized, allow);
    };
}

// ============================================================================
// Pruner —— 严格对齐 OC: pruner.ts pruneContextMessages
// ============================================================================

PruneContextResult prune_context_messages(const PruneContextParams& params) {
    PruneContextResult out;
    out.messages = params.messages;  // 默认原样返回

    const auto& settings = params.settings;
    const int context_window_tokens = params.context_window_tokens;

    // 🎯 对齐 OC: 无 contextWindow → 不剪枝
    if (context_window_tokens <= 0) {
        return out;
    }
    const long long char_window =
        static_cast<long long>(context_window_tokens) * kCharsPerTokenEstimate;
    if (char_window <= 0) return out;

    // 🎯 对齐 OC: findAssistantCutoffIndex
    const int cutoff_index = find_assistant_cutoff_index(
        params.messages, settings.keep_last_assistants);
    if (cutoff_index < 0) {
        return out;  // assistant 消息不足，不剪枝
    }

    // 🎯 对齐 OC: Bootstrap safety — 第一条 user 消息之前的内容不剪
    const int first_user_index = find_first_user_index(params.messages);
    const int prune_start_index =
        first_user_index < 0 ? static_cast<int>(params.messages.size())
                             : first_user_index;

    // 🎯 对齐 OC: isToolPrunable 默认从 settings.tools 生成
    ToolPrunablePredicate is_tool_prunable =
        params.is_tool_prunable.value_or(
            make_tool_prunable_predicate(settings.tools));

    // 🎯 对齐 OC: estimateContextChars
    const int total_chars_before = estimate_context_chars(params.messages);
    out.total_chars_before = total_chars_before;
    int total_chars = total_chars_before;
    double ratio = static_cast<double>(total_chars) /
                   static_cast<double>(char_window);

    // 🎯 对齐 OC: 未触达 soft_trim_ratio → 不剪枝
    if (ratio < settings.soft_trim_ratio) {
        out.total_chars_after = total_chars;
        return out;
    }

    // ─── Phase 1: Soft trim ─────────────────────────────────────────────
    // 🎯 对齐 OC: 遍历 [pruneStartIndex, cutoffIndex) 内的 toolResult
    std::vector<int> prunable_tool_indexes;
    auto& msgs = out.messages;  // mutable
    for (int i = prune_start_index;
         i < cutoff_index && i < static_cast<int>(msgs.size()); ++i) {
        auto& msg = msgs[i];
        if (msg.role != llm::LLMMessageRole::Tool) continue;
        // OC 用 msg.toolName；HC 用 msg.name（若缺失则退化，看 content 本身）
        const std::string tool_key = !msg.name.empty() ? msg.name : msg.tool_call_id;
        if (!is_tool_prunable(tool_key)) continue;
        prunable_tool_indexes.push_back(i);

        auto updated = soft_trim_tool_result_content(msg.content, settings);
        if (!updated) continue;  // 无需变更

        const int before = estimate_message_chars(msg);
        msg.content = std::move(*updated);
        const int after = estimate_message_chars(msg);
        total_chars += after - before;
        ++out.soft_trimmed_count;
        out.changed = true;
    }

    ratio = static_cast<double>(total_chars) / static_cast<double>(char_window);

    // 🎯 对齐 OC: 未触达 hard_clear_ratio → 到此为止
    if (ratio < settings.hard_clear_ratio) {
        out.total_chars_after = total_chars;
        return out;
    }
    if (!settings.hard_clear.enabled) {
        out.total_chars_after = total_chars;
        return out;
    }

    // 🎯 对齐 OC: 可剪枝 tool 总字符 < 门槛 → 不值得 hard-clear
    int prunable_tool_chars = 0;
    for (int i : prunable_tool_indexes) {
        if (i >= 0 && i < static_cast<int>(msgs.size()) &&
            msgs[i].role == llm::LLMMessageRole::Tool) {
            prunable_tool_chars += estimate_message_chars(msgs[i]);
        }
    }
    if (prunable_tool_chars < settings.min_prunable_tool_chars) {
        out.total_chars_after = total_chars;
        return out;
    }

    // ─── Phase 2: Hard clear ────────────────────────────────────────────
    // 🎯 对齐 OC: 按顺序 hard-clear 直到 ratio 降到 hard_clear_ratio 以下
    for (int i : prunable_tool_indexes) {
        if (ratio < settings.hard_clear_ratio) break;
        if (i < 0 || i >= static_cast<int>(msgs.size())) continue;
        auto& msg = msgs[i];
        if (msg.role != llm::LLMMessageRole::Tool) continue;

        const int before = estimate_message_chars(msg);
        msg.content = settings.hard_clear.placeholder;
        const int after = estimate_message_chars(msg);
        total_chars += after - before;
        ratio = static_cast<double>(total_chars) /
                static_cast<double>(char_window);
        ++out.hard_cleared_count;
        out.changed = true;
    }

    out.total_chars_after = total_chars;
    return out;
}

// ============================================================================
// JSON Config Parser —— 对齐 OC: config loader (parseContextPruningConfig)
// ============================================================================

namespace {

// 解析时长：接受纯数字（毫秒）或 "300s" / "5m" / "1h" 格式
int64_t parse_duration_ms(const core::JsonValue& v) {
    if (v.is_number()) {
        // 纯数字视为毫秒
        return static_cast<int64_t>(v.as_number());
    }
    if (!v.is_string()) return 0;
    std::string s = v.as_string();
    if (s.empty()) return 0;

    char unit = 0;
    size_t n_end = 0;
    while (n_end < s.size() &&
           (std::isdigit(static_cast<unsigned char>(s[n_end])) ||
            s[n_end] == '.')) {
        ++n_end;
    }
    if (n_end == 0) return 0;
    double num = 0.0;
    try {
        num = std::stod(s.substr(0, n_end));
    } catch (...) {
        return 0;
    }
    if (n_end < s.size()) unit = s[n_end];

    switch (unit) {
        case 's': case 'S': return static_cast<int64_t>(num * 1000);
        case 'm': case 'M': return static_cast<int64_t>(num * 60 * 1000);
        case 'h': case 'H': return static_cast<int64_t>(num * 60 * 60 * 1000);
        default:            return static_cast<int64_t>(num);  // 默认毫秒
    }
}

std::vector<std::string> parse_string_array(const core::JsonValue& v) {
    std::vector<std::string> out;
    if (!v.is_array()) return out;
    for (size_t i = 0; i < v.size(); ++i) {
        const auto& item = v[i];
        if (item.is_string()) out.push_back(item.as_string());
    }
    return out;
}

}  // anonymous namespace

ContextPruningConfig parse_context_pruning_config_from_json(
    const core::JsonValue& json) {
    ContextPruningConfig cfg;  // 默认 mode=OFF
    if (!json.is_object()) return cfg;

    // mode
    if (json.contains("mode")) {
        const auto& m = json["mode"];
        if (m.is_string()) {
            std::string mode_str = m.as_string();
            std::transform(mode_str.begin(), mode_str.end(), mode_str.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            if (mode_str == "cache-ttl") {
                cfg.mode = ContextPruningMode::CACHE_TTL;
            }
        }
    }

    if (json.contains("ttl")) {
        int64_t ttl = parse_duration_ms(json["ttl"]);
        if (ttl > 0) cfg.ttl_ms = ttl;
    }

    if (json.contains("keepLastAssistants") &&
        json["keepLastAssistants"].is_number()) {
        cfg.keep_last_assistants =
            static_cast<int>(json["keepLastAssistants"].as_number());
    }
    if (json.contains("softTrimRatio") &&
        json["softTrimRatio"].is_number()) {
        cfg.soft_trim_ratio = json["softTrimRatio"].as_number();
    }
    if (json.contains("hardClearRatio") &&
        json["hardClearRatio"].is_number()) {
        cfg.hard_clear_ratio = json["hardClearRatio"].as_number();
    }
    if (json.contains("minPrunableToolChars") &&
        json["minPrunableToolChars"].is_number()) {
        cfg.min_prunable_tool_chars =
            static_cast<int>(json["minPrunableToolChars"].as_number());
    }

    if (json.contains("tools") && json["tools"].is_object()) {
        const auto& tools_json = json["tools"];
        if (tools_json.contains("allow")) {
            cfg.tools.allow = parse_string_array(tools_json["allow"]);
        }
        if (tools_json.contains("deny")) {
            cfg.tools.deny = parse_string_array(tools_json["deny"]);
        }
    }

    if (json.contains("softTrim") && json["softTrim"].is_object()) {
        const auto& st = json["softTrim"];
        if (st.contains("maxChars") && st["maxChars"].is_number()) {
            cfg.soft_trim.max_chars =
                static_cast<int>(st["maxChars"].as_number());
        }
        if (st.contains("headChars") && st["headChars"].is_number()) {
            cfg.soft_trim.head_chars =
                static_cast<int>(st["headChars"].as_number());
        }
        if (st.contains("tailChars") && st["tailChars"].is_number()) {
            cfg.soft_trim.tail_chars =
                static_cast<int>(st["tailChars"].as_number());
        }
    }

    if (json.contains("hardClear") && json["hardClear"].is_object()) {
        const auto& hc = json["hardClear"];
        if (hc.contains("enabled") && hc["enabled"].is_bool()) {
            cfg.hard_clear.enabled = hc["enabled"].as_bool();
        }
        if (hc.contains("placeholder") && hc["placeholder"].is_string()) {
            std::string ph = hc["placeholder"].as_string();
            if (!ph.empty()) cfg.hard_clear.placeholder = ph;
        }
    }

    return cfg;
}

// ============================================================================
// Cache-TTL Eligibility —— 对齐 OC: cache-ttl.ts isCacheTtlEligibleProvider
// ============================================================================

bool is_cache_ttl_eligible_provider(const std::string& provider,
                                    const std::string& /*model_id*/,
                                    const std::string& model_api) {
    // 🎯 对齐 OC: 只对 Anthropic-messages API 兼容的 provider 启用 cache-ttl 剪枝
    // （这些 provider 走 Anthropic messages API，支持 cache_control）
    auto ieq = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    };

    if (ieq(model_api, "anthropic-messages")) return true;

    static const std::unordered_set<std::string> kEligibleProviders = {
        "anthropic", "litellm", "bedrock", "vertex",
    };
    std::string p;
    p.reserve(provider.size());
    for (char c : provider) {
        p.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return kEligibleProviders.count(p) > 0;
}

}  // namespace context
}  // namespace hunterclaw
