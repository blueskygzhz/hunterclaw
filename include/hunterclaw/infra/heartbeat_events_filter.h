#pragma once

// ============================================================================
// heartbeat_events_filter.h
// Aligned with openclaw: src/infra/heartbeat-events-filter.ts
//
// Provides:
//   - build_cron_event_prompt            (aligned with buildCronEventPrompt)
//   - build_exec_event_prompt            (aligned with buildExecEventPrompt)
//   - is_exec_completion_event           (aligned with isExecCompletionEvent)
//   - is_relayable_exec_completion_event (aligned with isRelayableExecCompletionEvent)
//   - is_cron_system_event               (aligned with isCronSystemEvent)
// ============================================================================

#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace infra {

// ── build_cron_event_prompt ──────────────────────────────────────────────────
// Aligned with openclaw: buildCronEventPrompt

std::string build_cron_event_prompt(
    const std::vector<std::string>& pending_events,
    bool deliver_to_user = true
);

// ── build_exec_event_prompt ──────────────────────────────────────────────────
// Aligned with openclaw: buildExecEventPrompt
//
// pending_events 的文本会被直接嵌入 prompt（超过 8000 字符截断并追加
// "[truncated]"）。真源明确否决"结果见上方系统消息"这类写法——系统事件
// 队列并不注入 LLM 上下文，模型看不到。

std::string build_exec_event_prompt(
    const std::vector<std::string>& pending_events,
    bool deliver_to_user = true
);

// ── is_exec_completion_event ─────────────────────────────────────────────────
// Aligned with openclaw: isExecCompletionEvent
// 匹配两种形态：
//   1. /^exec finished(?::|\s*\()/i
//   2. 结构化事件 "exec (completed|failed) (<id>, code N|signal X)[ :: output]"

bool is_exec_completion_event(const std::string& evt);

// ── is_relayable_exec_completion_event ───────────────────────────────────────
// Aligned with openclaw: isRelayableExecCompletionEvent
// 有输出 → true；无输出但失败 → true；无输出且成功 → false（无需转达用户）。

bool is_relayable_exec_completion_event(const std::string& evt);

// ── is_cron_system_event ─────────────────────────────────────────────────────
// Aligned with openclaw: isCronSystemEvent
// Returns true when a system event should be treated as real cron reminder content.
// Excludes heartbeat noise events and exec completion events.

bool is_cron_system_event(const std::string& evt);

} // namespace infra
} // namespace hunterclaw
