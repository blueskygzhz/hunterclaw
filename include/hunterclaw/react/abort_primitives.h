#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>

// 严格对齐 OpenClaw src/auto-reply/reply/abort-primitives.ts (90行)
// Abort 原语：isAbortTrigger, abort memory, abort request text detection

namespace hunterclaw {
namespace react {

// ============================================================================
// isAbortTrigger (对齐 abort-primitives.ts:5-22)
// 检测消息是否为 abort 触发器
// ============================================================================
bool is_abort_trigger(const std::string& text);

// ============================================================================
// isAbortRequestText (对齐 abort-primitives.ts:24-40)
// 检测是否为 "/abort", "/stop", "stop" 等命令文本
// ============================================================================
bool is_abort_request_text(const std::string& text);

// ============================================================================
// Abort Memory (对齐 abort-primitives.ts:42-80)
// Per-session 内存中的 abort 状态追踪
// ============================================================================

// 获取 abort 状态 (对齐 getAbortMemory)
std::optional<bool> get_abort_memory(const std::string& key);

// 设置 abort 状态 (对齐 setAbortMemory)
void set_abort_memory(const std::string& key, bool aborted);

// 获取 abort memory 大小 (for testing)
size_t get_abort_memory_size();

// 重置 (for testing)
void reset_abort_memory();

}  // namespace react
}  // namespace hunterclaw
