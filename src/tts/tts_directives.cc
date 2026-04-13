// 对齐 OC: src/tts/directives.ts (135行) + tts-core.ts (212行) + provider-registry.ts (55行)
// TTS 指令解析 + 核心逻辑 + Provider 注册表
#include "hunterclaw/tts/tts_provider.h"
#include <nlohmann/json.hpp>
#include <regex>
#include <algorithm>
#include <map>
#include <stdexcept>
#include <cctype>
#include <optional>
#include <sstream>

using json = nlohmann::json;

namespace hunterclaw {
namespace tts {

// =========================================================================
// 对齐 OC tts-core.ts: Core Validation
// =========================================================================

// 对齐 OC: requireInRange
void require_in_range(double value, double min_val, double max_val,
                       const std::string& label) {
    if (!std::isfinite(value) || value < min_val || value > max_val) {
        throw std::runtime_error(label + " must be between " +
            std::to_string(min_val) + " and " + std::to_string(max_val));
    }
}

// 对齐 OC: normalizeLanguageCode
std::optional<std::string> normalize_language_code(const std::string& code) {
    if (code.empty()) return std::nullopt;

    std::string lower;
    lower.reserve(code.size());
    for (char c : code) lower += static_cast<char>(std::tolower(c));

    // Must be exactly 2 lowercase letters
    if (lower.size() != 2 || !std::isalpha(lower[0]) || !std::isalpha(lower[1])) {
        throw std::runtime_error(
            "languageCode must be a 2-letter ISO 639-1 code (e.g. en, de, fr)");
    }
    return lower;
}

// 对齐 OC: normalizeApplyTextNormalization
std::optional<std::string> normalize_apply_text_normalization(const std::string& mode) {
    if (mode.empty()) return std::nullopt;

    std::string lower;
    for (char c : mode) lower += static_cast<char>(std::tolower(c));

    if (lower == "auto" || lower == "on" || lower == "off") return lower;
    throw std::runtime_error(
        "applyTextNormalization must be 'auto', 'on', or 'off'");
}

// =========================================================================
// 对齐 OC tts-core.ts: Text Summarization Prep
// =========================================================================

static const int64_t TEMP_FILE_CLEANUP_DELAY_MS = 5 * 60 * 1000;  // 5 minutes

// 对齐 OC: prepareSummarizableText
std::string prepare_summarizable_text(const std::string& text, int max_chars) {
    if (max_chars <= 0) max_chars = 4000;
    if (static_cast<int>(text.size()) <= max_chars) return text;

    // Truncate at word boundary
    int pos = max_chars;
    while (pos > 0 && !std::isspace(text[pos])) --pos;
    if (pos == 0) pos = max_chars;  // No word boundary found
    return text.substr(0, pos) + "...";
}

// =========================================================================
// 对齐 OC provider-registry.ts: Speech Provider Registry
// =========================================================================

// 对齐 OC: normalizeSpeechProviderId
std::string normalize_speech_provider_id(const std::string& id) {
    if (id.empty()) return "";
    std::string lower;
    for (char c : id) lower += static_cast<char>(std::tolower(c));
    // Trim whitespace
    size_t start = lower.find_first_not_of(" \t\n\r");
    size_t end = lower.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return lower.substr(start, end - start + 1);
}

// 对齐 OC: listSpeechProviders
std::vector<std::string> list_speech_providers() {
    auto& registry = TtsProviderRegistry::instance();
    std::vector<std::string> providers;
    // Delegate to registry's provider listing
    for (const auto& [name, provider] : registry.get_all_providers()) {
        providers.push_back(name);
    }
    std::sort(providers.begin(), providers.end());
    return providers;
}

// 对齐 OC: getSpeechProvider
TtsProvider::Ptr get_speech_provider(const std::string& provider_id) {
    std::string normalized = normalize_speech_provider_id(provider_id);
    if (normalized.empty()) return nullptr;
    return TtsProviderRegistry::instance().get_provider(normalized);
}

// 对齐 OC: canonicalizeSpeechProviderId
std::string canonicalize_speech_provider_id(const std::string& provider_id) {
    auto provider = get_speech_provider(provider_id);
    if (!provider) return normalize_speech_provider_id(provider_id);
    return provider->name();
}

// =========================================================================
// 对齐 OC directives.ts: TTS Directive Parser
// =========================================================================

struct TtsDirectiveOverrides {
    std::optional<std::string> tts_text;
    std::optional<std::string> provider_id;
    std::optional<std::string> voice;
    std::optional<double> speed;
    std::optional<std::string> language_code;
};

struct TtsDirectiveParseResult {
    std::string cleaned_text;
    TtsDirectiveOverrides overrides;
    std::vector<std::string> warnings;
    bool has_directive = false;
};

struct SpeechModelOverridePolicy {
    bool enabled = false;
    bool allow_text = false;
    bool allow_provider = false;
    bool allow_voice = false;
    bool allow_speed = false;
    bool allow_language = false;
};

// 对齐 OC: parseTtsDirectives
TtsDirectiveParseResult parse_tts_directives(
        const std::string& text,
        const SpeechModelOverridePolicy& policy) {

    TtsDirectiveParseResult result;
    result.cleaned_text = text;

    if (!policy.enabled) {
        return result;
    }

    // 对齐 OC: [[tts:text]] ... [[/tts:text]] block parsing
    std::regex block_regex(R"(\[\[tts:text\]\]([\s\S]*?)\[\[\/tts:text\]\])",
                           std::regex::icase);
    std::smatch block_match;
    std::string working = text;

    if (std::regex_search(working, block_match, block_regex)) {
        result.has_directive = true;
        if (policy.allow_text && !result.overrides.tts_text.has_value()) {
            std::string inner = block_match[1].str();
            // Trim
            size_t start = inner.find_first_not_of(" \t\n\r");
            size_t end = inner.find_last_not_of(" \t\n\r");
            if (start != std::string::npos) {
                result.overrides.tts_text = inner.substr(start, end - start + 1);
            }
        }
        working = block_match.prefix().str() + block_match.suffix().str();
    }

    // 对齐 OC: [[tts:provider=xxx]] inline directives
    std::regex provider_regex(R"(\[\[tts:provider=([^\]]+)\]\])", std::regex::icase);
    std::smatch provider_match;
    if (std::regex_search(working, provider_match, provider_regex)) {
        result.has_directive = true;
        if (policy.allow_provider) {
            result.overrides.provider_id = provider_match[1].str();
        }
        working = provider_match.prefix().str() + provider_match.suffix().str();
    }

    // 对齐 OC: [[tts:voice=xxx]]
    std::regex voice_regex(R"(\[\[tts:voice=([^\]]+)\]\])", std::regex::icase);
    std::smatch voice_match;
    if (std::regex_search(working, voice_match, voice_regex)) {
        result.has_directive = true;
        if (policy.allow_voice) {
            result.overrides.voice = voice_match[1].str();
        }
        working = voice_match.prefix().str() + voice_match.suffix().str();
    }

    // 对齐 OC: [[tts:speed=xxx]]
    std::regex speed_regex(R"(\[\[tts:speed=([^\]]+)\]\])", std::regex::icase);
    std::smatch speed_match;
    if (std::regex_search(working, speed_match, speed_regex)) {
        result.has_directive = true;
        if (policy.allow_speed) {
            try {
                double speed = std::stod(speed_match[1].str());
                require_in_range(speed, 0.25, 4.0, "tts:speed");
                result.overrides.speed = speed;
            } catch (...) {
                result.warnings.push_back("Invalid tts:speed value");
            }
        }
        working = speed_match.prefix().str() + speed_match.suffix().str();
    }

    // 对齐 OC: [[tts:lang=xxx]]
    std::regex lang_regex(R"(\[\[tts:lang=([^\]]+)\]\])", std::regex::icase);
    std::smatch lang_match;
    if (std::regex_search(working, lang_match, lang_regex)) {
        result.has_directive = true;
        if (policy.allow_language) {
            try {
                result.overrides.language_code = normalize_language_code(lang_match[1].str());
            } catch (...) {
                result.warnings.push_back("Invalid tts:lang value");
            }
        }
        working = lang_match.prefix().str() + lang_match.suffix().str();
    }

    result.cleaned_text = working;
    return result;
}

}  // namespace tts
}  // namespace hunterclaw
