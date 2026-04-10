#pragma once

// ============================================================================
// Session Memory Hook — 严格对齐 OpenClaw session-memory/ (handler.ts + transcript.ts)
//
// 会话记忆钩子系统。在 /new 或 /reset 命令时，自动将会话内容保存为
// memory/YYYY-MM-DD-slug.md 文件。
//
// 核心功能:
//   - getRecentSessionContent: 从 JSONL 会话文件提取最近 N 条消息
//   - getRecentSessionContentWithResetFallback: 主文件为空时回退到 .reset.* 文件
//   - findPreviousSessionFile: 查找前一个会话文件
//   - saveSessionToMemory: 完整的 hook handler
// ============================================================================

#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Transcript helpers (对齐 transcript.ts)
// ============================================================================

/**
 * Read recent session content from a JSONL session file.
 * Extracts the last N user/assistant messages, filtering out:
 *   - Non-message entries (tool calls, system)
 *   - Inter-session user messages (provenance.kind === "inter_session")
 *   - Command messages starting with "/"
 *
 * (对齐 transcript.ts:24-62 getRecentSessionContent)
 */
std::optional<std::string> get_recent_session_content(
    const std::string& session_file_path,
    int message_count = 15
);

/**
 * Like getRecentSessionContent but falls back to the newest .reset.* transcript
 * when the primary file is empty.
 *
 * (对齐 transcript.ts:64-89 getRecentSessionContentWithResetFallback)
 */
std::optional<std::string> get_recent_session_content_with_reset_fallback(
    const std::string& session_file_path,
    int message_count = 15
);

/**
 * Strip .reset.* suffix from a session filename.
 * (对齐 transcript.ts:91-94 stripResetSuffix)
 */
std::string strip_reset_suffix(const std::string& file_name);

/**
 * Find the previous session file in the sessions directory.
 * Priority: strip-reset base → sessionId.jsonl → topic variants → newest non-reset jsonl
 *
 * (对齐 transcript.ts:96-148 findPreviousSessionFile)
 */
std::optional<std::string> find_previous_session_file(
    const std::string& sessions_dir,
    const std::optional<std::string>& current_session_file = std::nullopt,
    const std::optional<std::string>& session_id = std::nullopt
);

// ============================================================================
// Hook handler (对齐 handler.ts)
// ============================================================================

struct SessionMemoryHookEvent {
    std::string type;           // "command" | "agent"
    std::string action;         // "new" | "reset" | "help" | ...
    std::string session_key;
    int64_t timestamp = 0;      // Unix millis
    
    // Context
    std::string workspace_dir;
    std::string session_id;
    std::string session_file;
    std::string command_source;
    int message_count = 15;     // Configurable message limit
};

/**
 * Save session context to memory when /new or /reset command is triggered.
 * Creates memory/YYYY-MM-DD-slug.md in the workspace directory.
 *
 * (对齐 handler.ts:53-223 saveSessionToMemory)
 *
 * @param event Hook event with session context
 * @param slug_generator Optional callback to generate descriptive slug from content
 * @return Path to created memory file, or empty on skip/error
 */
std::optional<std::string> save_session_to_memory(
    const SessionMemoryHookEvent& event,
    std::function<std::string(const std::string& content)> slug_generator = nullptr
);

}  // namespace memory
}  // namespace hunterclaw
