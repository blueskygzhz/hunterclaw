// ✅ 严格对齐 OpenClaw: src/hooks/bundled/command-logger/handler.ts (70行)
// 命令审计日志 Hook — 将所有命令事件写入 commands.log
#pragma once

#include <string>
#include <chrono>
#include <optional>

namespace hunterclaw {
namespace hooks {

// ============================================================================
// CommandEvent — 对齐 OpenClaw: HookEvent (command type)
// ============================================================================

struct CommandEvent {
    std::string type = "command";                        // 事件类型
    std::string action;                                  // "new" | "reset" | "stop" | etc.
    std::chrono::system_clock::time_point timestamp;     // 事件时间戳
    std::string session_key;                             // 会话标识
    std::string sender_id;                               // 发送者 ID
    std::string command_source;                          // "telegram" | "discord" | etc.
};

// ============================================================================
// Public API
// ============================================================================

// ✅ 对齐 OpenClaw: logCommand handler (handler.ts L39-67)
// 将命令事件写入审计日志文件
// 日志文件位置: ~/.hunterclaw/logs/commands.log
// 格式: JSONL (每行一个 JSON 对象)
void log_command_event(const CommandEvent& event);

// ✅ 对齐 OpenClaw: resolve log directory
std::string resolve_command_log_dir();

}  // namespace hooks
}  // namespace hunterclaw
