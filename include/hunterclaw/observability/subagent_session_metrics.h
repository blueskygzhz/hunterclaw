// ✅ 严格对齐 OpenClaw: src/agents/subagent-session-metrics.ts (70行)
// 子代理会话指标 — 计算子代理运行时长、会话开始时间、运行状态
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace hunterclaw {
namespace observability {

// ============================================================================
// SubagentRunRecord — 对齐 OpenClaw: SubagentRunRecord (partial)
// ============================================================================

struct SubagentRunRecord {
    std::optional<int64_t> session_started_at;
    std::optional<int64_t> started_at;
    std::optional<int64_t> created_at;
    std::optional<int64_t> ended_at;
    std::optional<int64_t> accumulated_runtime_ms;
    std::string ended_reason;
    struct Outcome {
        std::string status;  // "error" | "timeout" | "success"
    };
    std::optional<Outcome> outcome;
};

// ============================================================================
// Constants
// ============================================================================

// ✅ 对齐 OpenClaw: SUBAGENT_ENDED_REASON_KILLED
constexpr const char* SUBAGENT_ENDED_REASON_KILLED = "killed";

// ============================================================================
// Public API
// ============================================================================

// ✅ 对齐 OpenClaw: getSubagentSessionStartedAt
std::optional<int64_t> get_subagent_session_started_at(
    const SubagentRunRecord* entry);

// ✅ 对齐 OpenClaw: getSubagentSessionRuntimeMs
std::optional<int64_t> get_subagent_session_runtime_ms(
    const SubagentRunRecord* entry,
    int64_t now_ms = 0);

// ✅ 对齐 OpenClaw: resolveSubagentSessionStatus
// Returns: "running" | "killed" | "failed" | "timeout" | "done" | ""
std::string resolve_subagent_session_status(
    const SubagentRunRecord* entry);

}  // namespace observability
}  // namespace hunterclaw
