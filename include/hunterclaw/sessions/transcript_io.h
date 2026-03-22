#pragma once

#include "hunterclaw/llm/llm_client.h"
#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {
namespace sessions {

/**
 * Transcript I/O - 对齐 OpenClaw transcript 读写逻辑
 * 
 * 参考:
 * - openclaw-analysis/src/gateway/server-methods/chat-transcript-inject.ts
 * - openclaw-analysis/src/config/sessions/transcript.ts
 */

// ============================================================================
// 路径解析 (对齐 OpenClaw)
// ============================================================================

/**
 * 解析 session transcript 文件路径
 * 对齐: openclaw-analysis/src/gateway/server-methods/chat.ts:580
 */
std::string resolve_transcript_path(
    const std::string& session_id,
    const std::string& agent_id,
    const std::string& sessions_dir
);

// ============================================================================
// Transcript 文件初始化
// ============================================================================

struct EnsureTranscriptResult {
    bool ok;
    std::string error;
};

/**
 * 确保 transcript 文件存在 (不存在则创建)
 * 对齐: openclaw-analysis/src/gateway/server-methods/chat.ts:602
 */
EnsureTranscriptResult ensure_transcript_file(
    const std::string& transcript_path,
    const std::string& session_id
);

// ============================================================================
// Transcript 写入 (对齐 OpenClaw)
// ============================================================================

struct AppendTranscriptResult {
    bool ok;
    std::string message_id;
    std::string error;
};

struct AppendTranscriptOptions {
    std::string message;
    std::string label;
    std::string idempotency_key;
    bool create_if_missing = false;
    
    // Abort 元数据 (对齐 OpenClaw)
    bool aborted = false;
    std::string abort_origin;  // "rpc" | "stop-command"
    std::string abort_run_id;
};

/**
 * 追加 assistant 消息到 transcript (对齐 OpenClaw)
 * 
 * 使用 SessionManager 风格追加 (保持 parentId 链)
 * 对齐: openclaw-analysis/src/gateway/server-methods/chat-transcript-inject.ts:18
 */
AppendTranscriptResult append_assistant_message_to_transcript(
    const std::string& transcript_path,
    const AppendTranscriptOptions& options
);

/**
 * 追加用户消息到 transcript
 */
AppendTranscriptResult append_user_message_to_transcript(
    const std::string& transcript_path,
    const std::string& message,
    const std::string& run_id
);

// ============================================================================
// Tool Result Persist (对齐 OpenClaw tool_result_persist hook)
// ============================================================================

struct AppendToolResultOptions {
    std::string tool_name;
    std::string tool_call_id;
    std::string result_json;   // Serialized tool result content
    bool is_synthetic = false; // True when synthesized by guard/repair step
    std::string run_id;
};

/**
 * 追加工具结果消息到 transcript，并触发 tool_result_persist hook
 *
 * Aligned with OpenClaw sessions/transcript.ts appendToolResultMessage()
 * Hook: tool_result_persist — sync, sequential, allows plugins to modify
 *       the toolResult message before it is written to JSONL.
 */
AppendTranscriptResult append_tool_result_to_transcript(
    const std::string& transcript_path,
    const AppendToolResultOptions& options
);

// ============================================================================
// Transcript 读取 (对齐 OpenClaw)
// ============================================================================

/**
 * 从 transcript 读取消息历史
 * 对齐: openclaw-analysis/src/gateway/session-utils.ts
 * 
 * @param limit 最多返回多少条消息 (从文件末尾向前读)
 */
std::vector<llm::LLMMessage> read_transcript_messages(
    const std::string& transcript_path,
    int limit = 200
);

// ============================================================================
// 幂等性检查 (对齐 OpenClaw)
// ============================================================================

/**
 * 检查 transcript 是否已经包含指定 idempotency_key
 * 对齐: openclaw-analysis/src/gateway/server-methods/chat.ts:628
 */
bool transcript_has_idempotency_key(
    const std::string& transcript_path,
    const std::string& idempotency_key
);

}  // namespace sessions
}  // namespace hunterclaw
