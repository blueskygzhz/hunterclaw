#pragma once

#include <optional>
#include <string>
#include <cstdint>

// 严格对齐 OpenClaw src/auto-reply/reply/abort-cutoff.ts (100行)
// Abort cutoff 时间戳管理 — 用于跳过 abort 前排队的消息

namespace hunterclaw {
namespace react {

// ============================================================================
// AbortCutoff (对齐 abort-cutoff.ts:8-20)
// ============================================================================
struct AbortCutoff {
    int64_t timestamp_ms = 0;   // abort 发出的时间戳
    std::string session_key;    // 被 abort 的 session key
};

// ============================================================================
// resolveAbortCutoffFromContext (对齐 abort-cutoff.ts:22-42)
// 从当前 context 解析 abort cutoff
// ============================================================================
std::optional<AbortCutoff> resolve_abort_cutoff_from_context(
    const std::string& command_session_key,
    int64_t now_ms = 0
);

// ============================================================================
// shouldPersistAbortCutoff (对齐 abort-cutoff.ts:44-58)
// 判断是否需要持久化 abort cutoff
// ============================================================================
bool should_persist_abort_cutoff(
    const std::string& command_session_key,
    const std::string& target_session_key
);

// ============================================================================
// applyAbortCutoffToSessionEntry (对齐 abort-cutoff.ts:60-80)
// 将 abort cutoff 应用到 session entry
// ============================================================================
struct SessionEntryAbortFields {
    std::optional<int64_t> abort_cutoff_ms;
    bool aborted_last_run = false;
};

SessionEntryAbortFields apply_abort_cutoff_to_session_entry(
    const AbortCutoff& cutoff
);

// ============================================================================
// shouldSkipMessageByAbortCutoff (对齐 abort-cutoff.ts:82-100)
// 判断消息是否应被 abort cutoff 跳过
// ============================================================================
bool should_skip_message_by_abort_cutoff(
    int64_t message_timestamp_ms,
    std::optional<int64_t> abort_cutoff_ms
);

}  // namespace react
}  // namespace hunterclaw
