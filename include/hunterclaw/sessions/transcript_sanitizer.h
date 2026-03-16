#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/llm/llm_client.h"
#include <string>
#include <vector>
#include <map>

namespace hunterclaw {
namespace sessions {
namespace sanitizer {

/**
 * @brief 清理模式
 */
enum class SanitizeMode {
    Full,         // 完全清理（移除所有图片）
    ImagesOnly    // 仅清理图片内容（保留结构）
};

/**
 * @brief Tool Call ID 清理模式
 */
enum class ToolCallIdMode {
    None,      // 不清理
    Strict,    // 严格字母数字（[A-Za-z0-9]+）
    Strict9    // 严格 9 位字母数字（Mistral）
};

/**
 * @brief 清理选项配置
 */
struct SanitizeOptions {
    // 图片清理
    SanitizeMode sanitize_mode = SanitizeMode::Full;
    
    // Tool Call ID 清理
    ToolCallIdMode tool_call_id_mode = ToolCallIdMode::Strict;
    
    // 敏感信息脱敏
    bool redact_sessions_spawn_attachments = true;
    bool redact_large_content = true;
    int max_content_size = 100000;  // 100KB
    
    // 内容过滤
    bool drop_thinking_blocks = false;
    bool preserve_signatures = true;
    
    // Provider 特定
    bool apply_google_turn_ordering = false;
    bool validate_anthropic_turns = true;
};

/**
 * @brief Transcript 清理器
 * 
 * 对齐 OpenClaw 的 transcript-policy.ts 和 session-transcript-repair.ts
 */
class TranscriptSanitizer {
public:
    /**
     * @brief 完整清理消息列表
     */
    static std::vector<llm::LLMMessage> sanitize(
        const std::vector<llm::LLMMessage>& messages,
        const SanitizeOptions& options
    );
    
    /**
     * @brief 清理 Tool Call IDs
     * 
     * 对齐 OpenClaw: sanitizeToolCallIds()
     */
    static std::vector<llm::LLMMessage> sanitize_tool_call_ids(
        const std::vector<llm::LLMMessage>& messages,
        ToolCallIdMode mode
    );
    
    /**
     * @brief 清理图片内容
     * 
     * 对齐 OpenClaw: sanitizeImageContent()
     */
    static std::vector<llm::LLMMessage> sanitize_images(
        const std::vector<llm::LLMMessage>& messages,
        SanitizeMode mode
    );
    
    /**
     * @brief 脱敏 sessions_spawn 参数中的 attachments
     * 
     * 对齐 OpenClaw: redactSessionsSpawnAttachmentsArgs()
     */
    static core::JsonValue redact_sessions_spawn_args(
        const core::JsonValue& args
    );
    
    /**
     * @brief 脱敏大内容
     */
    static std::string redact_large_content(
        const std::string& content,
        int max_size
    );
    
    /**
     * @brief 根据 Provider 解析清理策略
     * 
     * 对齐 OpenClaw: resolveTranscriptPolicy()
     */
    static SanitizeOptions resolve_policy_for_provider(
        const std::string& provider,
        const std::string& model_id = ""
    );
    
private:
    /**
     * @brief 生成字母数字 ID
     */
    static std::string generate_alphanumeric_id(int length = 16);
    
    /**
     * @brief 生成严格 9 位字母数字 ID (Mistral)
     */
    static std::string generate_strict9_id();
    
    /**
     * @brief 检查 provider 类型
     */
    static bool is_google_provider(const std::string& provider);
    static bool is_anthropic_provider(const std::string& provider);
    static bool is_mistral_provider(const std::string& provider);
    static bool is_openai_provider(const std::string& provider);
};

} // namespace sanitizer
} // namespace sessions
} // namespace hunterclaw
