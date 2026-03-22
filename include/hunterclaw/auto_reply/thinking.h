#ifndef HUNTERCLAW_AUTO_REPLY_THINKING_H
#define HUNTERCLAW_AUTO_REPLY_THINKING_H

#include <string>
#include <optional>
#include <vector>
#include <set>
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Thinking / Verbose / Reasoning level types
// Aligned with OpenClaw: src/auto-reply/thinking.ts
// ============================================================================

// Think level (aligned with OpenClaw: ThinkLevel)
enum class ThinkLevel {
    Off,       // "off"
    Minimal,   // "minimal"
    Low,       // "low"
    Medium,    // "medium"
    High,      // "high"
    XHigh,     // "xhigh"
    Adaptive   // "adaptive"
};

// Verbose level (aligned with OpenClaw: VerboseLevel)
enum class VerboseLevel {
    Off,   // "off"
    On,    // "on"
    Full   // "full"
};

// Notice level (aligned with OpenClaw: NoticeLevel)
enum class NoticeLevel {
    Off,   // "off"
    On,    // "on"
    Full   // "full"
};

// Elevated level (aligned with OpenClaw: ElevatedLevel)
enum class ElevatedLevel {
    Off,   // "off"
    On,    // "on"
    Ask,   // "ask"
    Full   // "full"
};

// Elevated mode (aligned with OpenClaw: ElevatedMode)
enum class ElevatedMode {
    Off,   // "off"
    Ask,   // "ask"
    Full   // "full"
};

// Reasoning level (aligned with OpenClaw: ReasoningLevel)
enum class ReasoningLevel {
    Off,     // "off"
    On,      // "on"
    Stream   // "stream"
};

// Usage display level (aligned with OpenClaw: UsageDisplayLevel)
enum class UsageDisplayLevel {
    Off,     // "off"
    Tokens,  // "tokens"
    Full     // "full"
};

// Thinking catalog entry (aligned with OpenClaw: ThinkingCatalogEntry)
struct ThinkingCatalogEntry {
    std::string provider;
    std::string id;
    bool reasoning = false;
};

// XHigh model refs (aligned with OpenClaw: XHIGH_MODEL_REFS)
inline const std::vector<std::string>& xhigh_model_refs() {
    static const std::vector<std::string> refs = {
        "openai/gpt-5.4",
        "openai/gpt-5.4-pro",
        "openai/gpt-5.2",
        "openai-codex/gpt-5.4",
        "openai-codex/gpt-5.3-codex",
        "openai-codex/gpt-5.3-codex-spark",
        "openai-codex/gpt-5.2-codex",
        "openai-codex/gpt-5.1-codex",
        "github-copilot/gpt-5.2-codex",
        "github-copilot/gpt-5.2",
    };
    return refs;
}

inline std::string normalize_provider_id(const std::string& provider) {
    if (provider.empty()) return "";
    std::string normalized = provider;
    // Trim
    size_t s = normalized.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return "";
    size_t e = normalized.find_last_not_of(" \t\r\n");
    normalized = normalized.substr(s, e - s + 1);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    if (normalized == "z.ai" || normalized == "z-ai") return "zai";
    if (normalized == "bedrock" || normalized == "aws-bedrock") return "amazon-bedrock";
    return normalized;
}

/**
 * Check if provider uses binary thinking (on/off only).
 * Aligned with OpenClaw: isBinaryThinkingProvider()
 */
inline bool is_binary_thinking_provider(const std::string& provider) {
    return normalize_provider_id(provider) == "zai";
}

/**
 * Check if model supports xhigh thinking.
 * Aligned with OpenClaw: supportsXHighThinking()
 */
inline bool supports_xhigh_thinking(const std::string& provider, const std::string& model) {
    if (model.empty()) return false;
    std::string model_key = model;
    std::transform(model_key.begin(), model_key.end(), model_key.begin(), ::tolower);
    // Trim
    size_t s = model_key.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return false;
    size_t e = model_key.find_last_not_of(" \t\r\n");
    model_key = model_key.substr(s, e - s + 1);

    std::string provider_key = normalize_provider_id(provider);

    // Build sets
    static std::set<std::string> xhigh_set;
    static std::set<std::string> xhigh_ids;
    static bool initialized = false;
    if (!initialized) {
        for (const auto& ref : xhigh_model_refs()) {
            std::string lower_ref = ref;
            std::transform(lower_ref.begin(), lower_ref.end(), lower_ref.begin(), ::tolower);
            xhigh_set.insert(lower_ref);
            size_t slash = lower_ref.find('/');
            if (slash != std::string::npos) {
                xhigh_ids.insert(lower_ref.substr(slash + 1));
            }
        }
        initialized = true;
    }

    if (!provider_key.empty()) {
        return xhigh_set.count(provider_key + "/" + model_key) > 0;
    }
    return xhigh_ids.count(model_key) > 0;
}

/**
 * Normalize a think level string.
 * Aligned with OpenClaw: normalizeThinkLevel()
 */
inline std::optional<ThinkLevel> normalize_think_level(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string key = raw;
    size_t s = key.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = key.find_last_not_of(" \t\r\n");
    key = key.substr(s, e - s + 1);
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    // Collapse separators
    std::string collapsed;
    for (char c : key) {
        if (c == ' ' || c == '_' || c == '-') continue;
        collapsed += c;
    }

    if (collapsed == "adaptive" || collapsed == "auto") return ThinkLevel::Adaptive;
    if (collapsed == "xhigh" || collapsed == "extrahigh") return ThinkLevel::XHigh;
    if (key == "off") return ThinkLevel::Off;
    if (key == "on" || key == "enable" || key == "enabled") return ThinkLevel::Low;
    if (key == "min" || key == "minimal") return ThinkLevel::Minimal;
    if (key == "low" || key == "thinkhard" || collapsed == "thinkhard") return ThinkLevel::Low;
    if (key == "mid" || key == "med" || key == "medium" ||
        key == "thinkharder" || collapsed == "thinkharder" || key == "harder") return ThinkLevel::Medium;
    if (key == "high" || key == "ultra" || collapsed == "ultrathink" ||
        key == "thinkhardest" || key == "highest" || key == "max") return ThinkLevel::High;
    if (key == "think") return ThinkLevel::Minimal;
    return std::nullopt;
}

/**
 * Normalize a verbose level string.
 * Aligned with OpenClaw: normalizeVerboseLevel()
 */
inline std::optional<VerboseLevel> normalize_verbose_level(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (key == "off" || key == "false" || key == "no" || key == "0") return VerboseLevel::Off;
    if (key == "full" || key == "all" || key == "everything") return VerboseLevel::Full;
    if (key == "on" || key == "minimal" || key == "true" || key == "yes" || key == "1") return VerboseLevel::On;
    return std::nullopt;
}

/**
 * Normalize a notice level string.
 * Aligned with OpenClaw: normalizeNoticeLevel()
 */
inline std::optional<NoticeLevel> normalize_notice_level(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (key == "off" || key == "false" || key == "no" || key == "0") return NoticeLevel::Off;
    if (key == "full" || key == "all" || key == "everything") return NoticeLevel::Full;
    if (key == "on" || key == "minimal" || key == "true" || key == "yes" || key == "1") return NoticeLevel::On;
    return std::nullopt;
}

/**
 * Normalize a usage display level string.
 * Aligned with OpenClaw: normalizeUsageDisplay()
 */
inline std::optional<UsageDisplayLevel> normalize_usage_display(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (key == "off" || key == "false" || key == "no" || key == "0" ||
        key == "disable" || key == "disabled") return UsageDisplayLevel::Off;
    if (key == "on" || key == "true" || key == "yes" || key == "1" ||
        key == "enable" || key == "enabled") return UsageDisplayLevel::Tokens;
    if (key == "tokens" || key == "token" || key == "tok" ||
        key == "minimal" || key == "min") return UsageDisplayLevel::Tokens;
    if (key == "full" || key == "session") return UsageDisplayLevel::Full;
    return std::nullopt;
}

/**
 * Resolve response usage mode (with default "off").
 * Aligned with OpenClaw: resolveResponseUsageMode()
 */
inline UsageDisplayLevel resolve_response_usage_mode(const std::string& raw) {
    return normalize_usage_display(raw).value_or(UsageDisplayLevel::Off);
}

/**
 * Normalize fast mode flag.
 * Aligned with OpenClaw: normalizeFastMode()
 */
inline std::optional<bool> normalize_fast_mode(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (key == "off" || key == "false" || key == "no" || key == "0" ||
        key == "disable" || key == "disabled" || key == "normal") return false;
    if (key == "on" || key == "true" || key == "yes" || key == "1" ||
        key == "enable" || key == "enabled" || key == "fast") return true;
    return std::nullopt;
}

/**
 * Normalize elevated level string.
 * Aligned with OpenClaw: normalizeElevatedLevel()
 */
inline std::optional<ElevatedLevel> normalize_elevated_level(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (key == "off" || key == "false" || key == "no" || key == "0") return ElevatedLevel::Off;
    if (key == "full" || key == "auto" || key == "auto-approve" || key == "autoapprove") return ElevatedLevel::Full;
    if (key == "ask" || key == "prompt" || key == "approval" || key == "approve") return ElevatedLevel::Ask;
    if (key == "on" || key == "true" || key == "yes" || key == "1") return ElevatedLevel::On;
    return std::nullopt;
}

/**
 * Resolve elevated mode from level.
 * Aligned with OpenClaw: resolveElevatedMode()
 */
inline ElevatedMode resolve_elevated_mode(std::optional<ElevatedLevel> level) {
    if (!level || *level == ElevatedLevel::Off) return ElevatedMode::Off;
    if (*level == ElevatedLevel::Full) return ElevatedMode::Full;
    return ElevatedMode::Ask;
}

/**
 * Normalize reasoning level string.
 * Aligned with OpenClaw: normalizeReasoningLevel()
 */
inline std::optional<ReasoningLevel> normalize_reasoning_level(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if (key == "off" || key == "false" || key == "no" || key == "0" ||
        key == "hide" || key == "hidden" || key == "disable" || key == "disabled") return ReasoningLevel::Off;
    if (key == "on" || key == "true" || key == "yes" || key == "1" ||
        key == "show" || key == "visible" || key == "enable" || key == "enabled") return ReasoningLevel::On;
    if (key == "stream" || key == "streaming" || key == "draft" || key == "live") return ReasoningLevel::Stream;
    return std::nullopt;
}

/**
 * List available thinking levels for a model.
 * Aligned with OpenClaw: listThinkingLevels()
 */
inline std::vector<ThinkLevel> list_thinking_levels(
    const std::string& provider = "",
    const std::string& model = ""
) {
    std::vector<ThinkLevel> levels = {
        ThinkLevel::Off, ThinkLevel::Minimal, ThinkLevel::Low,
        ThinkLevel::Medium, ThinkLevel::High
    };
    if (supports_xhigh_thinking(provider, model)) {
        levels.push_back(ThinkLevel::XHigh);
    }
    levels.push_back(ThinkLevel::Adaptive);
    return levels;
}

/**
 * Resolve the default thinking level for a model.
 * Aligned with OpenClaw: resolveThinkingDefaultForModel()
 */
inline ThinkLevel resolve_thinking_default_for_model(
    const std::string& provider,
    const std::string& model,
    const std::vector<ThinkingCatalogEntry>& catalog = {}
) {
    static const std::regex claude_46_re{
        "claude-(?:opus|sonnet)-4(?:\\.|-)6(?:$|[-.])",
        std::regex::icase
    };

    std::string normalized_provider = normalize_provider_id(provider);
    std::string model_lower = model;
    std::transform(model_lower.begin(), model_lower.end(), model_lower.begin(), ::tolower);

    bool is_anthropic_family =
        normalized_provider == "anthropic" ||
        normalized_provider == "amazon-bedrock" ||
        model_lower.find("anthropic/") != std::string::npos ||
        model_lower.find(".anthropic.") != std::string::npos;

    if (is_anthropic_family && std::regex_search(model_lower, claude_46_re)) {
        return ThinkLevel::Adaptive;
    }

    for (const auto& entry : catalog) {
        if (entry.provider == provider && entry.id == model && entry.reasoning) {
            return ThinkLevel::Low;
        }
    }

    return ThinkLevel::Off;
}

// String conversion helpers
inline std::string to_string(ThinkLevel level) {
    switch (level) {
        case ThinkLevel::Off:      return "off";
        case ThinkLevel::Minimal:  return "minimal";
        case ThinkLevel::Low:      return "low";
        case ThinkLevel::Medium:   return "medium";
        case ThinkLevel::High:     return "high";
        case ThinkLevel::XHigh:    return "xhigh";
        case ThinkLevel::Adaptive: return "adaptive";
    }
    return "off";
}

inline std::string to_string(VerboseLevel level) {
    switch (level) {
        case VerboseLevel::Off:  return "off";
        case VerboseLevel::On:   return "on";
        case VerboseLevel::Full: return "full";
    }
    return "off";
}

inline std::string to_string(ReasoningLevel level) {
    switch (level) {
        case ReasoningLevel::Off:    return "off";
        case ReasoningLevel::On:     return "on";
        case ReasoningLevel::Stream: return "stream";
    }
    return "off";
}

inline std::string to_string(ElevatedLevel level) {
    switch (level) {
        case ElevatedLevel::Off:  return "off";
        case ElevatedLevel::On:   return "on";
        case ElevatedLevel::Ask:  return "ask";
        case ElevatedLevel::Full: return "full";
    }
    return "off";
}

inline std::string to_string(UsageDisplayLevel level) {
    switch (level) {
        case UsageDisplayLevel::Off:    return "off";
        case UsageDisplayLevel::Tokens: return "tokens";
        case UsageDisplayLevel::Full:   return "full";
    }
    return "off";
}

} // namespace auto_reply
} // namespace hunterclaw

#endif // HUNTERCLAW_AUTO_REPLY_THINKING_H
