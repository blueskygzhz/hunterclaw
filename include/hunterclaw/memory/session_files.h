#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <map>

// 严格对齐 OpenClaw src/memory/session-files.ts

namespace hunterclaw {
namespace memory {

// ============================================================================
// SessionFileEntry (对齐 session-files.ts:10-19)
// ============================================================================
struct SessionFileEntry {
    std::string path;           // Relative path: "sessions/xxx.jsonl"
    std::string abs_path;       // Absolute file system path
    int64_t mtime_ms;          // Modification time
    size_t size;               // File size in bytes
    std::string hash;          // Content hash
    std::string content;       // Extracted text content
    std::vector<int> line_map; // Maps each content line to JSONL source line
};

// ============================================================================
// Session文件处理函数 (对齐 session-files.ts)
// ============================================================================

/**
 * 列出指定Agent的所有Session文件 (对齐 listSessionFilesForAgent)
 * @param agent_id Agent ID
 * @return Session文件绝对路径列表
 */
core::Result<std::vector<std::string>> list_session_files_for_agent(
    const std::string& agent_id
);

/**
 * 将绝对路径转换为Session相对路径 (对齐 sessionPathForFile)
 * @param abs_path 绝对路径
 * @return "sessions/xxx.jsonl"
 */
std::string session_path_for_file(const std::string& abs_path);

/**
 * 提取Session文本内容 (对齐 extractSessionText)
 * @param content JSON content (string或array)
 * @return Normalized text或empty
 */
std::string extract_session_text(const nlohmann::json& content);

/**
 * 构建Session文件条目 (对齐 buildSessionEntry)
 * @param abs_path Session JSONL文件绝对路径
 * @return SessionFileEntry或error
 */
core::Result<SessionFileEntry> build_session_entry(const std::string& abs_path);

// ============================================================================
// 内部辅助函数
// ============================================================================

/**
 * 标准化Session文本 (对齐 normalizeSessionText)
 * - 移除多余空白
 * - 折叠换行符
 */
std::string normalize_session_text(const std::string& value);

/**
 * Redact敏感信息 (简化版)
 * 对齐 redactSensitiveText
 */
std::string redact_sensitive_text(const std::string& text);

} // namespace memory
} // namespace hunterclaw
