#pragma once

// 严格对齐 OpenClaw: src/tasks/task-completion-contract.ts
// Defines task terminal outcome contracts used by completion handling.

#include "hunterclaw/tasks/task_registry_types.h"

#include <optional>
#include <string>

namespace hunterclaw {
namespace tasks {

/// Terminal fields required when a mandatory detached task completion is invalid.
/// 对齐 OC: type RequiredCompletionTerminalResult
/// terminal_outcome 只可能是 BLOCKED（对齐 OC 的 Extract<TaskTerminalOutcome, "blocked">）
struct RequiredCompletionTerminalResult {
    std::optional<TaskTerminalOutcome> terminal_outcome;
    std::optional<std::string> terminal_summary;
};

/// Collapse whitespace runs to single spaces and trim.
/// 对齐 OC: normalizeCompletionText —— value?.replace(/\s+/g, " ").trim() ?? ""
std::string normalize_completion_text(const std::string& value);

/// UTF-8 safe truncation that never splits a multi-byte sequence.
/// 对齐 OC: truncateUtf16Safe（真源保证不切断代理对，此处保证不切断 UTF-8 序列）
std::string truncate_utf8_safe(const std::string& value, size_t max_bytes);

/// Detects "I'll go check on that" style text that reports intent, not a deliverable.
/// 对齐 OC: isProgressOnlyCompletionText
bool is_progress_only_completion_text(const std::string& value);

/// Resolves blocked terminal state for a mandatory completion.
/// 对齐 OC: resolveRequiredCompletionTerminalResult
RequiredCompletionTerminalResult resolve_required_completion_terminal_result(
    const std::string& result_text);

} // namespace tasks
} // namespace hunterclaw
