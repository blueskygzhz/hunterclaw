// ============================================================================
// remote_a2a/task_tracking.cc
// 长任务状态机实现。纯函数、零 IO、零线程。
// ============================================================================

#include "hunterclaw/remote_a2a/task_tracking.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace hunterclaw {
namespace remote_a2a {

namespace {

std::string trim_copy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string to_lower_copy(const std::string& s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

// ── ProgressFingerprint ─────────────────────────────────────────────────────

ProgressFingerprint ProgressFingerprint::from_task(const Task& t) {
    ProgressFingerprint f;
    f.status_timestamp = t.status.timestamp;
    f.history_count = t.history.size();
    f.artifact_count = t.artifacts.size();
    for (const auto& a : t.artifacts) f.total_artifact_parts += a.parts.size();
    if (t.status.message) {
        f.last_status_message_len = concat_text_parts(t.status.message->parts).size();
    }
    // event_seq 只在 SSE 路径用，Task 快照路径保持 0
    return f;
}

void ProgressFingerprint::bump_from_event(const ParsedStreamEvent& e) {
    // ★ 任何携带信息的事件都算推进。
    //   注意：不能只在 text 非空时推进 —— 远端可能只更新 timestamp，
    //   那也是活着的证据。
    ++event_seq;

    if (!e.status_timestamp.empty()) status_timestamp = e.status_timestamp;

    switch (e.kind) {
        case ParsedStreamEvent::Kind::ArtifactUpdate:
            // 每个 artifact 增量都累加分片数
            total_artifact_parts += e.artifact_part_count;
            if (e.last_chunk) ++artifact_count;
            break;
        case ParsedStreamEvent::Kind::Task:
            // 完整快照：直接覆盖计数（避免与增量重复累加）
            total_artifact_parts = e.artifact_part_count;
            break;
        case ParsedStreamEvent::Kind::StatusUpdate:
        case ParsedStreamEvent::Kind::Message:
            if (e.has_status_message) ++history_count;
            break;
        case ParsedStreamEvent::Kind::Unknown:
            break;
    }
    if (!e.text.empty()) last_status_message_len = e.text.size();
}

bool ProgressFingerprint::operator==(const ProgressFingerprint& o) const {
    return status_timestamp == o.status_timestamp &&
           history_count == o.history_count &&
           artifact_count == o.artifact_count &&
           total_artifact_parts == o.total_artifact_parts &&
           last_status_message_len == o.last_status_message_len &&
           event_seq == o.event_seq;
}

// ── WorkingSubState ─────────────────────────────────────────────────────────

const char* working_sub_state_text(WorkingSubState s) {
    switch (s) {
        case WorkingSubState::Fresh:   return "fresh";
        case WorkingSubState::Stale:   return "stale";
        case WorkingSubState::Suspect: return "suspect";
        case WorkingSubState::Zombie:  return "zombie";
    }
    return "unknown";
}

// ── TerminalReason ──────────────────────────────────────────────────────────

const char* terminal_reason_text(TerminalReason r) {
    switch (r) {
        case TerminalReason::HardTimeout:  return "hard_timeout";
        case TerminalReason::Zombie:       return "zombie";
        case TerminalReason::RemoteFailed: return "remote_failed";
        case TerminalReason::Canceled:     return "canceled";
        case TerminalReason::ConnLost:     return "conn_lost";
        case TerminalReason::Completed:    return "completed";
    }
    return "unknown";
}

TaskState terminal_reason_to_task_state(TerminalReason r) {
    switch (r) {
        case TerminalReason::HardTimeout:
        case TerminalReason::Zombie:
        case TerminalReason::RemoteFailed:
        case TerminalReason::ConnLost:
            return TaskState::Failed;
        case TerminalReason::Canceled:
            return TaskState::Canceled;
        case TerminalReason::Completed:
            return TaskState::Completed;
    }
    return TaskState::Failed;
}

bool is_sticky(TerminalReason r) {
    // 対标 run_wait.h:105-108 isStickyAgentRunTerminalOutcome
    return r == TerminalReason::HardTimeout || r == TerminalReason::Canceled;
}

// ── classify ────────────────────────────────────────────────────────────────

TerminalReason classify(const TaskTracking& t) {
    // ★★ 顺序即优先级，逐条短路。対标 run_wait_pure.cc:150-164。
    //    不要重排、不要改成 if-else 平铺。

    // ① 总时长超限 —— 最高优先级。即使远端已报 failed，归因也应是硬超时，
    //    否则运维看到的是"远端失败"而真实原因是"我们等太久了"。
    if (t.max_task_ms > 0 && t.elapsed_ms > t.max_task_ms) {
        return TerminalReason::HardTimeout;
    }

    // ② 连续探测无推进 —— 我们主动判死，优先于远端的自我报告
    if (t.max_no_progress > 0 && t.consecutive_no_progress >= t.max_no_progress) {
        return TerminalReason::Zombie;
    }

    // ③ 远端明确失败
    if (t.remote_state == TaskState::Failed) {
        return TerminalReason::RemoteFailed;
    }

    // ④ 取消
    if (t.remote_state == TaskState::Canceled) {
        return TerminalReason::Canceled;
    }

    // ⑤ 传输不可恢复。可恢复的抖动不算终态 —— 让调用方重试。
    if (t.transport_dead && !is_recoverable_transport_error(t.last_err)) {
        return TerminalReason::ConnLost;
    }

    return TerminalReason::Completed;
}

WorkingSubState classify_working(const TaskTracking& t, bool fp_changed) {
    if (t.max_no_progress > 0 && t.consecutive_no_progress >= t.max_no_progress) {
        return WorkingSubState::Zombie;
    }
    if (t.idle_timeout_ms <= 0 || t.idle_ms <= t.idle_timeout_ms) {
        return WorkingSubState::Fresh;
    }
    return fp_changed ? WorkingSubState::Stale : WorkingSubState::Suspect;
}

bool should_keep_waiting(const TaskTracking& t) {
    // 远端已到终态 → 不必再等
    if (is_terminal(t.remote_state)) return false;
    // 触发了任何判死条件 → 不必再等
    const TerminalReason r = classify(t);
    if (r != TerminalReason::Completed) return false;
    return true;
}

// ── 传输错误可恢复性 ────────────────────────────────────────────────────────

bool is_recoverable_transport_error(const std::string& error) {
    const std::string msg = to_lower_copy(trim_copy(error));
    if (msg.empty()) return false;

    // pattern 表参考 run_wait_pure.cc:217-226 的 errno 族，
    // 去掉 gateway 专属项，加 HTTP 特有项。
    static const char* const kPatterns[] = {
        // socket / DNS errno（与 run_wait 共用）
        "econnreset", "econnrefused", "epipe", "etimedout", "enotfound",
        "eai_again", "ehostunreach", "enetunreach", "econnaborted",
        // 传输层描述
        "connection closed", "connection loss", "connection reset",
        "socket hang up", "transport close", "broken pipe",
        "ssl", "handshake",
        // HTTP 5xx —— 上游抖动
        // ⚠ 与 run_wait 不同：那里把 "gateway timeout" 排除在可恢复之外
        //   （因为那指 WS gateway 的确定性超时），HTTP 504 是上游抖动，可恢复。
        "502", "503", "504",
        "bad gateway", "service unavailable", "gateway time-out",
        "gateway timeout", "upstream connect error", "upstream timeout",
        "temporarily unavailable", "too many requests", "429",
    };
    for (const char* p : kPatterns) {
        if (msg.find(p) != std::string::npos) return true;
    }
    return false;
}

}  // namespace remote_a2a
}  // namespace hunterclaw
