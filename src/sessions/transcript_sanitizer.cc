#include "hunterclaw/sessions/transcript_sanitizer.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <random>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace hunterclaw {
namespace sessions {
namespace sanitizer {

namespace {
    auto log() {
        static auto logger = spdlog::get("transcript");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("transcript");
        }
        return logger;
    }
}

// ============================================================================
// 主清理函数
// ============================================================================

std::vector<llm::LLMMessage> TranscriptSanitizer::sanitize(
    const std::vector<llm::LLMMessage>& messages,
    const SanitizeOptions& options
) {
    auto result = messages;
    
    // 1. 清理 Tool Call IDs
    if (options.tool_call_id_mode != ToolCallIdMode::None) {
        result = sanitize_tool_call_ids(result, options.tool_call_id_mode);
    }
    
    // 2. 清理图片内容
    result = sanitize_images(result, options.sanitize_mode);
    
    // 3. 脱敏敏感信息
    if (options.redact_sessions_spawn_attachments) {
        for (auto& msg : result) {
            if (msg.role == llm::LLMMessageRole::Assistant && !msg.tool_calls.empty()) {
                // 遍历所有 tool calls
                for (auto& call_val : msg.tool_calls) {
                    json call = call_val.to_json();
                    if (call.contains("name") && 
                        call["name"].get<std::string>() == "sessions_spawn") {
                        if (call.contains("arguments")) {
                            auto args = core::JsonValue(call["arguments"]);
                            call["arguments"] = redact_sessions_spawn_args(args).to_json();
                        }
                        call_val = core::JsonValue(call);
                    }
                }
            }
        }
    }
    
    // 4. 脱敏大内容
    if (options.redact_large_content) {
        for (auto& msg : result) {
            if (msg.content.size() > static_cast<size_t>(options.max_content_size)) {
                msg.content = redact_large_content(msg.content, options.max_content_size);
            }
        }
    }
    
    // 5. Google Turn Ordering (如果启用)
    if (options.apply_google_turn_ordering) {
        // 移除连续的 assistant 消息
        std::vector<llm::LLMMessage> cleaned;
        for (size_t i = 0; i < result.size(); ++i) {
            const auto& curr = result[i];
            const auto* next = (i + 1 < result.size()) ? &result[i + 1] : nullptr;
            
            if (curr.role == llm::LLMMessageRole::Assistant && 
                next && next->role == llm::LLMMessageRole::Assistant) {
                continue;  // 跳过重复的 assistant
            }
            
            cleaned.push_back(curr);
        }
        result = cleaned;
    }
    
    return result;
}

// ============================================================================
// Tool Call ID 清理
// ============================================================================

std::vector<llm::LLMMessage> TranscriptSanitizer::sanitize_tool_call_ids(
    const std::vector<llm::LLMMessage>& messages,
    ToolCallIdMode mode
) {
    if (mode == ToolCallIdMode::None) {
        return messages;
    }
    
    std::map<std::string, std::string> id_map;  // old_id -> new_id
    auto result = messages;
    
    // 第一遍: 替换 tool calls 中的 ID
    for (auto& msg : result) {
        if (msg.role == llm::LLMMessageRole::Assistant && !msg.tool_calls.empty()) {
            // 遍历所有 tool calls
            for (auto& call_val : msg.tool_calls) {
                json call = call_val.to_json();
                if (call.contains("id")) {
                    std::string old_id = call["id"].get<std::string>();
                    
                    // 生成新 ID
                    std::string new_id = (mode == ToolCallIdMode::Strict9) 
                        ? generate_strict9_id()
                        : generate_alphanumeric_id();
                    
                    id_map[old_id] = new_id;
                    call["id"] = new_id;
                    
                    log()->debug("Sanitize tool call ID: {} -> {}", old_id, new_id);
                }
                call_val = core::JsonValue(call);
            }
        }
    }
    
    // 第二遍: 更新 tool results 中的 tool_call_id
    for (auto& msg : result) {
        if (msg.role == llm::LLMMessageRole::Tool && !msg.tool_call_id.empty()) {
            std::string old_id = msg.tool_call_id;
            auto it = id_map.find(old_id);
            if (it != id_map.end()) {
                msg.tool_call_id = it->second;
                log()->debug("Update tool result ID: {} -> {}", old_id, it->second);
            }
        }
    }
    
    return result;
}

// ============================================================================
// 图片清理
// ============================================================================

std::vector<llm::LLMMessage> TranscriptSanitizer::sanitize_images(
    const std::vector<llm::LLMMessage>& messages,
    SanitizeMode mode
) {
    auto result = messages;
    
    for (auto& msg : result) {
        // 检查 content 是否包含 base64 图片
        if (msg.content.find("data:image/") != std::string::npos ||
            msg.content.find("base64,") != std::string::npos) {
            
            if (mode == SanitizeMode::Full) {
                // 完全移除图片内容
                msg.content = "[Image content removed]";
                log()->debug("Removed image content from message");
            } else if (mode == SanitizeMode::ImagesOnly) {
                // 保留结构，替换 base64 数据
                // 简化实现: 替换 base64 部分为 __REMOVED__
                size_t start = msg.content.find("base64,");
                if (start != std::string::npos) {
                    start += 7;  // "base64," 长度
                    size_t end = msg.content.find_first_of(" \n\"'", start);
                    if (end != std::string::npos) {
                        msg.content.replace(start, end - start, "__REMOVED__");
                    }
                }
                log()->debug("Redacted image base64 data");
            }
        }
    }
    
    return result;
}

// ============================================================================
// sessions_spawn 脱敏
// ============================================================================

core::JsonValue TranscriptSanitizer::redact_sessions_spawn_args(
    const core::JsonValue& args
) {
    auto j = args.to_json();
    
    if (j.is_object() && j.contains("attachments") && j["attachments"].is_array()) {
        for (auto& att : j["attachments"]) {
            if (att.is_object() && att.contains("content")) {
                att["content"] = "__OPENCLAW_REDACTED__";
            }
        }
        
        log()->debug("Redacted sessions_spawn attachments");
    }
    
    return core::JsonValue(j);
}

// ============================================================================
// 大内容脱敏
// ============================================================================

std::string TranscriptSanitizer::redact_large_content(
    const std::string& content,
    int max_size
) {
    if (static_cast<int>(content.size()) <= max_size) {
        return content;
    }
    
    std::string truncated = content.substr(0, max_size);
    truncated += "\n\n[Content truncated: " + 
                 std::to_string(content.size() - max_size) + 
                 " bytes removed]";
    
    log()->warn("Truncated large content: {} -> {} bytes", 
                content.size(), truncated.size());
    
    return truncated;
}

// ============================================================================
// Provider 策略解析
// ============================================================================

SanitizeOptions TranscriptSanitizer::resolve_policy_for_provider(
    const std::string& provider,
    const std::string& model_id
) {
    SanitizeOptions options;
    
    // 检测 provider 类型
    bool is_google = is_google_provider(provider);
    bool is_anthropic = is_anthropic_provider(provider);
    bool is_mistral = is_mistral_provider(provider);
    bool is_openai = is_openai_provider(provider);
    
    // 图片清理策略
    if (is_openai) {
        options.sanitize_mode = SanitizeMode::ImagesOnly;  // OpenAI: 仅清理内容
    } else {
        options.sanitize_mode = SanitizeMode::Full;  // 其他: 完全移除
    }
    
    // Tool Call ID 清理
    if (is_google || is_anthropic) {
        options.tool_call_id_mode = ToolCallIdMode::Strict;
    } else if (is_mistral) {
        options.tool_call_id_mode = ToolCallIdMode::Strict9;
    } else {
        options.tool_call_id_mode = ToolCallIdMode::None;  // OpenAI: 不清理
    }
    
    // Google 特定
    options.apply_google_turn_ordering = is_google;
    
    // Anthropic 特定
    options.validate_anthropic_turns = is_anthropic;
    
    log()->info("Resolved transcript policy for provider: {}, mode: {}, tool_call_id: {}", 
                provider, 
                (int)options.sanitize_mode,
                (int)options.tool_call_id_mode);
    
    return options;
}

// ============================================================================
// ID 生成辅助函数
// ============================================================================

std::string TranscriptSanitizer::generate_alphanumeric_id(int length) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);
    
    std::string id;
    id.reserve(length);
    
    for (int i = 0; i < length; ++i) {
        id += alphanum[dis(gen)];
    }
    
    return id;
}

std::string TranscriptSanitizer::generate_strict9_id() {
    return generate_alphanumeric_id(9);
}

// ============================================================================
// Provider 检测
// ============================================================================

bool TranscriptSanitizer::is_google_provider(const std::string& provider) {
    std::string lower = provider;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("google") != std::string::npos || 
           lower.find("gemini") != std::string::npos;
}

bool TranscriptSanitizer::is_anthropic_provider(const std::string& provider) {
    std::string lower = provider;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("anthropic") != std::string::npos || 
           lower.find("claude") != std::string::npos;
}

bool TranscriptSanitizer::is_mistral_provider(const std::string& provider) {
    std::string lower = provider;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("mistral") != std::string::npos;
}

bool TranscriptSanitizer::is_openai_provider(const std::string& provider) {
    std::string lower = provider;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("openai") != std::string::npos || 
           lower.find("gpt") != std::string::npos;
}

} // namespace sanitizer
} // namespace sessions
} // namespace hunterclaw
