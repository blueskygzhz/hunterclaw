// ============================================================================
// session_lifecycle_state.cc
// 🎯 严格对齐 OpenClaw: src/gateway/session-lifecycle-state.ts (170 行)
//
// 会话运行生命周期状态机 — 纯状态推导模块
// 状态转换: start → running, end/error → done/failed/killed/timeout
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <cmath>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ── Types ────────────────────────────────────────────────────────────────────
// 🎯 对齐: LifecyclePhase = "start" | "end" | "error"

enum class LifecyclePhase { Start, End, Error };

// 🎯 对齐: SessionRunStatus
enum class SessionRunStatus { Running, Done, Failed, Killed, Timeout };

static std::string session_run_status_to_string(SessionRunStatus s) {
    switch (s) {
        case SessionRunStatus::Running: return "running";
        case SessionRunStatus::Done:    return "done";
        case SessionRunStatus::Failed:  return "failed";
        case SessionRunStatus::Killed:  return "killed";
        case SessionRunStatus::Timeout: return "timeout";
    }
    return "unknown";
}

// 🎯 对齐: LifecycleEventLike
struct LifecycleEventLike {
    double ts = 0;
    struct Data {
        std::string phase;
        std::optional<double> started_at;
        std::optional<double> ended_at;
        std::optional<bool> aborted;
        std::string stop_reason;
    };
    std::optional<Data> data;
};

// 🎯 对齐: LifecycleSessionShape / GatewaySessionLifecycleSnapshot
struct LifecycleSnapshot {
    std::optional<double> updated_at;
    std::optional<SessionRunStatus> status;
    std::optional<double> started_at;
    std::optional<double> ended_at;
    std::optional<double> runtime_ms;
    std::optional<bool> aborted_last_run;
};

// ── Helper: isFiniteTimestamp ────────────────────────────────────────────────
// 🎯 对齐: isFiniteTimestamp(value): value is number
static bool is_finite_timestamp(std::optional<double> value) {
    return value.has_value() && std::isfinite(*value) && *value > 0;
}

static bool is_finite_timestamp(double value) {
    return std::isfinite(value) && value > 0;
}

// ── resolveLifecyclePhase ───────────────────────────────────────────────────
// 🎯 对齐: resolveLifecyclePhase(event) → LifecyclePhase | null

static std::optional<LifecyclePhase> resolve_lifecycle_phase(
    const LifecycleEventLike& event) {
    if (!event.data.has_value()) return std::nullopt;
    const auto& phase = event.data->phase;
    if (phase == "start") return LifecyclePhase::Start;
    if (phase == "end")   return LifecyclePhase::End;
    if (phase == "error") return LifecyclePhase::Error;
    return std::nullopt;
}

// ── resolveTerminalStatus ───────────────────────────────────────────────────
// 🎯 对齐: resolveTerminalStatus(event) → SessionRunStatus
// error → "failed", stopReason=aborted → "killed", aborted=true → "timeout", else → "done"

static SessionRunStatus resolve_terminal_status(const LifecycleEventLike& event) {
    auto phase = resolve_lifecycle_phase(event);
    if (phase == LifecyclePhase::Error) {
        return SessionRunStatus::Failed;
    }

    if (event.data.has_value() && event.data->stop_reason == "aborted") {
        return SessionRunStatus::Killed;
    }

    if (event.data.has_value() && event.data->aborted.value_or(false)) {
        return SessionRunStatus::Timeout;
    }

    return SessionRunStatus::Done;
}

// ── resolveLifecycleStartedAt ───────────────────────────────────────────────
// 🎯 对齐: resolveLifecycleStartedAt(existingStartedAt, event)
// Priority: event.data.startedAt > existingStartedAt > event.ts

static std::optional<double> resolve_lifecycle_started_at(
    std::optional<double> existing_started_at,
    const LifecycleEventLike& event) {
    if (event.data.has_value() && is_finite_timestamp(event.data->started_at)) {
        return event.data->started_at;
    }
    if (is_finite_timestamp(existing_started_at)) {
        return existing_started_at;
    }
    if (is_finite_timestamp(event.ts)) {
        return event.ts;
    }
    return std::nullopt;
}

// ── resolveLifecycleEndedAt ─────────────────────────────────────────────────
// 🎯 对齐: resolveLifecycleEndedAt(event)

static std::optional<double> resolve_lifecycle_ended_at(
    const LifecycleEventLike& event) {
    if (event.data.has_value() && is_finite_timestamp(event.data->ended_at)) {
        return event.data->ended_at;
    }
    if (is_finite_timestamp(event.ts)) {
        return event.ts;
    }
    return std::nullopt;
}

// ── resolveRuntimeMs ────────────────────────────────────────────────────────
// 🎯 对齐: resolveRuntimeMs({ startedAt, endedAt, existingRuntimeMs })

static std::optional<double> resolve_runtime_ms(
    std::optional<double> started_at,
    std::optional<double> ended_at,
    std::optional<double> existing_runtime_ms) {
    if (is_finite_timestamp(started_at) && is_finite_timestamp(ended_at)) {
        return std::max(0.0, *ended_at - *started_at);
    }
    if (existing_runtime_ms.has_value() &&
        std::isfinite(*existing_runtime_ms) &&
        *existing_runtime_ms >= 0) {
        return existing_runtime_ms;
    }
    return std::nullopt;
}

// ── deriveGatewaySessionLifecycleSnapshot ───────────────────────────────────
// 🎯 对齐: deriveGatewaySessionLifecycleSnapshot({ session, event })
// 纯函数：从事件推导会话生命周期快照

LifecycleSnapshot derive_gateway_session_lifecycle_snapshot(
    const LifecycleSnapshot* existing,
    const LifecycleEventLike& event) {

    auto phase = resolve_lifecycle_phase(event);
    if (!phase.has_value()) {
        return {};  // no valid phase → empty snapshot
    }

    if (*phase == LifecyclePhase::Start) {
        auto started_at = resolve_lifecycle_started_at(
            existing ? existing->started_at : std::nullopt, event);
        auto updated_at = started_at.has_value() ? started_at
            : (existing ? existing->updated_at : std::nullopt);
        LifecycleSnapshot snap;
        snap.updated_at = updated_at;
        snap.status = SessionRunStatus::Running;
        snap.started_at = started_at;
        snap.ended_at = std::nullopt;
        snap.runtime_ms = std::nullopt;
        snap.aborted_last_run = false;
        return snap;
    }

    // end or error phase
    auto started_at = resolve_lifecycle_started_at(
        existing ? existing->started_at : std::nullopt, event);
    auto ended_at = resolve_lifecycle_ended_at(event);
    auto updated_at = ended_at.has_value() ? ended_at
        : (existing ? existing->updated_at : std::nullopt);

    LifecycleSnapshot snap;
    snap.updated_at = updated_at;
    snap.status = resolve_terminal_status(event);
    snap.started_at = started_at;
    snap.ended_at = ended_at;
    snap.runtime_ms = resolve_runtime_ms(
        started_at, ended_at,
        existing ? existing->runtime_ms : std::nullopt);
    snap.aborted_last_run = (snap.status == SessionRunStatus::Killed);
    return snap;
}

// ── derivePersistedSessionLifecyclePatch ────────────────────────────────────
// 🎯 对齐: derivePersistedSessionLifecyclePatch({ entry, event })
// Wrapper that ensures updatedAt is a valid number

LifecycleSnapshot derive_persisted_session_lifecycle_patch(
    const LifecycleSnapshot* entry,
    const LifecycleEventLike& event) {
    auto snap = derive_gateway_session_lifecycle_snapshot(entry, event);
    if (snap.updated_at.has_value() && !is_finite_timestamp(snap.updated_at)) {
        snap.updated_at = std::nullopt;
    }
    return snap;
}

// ── JSON serialization helpers ──────────────────────────────────────────────

json lifecycle_snapshot_to_json(const LifecycleSnapshot& snap) {
    json j;
    if (snap.updated_at.has_value())
        j["updatedAt"] = *snap.updated_at;
    if (snap.status.has_value())
        j["status"] = session_run_status_to_string(*snap.status);
    if (snap.started_at.has_value())
        j["startedAt"] = *snap.started_at;
    if (snap.ended_at.has_value())
        j["endedAt"] = *snap.ended_at;
    if (snap.runtime_ms.has_value())
        j["runtimeMs"] = *snap.runtime_ms;
    if (snap.aborted_last_run.has_value())
        j["abortedLastRun"] = *snap.aborted_last_run;
    return j;
}

LifecycleEventLike lifecycle_event_from_json(const json& j) {
    LifecycleEventLike evt;
    evt.ts = j.value("ts", 0.0);
    if (j.contains("data") && j["data"].is_object()) {
        LifecycleEventLike::Data d;
        d.phase = j["data"].value("phase", "");
        if (j["data"].contains("startedAt") && j["data"]["startedAt"].is_number())
            d.started_at = j["data"]["startedAt"].get<double>();
        if (j["data"].contains("endedAt") && j["data"]["endedAt"].is_number())
            d.ended_at = j["data"]["endedAt"].get<double>();
        if (j["data"].contains("aborted") && j["data"]["aborted"].is_boolean())
            d.aborted = j["data"]["aborted"].get<bool>();
        d.stop_reason = j["data"].value("stopReason", "");
        evt.data = d;
    }
    return evt;
}

}  // namespace gateway
}  // namespace hunterclaw
