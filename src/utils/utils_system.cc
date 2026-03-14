#include "utils/utils_system.h"
#include <regex>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace hunterclaw {
namespace utils {

// ============================================================================
// 1. Delivery Context Implementation
// ============================================================================

json DeliveryContext::to_json() const {
    json j;
    if (channel) j["channel"] = *channel;
    if (to) j["to"] = *to;
    if (account_id) j["accountId"] = *account_id;
    if (thread_id) j["threadId"] = *thread_id;
    return j;
}

std::optional<DeliveryContext> DeliveryContext::from_json(const json& j) {
    if (!j.is_object()) return std::nullopt;
    
    DeliveryContext ctx;
    if (j.contains("channel") && j["channel"].is_string()) {
        ctx.channel = j["channel"].get<std::string>();
    }
    if (j.contains("to") && j["to"].is_string()) {
        ctx.to = j["to"].get<std::string>();
    }
    if (j.contains("accountId") && j["accountId"].is_string()) {
        ctx.account_id = j["accountId"].get<std::string>();
    }
    if (j.contains("threadId")) {
        if (j["threadId"].is_string()) {
            ctx.thread_id = j["threadId"].get<std::string>();
        } else if (j["threadId"].is_number()) {
            ctx.thread_id = std::to_string(j["threadId"].get<int64_t>());
        }
    }
    
    return ctx;
}

std::optional<DeliveryContext> DeliveryContextUtils::normalize(const DeliveryContext& context) {
    DeliveryContext normalized;
    bool has_any = false;
    
    if (context.channel && !context.channel->empty()) {
        normalized.channel = *context.channel;
        has_any = true;
    }
    if (context.to && !context.to->empty()) {
        normalized.to = *context.to;
        has_any = true;
    }
    if (context.account_id && !context.account_id->empty()) {
        normalized.account_id = *context.account_id;
        has_any = true;
    }
    if (context.thread_id && !context.thread_id->empty()) {
        normalized.thread_id = *context.thread_id;
        has_any = true;
    }
    
    return has_any ? std::make_optional(normalized) : std::nullopt;
}

DeliveryContext DeliveryContextUtils::merge(
    const std::optional<DeliveryContext>& base,
    const std::optional<DeliveryContext>& override) {
    
    DeliveryContext result;
    
    if (base) {
        result = *base;
    }
    
    if (override) {
        if (override->channel) result.channel = override->channel;
        if (override->to) result.to = override->to;
        if (override->account_id) result.account_id = override->account_id;
        if (override->thread_id) result.thread_id = override->thread_id;
    }
    
    return result;
}

// ============================================================================
// 2. API Key Masker Implementation
// ============================================================================

std::string ApiKeyMasker::mask_api_key(const std::string& value) {
    std::string trimmed = value;
    // Trim whitespace
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
    
    if (trimmed.empty()) {
        return "missing";
    }
    
    size_t len = trimmed.length();
    if (len <= 6) {
        return trimmed.substr(0, 1) + "..." + trimmed.substr(len - 1);
    } else if (len <= 16) {
        return trimmed.substr(0, 2) + "..." + trimmed.substr(len - 2);
    } else {
        return trimmed.substr(0, 8) + "..." + trimmed.substr(len - 8);
    }
}

std::string ApiKeyMasker::mask_keys_in_text(const std::string& text,
                                            const std::vector<std::string>& keys) {
    std::string result = text;
    for (const auto& key : keys) {
        if (!key.empty()) {
            std::string masked = mask_api_key(key);
            size_t pos = 0;
            while ((pos = result.find(key, pos)) != std::string::npos) {
                result.replace(pos, key.length(), masked);
                pos += masked.length();
            }
        }
    }
    return result;
}

std::string ApiKeyMasker::auto_mask_text(const std::string& text) {
    // Common API key patterns
    std::regex api_key_pattern(R"((sk-[a-zA-Z0-9]{32,}|[a-zA-Z0-9]{32,}))");
    std::string result = text;
    std::smatch match;
    
    std::string temp = result;
    while (std::regex_search(temp, match, api_key_pattern)) {
        std::string key = match[1].str();
        std::string masked = mask_api_key(key);
        size_t pos = result.find(key);
        if (pos != std::string::npos) {
            result.replace(pos, key.length(), masked);
        }
        temp = match.suffix();
    }
    
    return result;
}

// ============================================================================
// 3. Safe JSON Implementation
// ============================================================================

std::optional<std::string> SafeJSON::stringify(const json& value) {
    try {
        return value.dump();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<json> SafeJSON::parse(const std::string& str) {
    try {
        return json::parse(str);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> SafeJSON::stringify_with_replacer(const json& value, Replacer replacer) {
    try {
        json processed = value;
        // Apply replacer recursively
        std::function<void(json&, const std::string&)> process = [&](json& j, const std::string& path) {
            if (j.is_object()) {
                for (auto it = j.begin(); it != j.end(); ++it) {
                    std::string key = it.key();
                    it.value() = replacer(key, it.value());
                    process(it.value(), path + "." + key);
                }
            } else if (j.is_array()) {
                for (size_t i = 0; i < j.size(); ++i) {
                    j[i] = replacer(std::to_string(i), j[i]);
                    process(j[i], path + "[" + std::to_string(i) + "]");
                }
            }
        };
        process(processed, "");
        return processed.dump();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<json> SafeJSON::deep_clone(const json& value) {
    try {
        return json::parse(value.dump());
    } catch (...) {
        return std::nullopt;
    }
}

// ============================================================================
// 4. Directive Tags Implementation
// ============================================================================

InlineDirectiveParseResult DirectiveTags::parse_inline_directives(
    const std::string& text,
    const InlineDirectiveParseOptions& options) {
    
    InlineDirectiveParseResult result;
    result.text = text;
    
    if (text.empty() || text.find("[[") == std::string::npos) {
        return result;
    }
    
    // Parse [[audio_as_voice]]
    std::regex audio_tag_re(R"(\[\[\s*audio_as_voice\s*\]\])", std::regex::icase);
    if (std::regex_search(text, audio_tag_re)) {
        result.audio_as_voice = true;
        result.has_audio_tag = true;
        if (options.strip_audio_tag) {
            result.text = std::regex_replace(result.text, audio_tag_re, "");
        }
    }
    
    // Parse [[reply_to_current]] and [[reply_to:ID]]
    std::regex reply_tag_re(R"(\[\[\s*(?:reply_to_current|reply_to\s*:\s*([^\]\n]+))\s*\]\])", std::regex::icase);
    std::smatch match;
    std::string temp = result.text;
    
    if (std::regex_search(temp, match, reply_tag_re)) {
        result.has_reply_tag = true;
        
        if (match[1].matched) {
            // [[reply_to:ID]]
            result.reply_to_explicit_id = match[1].str();
            result.reply_to_id = match[1].str();
        } else {
            // [[reply_to_current]]
            result.reply_to_current = true;
            if (options.current_message_id) {
                result.reply_to_id = *options.current_message_id;
            }
        }
        
        if (options.strip_reply_tags) {
            result.text = std::regex_replace(result.text, reply_tag_re, "");
        }
    }
    
    // Trim whitespace
    result.text.erase(0, result.text.find_first_not_of(" \t\n\r"));
    result.text.erase(result.text.find_last_not_of(" \t\n\r") + 1);
    
    return result;
}

StripInlineDirectiveTagsResult DirectiveTags::strip_inline_directive_tags_for_display(
    const std::string& text) {
    
    if (text.empty()) {
        return {text, false};
    }
    
    std::string result = text;
    
    // Strip audio tag
    std::regex audio_tag_re(R"(\[\[\s*audio_as_voice\s*\]\])", std::regex::icase);
    result = std::regex_replace(result, audio_tag_re, "");
    
    // Strip reply tags
    std::regex reply_tag_re(R"(\[\[\s*(?:reply_to_current|reply_to\s*:\s*([^\]\n]+))\s*\]\])", std::regex::icase);
    result = std::regex_replace(result, reply_tag_re, "");
    
    bool changed = (result != text);
    return {result, changed};
}

json DirectiveTags::strip_inline_directive_tags_from_message_for_display(const json& message) {
    if (!message.is_object() || !message.contains("content") || !message["content"].is_array()) {
        return message;
    }
    
    json result = message;
    json& content = result["content"];
    
    for (auto& part : content) {
        if (part.is_object() && part.contains("type") && part["type"] == "text" &&
            part.contains("text") && part["text"].is_string()) {
            auto stripped = strip_inline_directive_tags_for_display(part["text"].get<std::string>());
            part["text"] = stripped.text;
        }
    }
    
    return result;
}

bool DirectiveTags::contains_directives(const std::string& text) {
    return text.find("[[") != std::string::npos;
}

// ============================================================================
// 5. Provider Utils Implementation
// ============================================================================

bool ProviderUtils::is_reasoning_tag_provider(const std::string& provider) {
    if (provider.empty()) return false;
    
    std::string normalized = provider;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    
    // Google providers require reasoning tags
    if (normalized == "google" || normalized == "google-gemini-cli" ||
        normalized == "google-generative-ai") {
        return true;
    }
    
    // Minimax providers
    if (normalized.find("minimax") != std::string::npos) {
        return true;
    }
    
    return false;
}

bool ProviderUtils::supports_streaming(const std::string& provider) {
    // Most providers support streaming
    return !provider.empty();
}

bool ProviderUtils::supports_function_calling(const std::string& provider) {
    std::string normalized = provider;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    
    // Known function calling providers
    if (normalized.find("openai") != std::string::npos ||
        normalized.find("anthropic") != std::string::npos ||
        normalized.find("google") != std::string::npos) {
        return true;
    }
    
    return false;
}

bool ProviderUtils::supports_vision(const std::string& provider) {
    std::string normalized = provider;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    
    // Known vision providers
    if (normalized.find("gpt-4") != std::string::npos ||
        normalized.find("claude-3") != std::string::npos ||
        normalized.find("gemini") != std::string::npos) {
        return true;
    }
    
    return false;
}

ProviderUtils::ProviderCapabilities ProviderUtils::get_capabilities(const std::string& provider) {
    ProviderCapabilities caps;
    caps.streaming = supports_streaming(provider);
    caps.function_calling = supports_function_calling(provider);
    caps.vision = supports_vision(provider);
    caps.reasoning_tags = is_reasoning_tag_provider(provider);
    
    // Set context windows (examples)
    std::string normalized = provider;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    
    if (normalized.find("gpt-4") != std::string::npos) {
        caps.context_window = 128000;
        caps.max_tokens = 4096;
    } else if (normalized.find("claude") != std::string::npos) {
        caps.context_window = 200000;
        caps.max_tokens = 8192;
    }
    
    return caps;
}

// ============================================================================
// 6. Message Channel Utils Implementation
// ============================================================================

std::optional<std::string> MessageChannelUtils::normalize_message_channel(const std::string& channel) {
    std::string normalized = channel;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    
    // Trim whitespace
    normalized.erase(0, normalized.find_first_not_of(" \t\n\r"));
    normalized.erase(normalized.find_last_not_of(" \t\n\r") + 1);
    
    if (normalized.empty()) return std::nullopt;
    
    // Known channel mappings
    if (normalized == "discord" || normalized == "dis") return "discord";
    if (normalized == "slack") return "slack";
    if (normalized == "telegram" || normalized == "tg") return "telegram";
    if (normalized == "whatsapp" || normalized == "wa") return "whatsapp";
    if (normalized == "signal") return "signal";
    if (normalized == "imessage" || normalized == "im") return "imessage";
    if (normalized == "web") return "web";
    
    return normalized;
}

MessageChannelUtils::ChannelType MessageChannelUtils::get_channel_type(const std::string& channel) {
    auto normalized = normalize_message_channel(channel);
    if (!normalized) return ChannelType::Unknown;
    
    if (*normalized == "discord") return ChannelType::Discord;
    if (*normalized == "slack") return ChannelType::Slack;
    if (*normalized == "telegram") return ChannelType::Telegram;
    if (*normalized == "whatsapp") return ChannelType::WhatsApp;
    if (*normalized == "signal") return ChannelType::Signal;
    if (*normalized == "imessage") return ChannelType::iMessage;
    if (*normalized == "web") return ChannelType::Web;
    
    return ChannelType::Unknown;
}

bool MessageChannelUtils::is_valid_channel(const std::string& channel) {
    return normalize_message_channel(channel).has_value();
}

// ============================================================================
// 7-17. Other Utils Implementations (Simplified Stubs)
// ============================================================================

std::optional<std::string> AccountIdUtils::normalize_account_id(const std::string& account_id) {
    if (account_id.empty()) return std::nullopt;
    return account_id;
}

bool AccountIdUtils::is_valid_account_id(const std::string& account_id) {
    return !account_id.empty();
}

std::optional<bool> BooleanUtils::parse_boolean(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") return true;
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") return false;
    
    return std::nullopt;
}

bool BooleanUtils::parse_boolean_or(const std::string& value, bool default_val) {
    auto result = parse_boolean(value);
    return result.value_or(default_val);
}

std::vector<std::string> ShellArgvParser::parse_shell_argv(const std::string& command) {
    std::vector<std::string> argv;
    std::istringstream iss(command);
    std::string arg;
    
    while (iss >> std::quoted(arg)) {
        argv.push_back(arg);
    }
    
    return argv;
}

std::string ShellArgvParser::quote_shell_arg(const std::string& arg) {
    if (arg.find(' ') != std::string::npos || arg.find('\t') != std::string::npos) {
        return "\"" + arg + "\"";
    }
    return arg;
}

std::string UsageFormat::format_usage(const TokenUsage& usage) {
    std::ostringstream oss;
    oss << "Tokens: " << total_tokens(usage)
        << " (input: " << usage.input_tokens
        << ", output: " << usage.output_tokens;
    
    if (usage.cache_read_tokens > 0) {
        oss << ", cache read: " << usage.cache_read_tokens;
    }
    if (usage.cache_creation_tokens > 0) {
        oss << ", cache creation: " << usage.cache_creation_tokens;
    }
    
    oss << ")";
    return oss.str();
}

int UsageFormat::total_tokens(const TokenUsage& usage) {
    return usage.input_tokens + usage.output_tokens +
           usage.cache_creation_tokens + usage.cache_read_tokens;
}

// ============================================================================
// Utils System Implementation
// ============================================================================

UtilsSystem::UtilsSystem() = default;
UtilsSystem::~UtilsSystem() = default;

bool UtilsSystem::initialize() {
    if (initialized_) return true;
    
    // Initialize all subsystems
    initialized_ = true;
    return true;
}

void UtilsSystem::shutdown() {
    if (!initialized_) return;
    initialized_ = false;
}

} // namespace utils
} // namespace hunterclaw
