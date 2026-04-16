// ✅ 严格对齐 OpenClaw: src/agents/subagent-session-metrics.ts (70行)
// 子代理会话指标实现

#include "hunterclaw/observability/subagent_session_metrics.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace hunterclaw {
namespace observability {

// ============================================================================
// Helper
// ============================================================================

static int64_t current_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static bool is_valid_timestamp(const std::optional<int64_t>& ts) {
    return ts.has_value() && std::isfinite(static_cast<double>(*ts));
}

// ============================================================================
// resolveSubagentSessionStartedAtInternal
// ✅ 严格对齐 OpenClaw: L4-16
// ============================================================================

static std::optional<int64_t> resolve_subagent_session_started_at_internal(
    const SubagentRunRecord& entry) {
    if (is_valid_timestamp(entry.session_started_at)) {
        return entry.session_started_at;
    }
    if (is_valid_timestamp(entry.started_at)) {
        return entry.started_at;
    }
    if (is_valid_timestamp(entry.created_at)) {
        return entry.created_at;
    }
    return std::nullopt;
}

// ============================================================================
// getSubagentSessionStartedAt — 严格对齐 OpenClaw: L18-22
// ============================================================================

std::optional<int64_t> get_subagent_session_started_at(
    const SubagentRunRecord* entry) {
    if (!entry) return std::nullopt;
    return resolve_subagent_session_started_at_internal(*entry);
}

// ============================================================================
// getSubagentSessionRuntimeMs — 严格对齐 OpenClaw: L24-47
// ============================================================================

std::optional<int64_t> get_subagent_session_runtime_ms(
    const SubagentRunRecord* entry, int64_t now_ms) {
    if (!entry) return std::nullopt;

    if (now_ms <= 0) {
        now_ms = current_time_ms();
    }

    int64_t accumulated_runtime_ms = 0;
    if (is_valid_timestamp(entry->accumulated_runtime_ms)) {
        accumulated_runtime_ms = std::max(int64_t(0), *entry->accumulated_runtime_ms);
    }

    if (!is_valid_timestamp(entry->started_at)) {
        return entry->accumulated_runtime_ms.has_value()
            ? std::make_optional(accumulated_runtime_ms)
            : std::nullopt;
    }

    int64_t current_run_ended_at =
        is_valid_timestamp(entry->ended_at) ? *entry->ended_at : now_ms;

    return std::max(int64_t(0),
        accumulated_runtime_ms + std::max(int64_t(0), current_run_ended_at - *entry->started_at));
}

// ============================================================================
// resolveSubagentSessionStatus — 严格对齐 OpenClaw: L49-69
// ============================================================================

std::string resolve_subagent_session_status(const SubagentRunRecord* entry) {
    if (!entry) return "";

    // ✅ 对齐 OpenClaw: !entry.endedAt → "running"
    if (!entry->ended_at.has_value()) {
        return "running";
    }

    // ✅ 对齐 OpenClaw: endedReason === SUBAGENT_ENDED_REASON_KILLED → "killed"
    if (entry->ended_reason == SUBAGENT_ENDED_REASON_KILLED) {
        return "killed";
    }

    // ✅ 对齐 OpenClaw: outcome?.status checks
    if (entry->outcome.has_value()) {
        const auto& status = entry->outcome->status;
        if (status == "error") return "failed";
        if (status == "timeout") return "timeout";
    }

    return "done";
}

}  // namespace observability
}  // namespace hunterclaw
