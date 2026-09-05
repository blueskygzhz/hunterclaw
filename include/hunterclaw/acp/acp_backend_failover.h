#pragma once

// 严格对齐 OpenClaw: src/acp/control-plane/manager.backend-failover.ts
// Backend failover helpers for ACP session initialization and turn execution.

#include "hunterclaw/acp/runtime_errors.h"

#include <functional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace acp {

/// Captured backend attempt state used to decide whether failover is safe.
/// 对齐 OC: type BackendAttempt
struct BackendAttempt {
    std::string backend;
    std::string error;
    AcpErrorCode code = AcpErrorCode::ACP_TURN_FAILED;
    bool saw_output = false;
};

/// Ordered backend candidates plus display helper for diagnostics.
/// 对齐 OC: type BackendCandidatePlan
struct BackendCandidatePlan {
    /// 注意：真源用 Set 去重且**保留空串**（`configured ?? resolved ?? ""`），
    /// 空串代表"让 registry 自选默认 backend"，不能过滤掉，否则未配置
    /// backend 的会话会一个候选都不剩。
    std::vector<std::string> candidate_backends;
    std::function<std::string(const std::string&)> describe_backend_candidate;
};

/// Builds the deduped backend order from configured primary, resolved primary, and fallbacks.
/// 对齐 OC: resolveBackendCandidatePlan
BackendCandidatePlan resolve_backend_candidate_plan(
    const std::string& configured_primary_backend,
    const std::string& resolved_primary_backend,
    const std::vector<std::string>& fallback_backends);

/// Returns true for early transient backend errors where trying another backend is safe.
/// 对齐 OC: isFailoverWorthyBackendError
///
/// 两道闸：(1) 已经出过可见输出就不切换（避免重复输出）；
///        (2) 错误码属三类之一且文本命中瞬时故障词表。
bool is_failover_worthy_backend_error(const BackendAttempt& attempt);

/// Returns whether another backend candidate remains after the current index.
/// 对齐 OC: shouldAttemptBackendFailover
bool should_attempt_backend_failover(size_t backend_index,
                                     const std::vector<std::string>& candidate_backends);

} // namespace acp
} // namespace hunterclaw
