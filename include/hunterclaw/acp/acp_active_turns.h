#pragma once

// 严格对齐 OpenClaw:
//   - src/acp/control-plane/active-turns.ts
//   - src/acp/control-plane/manager.runtime-resume-state.ts
//
// Process-local active-turn registry + stale persistent session recovery.

#include "hunterclaw/acp/runtime_errors.h"

#include <functional>
#include <string>

namespace hunterclaw {
namespace acp {

// ── active-turns.ts ──────────────────────────────────────────────────────────
// Process-local liveness signal for in-flight ACP prompt turns.
// 对齐 OC 注释：task maintenance 用它回答"这个会话是否还有 turn 在跑？"，
// 避免把崩溃残留的会话条目误当活跃 run 回收。
// 刻意不挂在 AcpSessionManager 上 —— 这是维护态，插件不应读到。

/// 对齐 OC: markAcpTurnActive
void mark_acp_turn_active(const std::string& session_key);

/// 对齐 OC: clearAcpTurnActive
void clear_acp_turn_active(const std::string& session_key);

/// 对齐 OC: isAcpTurnActive
bool is_acp_turn_active(const std::string& session_key);

// ── manager.runtime-resume-state.ts ──────────────────────────────────────────

/// acpx detail code for a persistent session that can no longer be resumed.
/// 对齐 OC: SESSION_RESUME_REQUIRED_DETAIL_CODE
inline constexpr const char* ACP_SESSION_RESUME_REQUIRED_DETAIL_CODE =
    "SESSION_RESUME_REQUIRED";

/// Detects acpx exits that are safe to retry with a fresh runtime handle.
/// 对齐 OC: isRecoverableManagerAcpxExitError
/// 正则：/^acpx exited with (code \d+|signal [a-z0-9]+)/i
bool is_recoverable_manager_acpx_exit_error(const std::string& message);

/// Detects a "persistent session can no longer be resumed" failure.
/// 对齐 OC: isRecoverableMissingManagerPersistentSessionError
///
/// 真源按 detailCode 判定而非 reason 文本 —— 因为 Claude 报 "Resource not found"、
/// Kiro 报 "Internal error"(-32603)，文本匹配漏掉 Kiro 会让线程永久卡死(#87830)。
/// C++ 侧 AcpRuntimeError 无 cause 链，只查自身 detail_code。
bool is_recoverable_missing_persistent_session_error(const AcpRuntimeError& error);

/// Result of a fresh-handle retry decision. 对齐 OC 返回的 boolean + 副作用
struct FreshHandleRetryDecision {
    bool should_retry = false;
    /// true 表示需要清掉持久化的 resume 标识（identity → pending）
    bool needs_clear_resume_state = false;
};

/// Prepares a one-time fresh-handle retry for recoverable pre-output runtime failures.
/// 对齐 OC: prepareFreshManagerRuntimeHandleRetry
///
/// 只在 attempt==0 且尚无可见输出时生效（对齐真源 `attempt > 0 || sawTurnOutput`）——
/// 这是"一次性"重试，避免无限循环；已有输出时重试会造成重复输出。
FreshHandleRetryDecision prepare_fresh_runtime_handle_retry(
    int attempt,
    const AcpRuntimeError& error,
    bool saw_turn_output,
    bool has_runtime,
    bool is_persistent_mode);

} // namespace acp
} // namespace hunterclaw
