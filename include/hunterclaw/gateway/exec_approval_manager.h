// HunterClaw - High-Performance AI Agent Platform (C++)
// Copyright (c) 2025 HunterClaw Team
//
// ExecApprovalManager - 严格对齐 OpenClaw gateway/exec-approval-manager.ts
// 管理 exec.approval.request / waitDecision / resolve 的生命周期

#pragma once

#include <string>
#include <memory>
#include <optional>
#include <functional>
#include <map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace gateway {

using json = nlohmann::json;

// ============================================================================
// ExecApprovalDecision (对齐 OpenClaw ExecApprovalDecision)
// ============================================================================
// "allow-once" | "allow-always" | "deny"
using ExecApprovalDecision = std::string;

// ============================================================================
// ExecApprovalRequestPayload (对齐 OpenClaw ExecApprovalRequestPayload)
// ============================================================================
struct ExecApprovalRequestPayload {
    std::string command;
    std::optional<std::string> command_preview;
    std::optional<std::vector<std::string>> command_argv;
    std::optional<std::vector<std::string>> env_keys;
    json system_run_binding = nullptr;   // SystemRunApprovalBinding | null
    json system_run_plan = nullptr;      // SystemRunApprovalPlan | null
    std::optional<std::string> cwd;
    std::optional<std::string> node_id;
    std::optional<std::string> host;
    std::optional<std::string> security;
    std::optional<std::string> ask;
    std::optional<std::string> agent_id;
    std::optional<std::string> resolved_path;
    std::optional<std::string> session_key;
    std::optional<std::string> turn_source_channel;
    std::optional<std::string> turn_source_to;
    std::optional<std::string> turn_source_account_id;
    json turn_source_thread_id = nullptr;  // string | number | null

    json to_json() const;
    static ExecApprovalRequestPayload from_json(const json& j);
};

// ============================================================================
// ExecApprovalRecord (对齐 OpenClaw ExecApprovalRecord)
// ============================================================================
struct ExecApprovalRecord {
    std::string id;
    ExecApprovalRequestPayload request;
    int64_t created_at_ms = 0;
    int64_t expires_at_ms = 0;
    // Caller metadata (best-effort)
    std::optional<std::string> requested_by_conn_id;
    std::optional<std::string> requested_by_device_id;
    std::optional<std::string> requested_by_client_id;
    // Resolution state
    std::optional<int64_t> resolved_at_ms;
    std::optional<ExecApprovalDecision> decision;
    std::optional<std::string> resolved_by;
};

// ============================================================================
// ExecApprovalIdLookupResult (对齐 OpenClaw ExecApprovalIdLookupResult)
// ============================================================================
struct ExecApprovalIdLookupResult {
    enum class Kind { Exact, Prefix, Ambiguous, None };
    Kind kind = Kind::None;
    std::string id;                    // for Exact/Prefix
    std::vector<std::string> ids;      // for Ambiguous
};

// ============================================================================
// ExecApprovalManager (对齐 OpenClaw ExecApprovalManager class)
// ============================================================================
// Grace period to keep resolved entries for late awaitDecision calls
static constexpr int64_t RESOLVED_ENTRY_GRACE_MS = 15000;

class ExecApprovalManager {
public:
    ExecApprovalManager() = default;
    ~ExecApprovalManager() = default;

    // Non-copyable
    ExecApprovalManager(const ExecApprovalManager&) = delete;
    ExecApprovalManager& operator=(const ExecApprovalManager&) = delete;

    // ── create() ──────────────────────────────────────────────────────────
    // 对齐 OpenClaw: create(request, timeoutMs, id?)
    ExecApprovalRecord create(
        const ExecApprovalRequestPayload& request,
        int64_t timeout_ms,
        const std::optional<std::string>& explicit_id = std::nullopt
    );

    // ── register_record() ─────────────────────────────────────────────────
    // 对齐 OpenClaw: register(record, timeoutMs)
    // 同步注册，返回 future-like 等待句柄 (通过 await_decision 等待)
    // 如果 ID 已存在且未 resolved，幂等返回；如果已 resolved，抛出异常
    bool register_record(const ExecApprovalRecord& record, int64_t timeout_ms);

    // ── resolve() ─────────────────────────────────────────────────────────
    // 对齐 OpenClaw: resolve(recordId, decision, resolvedBy?)
    bool resolve(
        const std::string& record_id,
        const ExecApprovalDecision& decision,
        const std::optional<std::string>& resolved_by = std::nullopt
    );

    // ── expire() ──────────────────────────────────────────────────────────
    // 对齐 OpenClaw: expire(recordId, resolvedBy?)
    bool expire(
        const std::string& record_id,
        const std::optional<std::string>& resolved_by = std::nullopt
    );

    // ── await_decision() ──────────────────────────────────────────────────
    // 对齐 OpenClaw: awaitDecision(recordId)
    // 阻塞等待决策，返回 decision 或 nullopt（超时/过期）
    // 如果 ID 不存在，返回 nullopt
    std::optional<ExecApprovalDecision> await_decision(
        const std::string& record_id,
        int64_t timeout_ms = 0  // 0 = 使用注册时的超时
    );

    // ── get_snapshot() ────────────────────────────────────────────────────
    // 对齐 OpenClaw: getSnapshot(recordId)
    std::optional<ExecApprovalRecord> get_snapshot(const std::string& record_id) const;

    // ── consume_allow_once() ──────────────────────────────────────────────
    // 对齐 OpenClaw: consumeAllowOnce(recordId)
    bool consume_allow_once(const std::string& record_id);

    // ── lookup_pending_id() ───────────────────────────────────────────────
    // 对齐 OpenClaw: lookupPendingId(input)
    // 支持精确匹配和前缀匹配
    ExecApprovalIdLookupResult lookup_pending_id(const std::string& input) const;

    // ── has_pending() ─────────────────────────────────────────────────────
    // 检查是否有待处理的 approval（用于 hasExecApprovalClients 判断）
    bool has_pending() const;

private:
    struct PendingEntry {
        ExecApprovalRecord record;
        std::condition_variable cv;
        bool decided = false;
        int64_t timeout_ms = 0;
    };

    mutable std::mutex mutex_;
    // record_id -> PendingEntry
    std::map<std::string, std::shared_ptr<PendingEntry>> pending_;

    static int64_t now_ms();
    static std::string generate_uuid();
};

} // namespace gateway
} // namespace hunterclaw
