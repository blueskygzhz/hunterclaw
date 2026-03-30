// HunterClaw - High-Performance AI Agent Platform (C++)
// Copyright (c) 2025 HunterClaw Team
//
// ExecApprovalManager Implementation
// 严格对齐 OpenClaw gateway/exec-approval-manager.ts

#include "hunterclaw/gateway/exec_approval_manager.h"
#include <chrono>
#include <algorithm>
#include <random>
#include <sstream>
#include <iomanip>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// ExecApprovalRequestPayload serialization
// ============================================================================

json ExecApprovalRequestPayload::to_json() const {
    json j;
    j["command"] = command;
    if (command_preview.has_value()) j["commandPreview"] = command_preview.value();
    if (command_argv.has_value()) {
        j["commandArgv"] = command_argv.value();
    }
    if (env_keys.has_value() && !env_keys.value().empty()) {
        j["envKeys"] = env_keys.value();
    }
    j["systemRunBinding"] = system_run_binding;
    j["systemRunPlan"] = system_run_plan;
    if (cwd.has_value()) j["cwd"] = cwd.value(); else j["cwd"] = nullptr;
    if (node_id.has_value()) j["nodeId"] = node_id.value(); else j["nodeId"] = nullptr;
    if (host.has_value()) j["host"] = host.value(); else j["host"] = nullptr;
    if (security.has_value()) j["security"] = security.value(); else j["security"] = nullptr;
    if (ask.has_value()) j["ask"] = ask.value(); else j["ask"] = nullptr;
    if (agent_id.has_value()) j["agentId"] = agent_id.value(); else j["agentId"] = nullptr;
    if (resolved_path.has_value()) j["resolvedPath"] = resolved_path.value(); else j["resolvedPath"] = nullptr;
    if (session_key.has_value()) j["sessionKey"] = session_key.value(); else j["sessionKey"] = nullptr;
    if (turn_source_channel.has_value()) j["turnSourceChannel"] = turn_source_channel.value(); else j["turnSourceChannel"] = nullptr;
    if (turn_source_to.has_value()) j["turnSourceTo"] = turn_source_to.value(); else j["turnSourceTo"] = nullptr;
    if (turn_source_account_id.has_value()) j["turnSourceAccountId"] = turn_source_account_id.value(); else j["turnSourceAccountId"] = nullptr;
    j["turnSourceThreadId"] = turn_source_thread_id;
    return j;
}

ExecApprovalRequestPayload ExecApprovalRequestPayload::from_json(const json& j) {
    ExecApprovalRequestPayload p;
    if (j.contains("command") && j["command"].is_string()) {
        p.command = j["command"].get<std::string>();
    }
    if (j.contains("commandPreview") && j["commandPreview"].is_string()) {
        p.command_preview = j["commandPreview"].get<std::string>();
    }
    if (j.contains("commandArgv") && j["commandArgv"].is_array()) {
        std::vector<std::string> argv;
        for (const auto& a : j["commandArgv"]) {
            if (a.is_string()) argv.push_back(a.get<std::string>());
        }
        p.command_argv = argv;
    }
    if (j.contains("envKeys") && j["envKeys"].is_array()) {
        std::vector<std::string> keys;
        for (const auto& k : j["envKeys"]) {
            if (k.is_string()) keys.push_back(k.get<std::string>());
        }
        p.env_keys = keys;
    }
    if (j.contains("systemRunBinding")) p.system_run_binding = j["systemRunBinding"];
    if (j.contains("systemRunPlan")) p.system_run_plan = j["systemRunPlan"];
    if (j.contains("cwd") && j["cwd"].is_string()) p.cwd = j["cwd"].get<std::string>();
    if (j.contains("nodeId") && j["nodeId"].is_string()) p.node_id = j["nodeId"].get<std::string>();
    if (j.contains("host") && j["host"].is_string()) p.host = j["host"].get<std::string>();
    if (j.contains("security") && j["security"].is_string()) p.security = j["security"].get<std::string>();
    if (j.contains("ask") && j["ask"].is_string()) p.ask = j["ask"].get<std::string>();
    if (j.contains("agentId") && j["agentId"].is_string()) p.agent_id = j["agentId"].get<std::string>();
    if (j.contains("resolvedPath") && j["resolvedPath"].is_string()) p.resolved_path = j["resolvedPath"].get<std::string>();
    if (j.contains("sessionKey") && j["sessionKey"].is_string()) p.session_key = j["sessionKey"].get<std::string>();
    if (j.contains("turnSourceChannel") && j["turnSourceChannel"].is_string()) p.turn_source_channel = j["turnSourceChannel"].get<std::string>();
    if (j.contains("turnSourceTo") && j["turnSourceTo"].is_string()) p.turn_source_to = j["turnSourceTo"].get<std::string>();
    if (j.contains("turnSourceAccountId") && j["turnSourceAccountId"].is_string()) p.turn_source_account_id = j["turnSourceAccountId"].get<std::string>();
    if (j.contains("turnSourceThreadId")) p.turn_source_thread_id = j["turnSourceThreadId"];
    return p;
}

// ============================================================================
// ExecApprovalManager helpers
// ============================================================================

int64_t ExecApprovalManager::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string ExecApprovalManager::generate_uuid() {
    static thread_local std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t hi = dist(rng);
    uint64_t lo = dist(rng);
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        (uint32_t)(hi >> 32),
        (uint16_t)(hi >> 16),
        (uint16_t)(hi),
        (uint16_t)(lo >> 48),
        (unsigned long long)(lo & 0x0000FFFFFFFFFFFFULL)
    );
    return std::string(buf);
}

// ============================================================================
// ExecApprovalManager::create
// 对齐 OpenClaw: create(request, timeoutMs, id?)
// ============================================================================
ExecApprovalRecord ExecApprovalManager::create(
    const ExecApprovalRequestPayload& request,
    int64_t timeout_ms,
    const std::optional<std::string>& explicit_id
) {
    int64_t now = now_ms();
    std::string resolved_id;
    if (explicit_id.has_value() && !explicit_id.value().empty()) {
        resolved_id = explicit_id.value();
        // trim
        while (!resolved_id.empty() && resolved_id.front() == ' ') resolved_id.erase(0, 1);
        while (!resolved_id.empty() && resolved_id.back() == ' ') resolved_id.pop_back();
    }
    if (resolved_id.empty()) {
        resolved_id = generate_uuid();
    }
    ExecApprovalRecord record;
    record.id = resolved_id;
    record.request = request;
    record.created_at_ms = now;
    record.expires_at_ms = now + timeout_ms;
    return record;
}

// ============================================================================
// ExecApprovalManager::register_record
// 对齐 OpenClaw: register(record, timeoutMs)
// ============================================================================
bool ExecApprovalManager::register_record(const ExecApprovalRecord& record, int64_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = pending_.find(record.id);
    if (it != pending_.end()) {
        // Idempotent: if still pending, return true
        if (!it->second->decided) {
            return true;
        }
        // Already resolved - don't allow re-registration
        return false;
    }

    auto entry = std::make_shared<PendingEntry>();
    entry->record = record;
    entry->decided = false;
    entry->timeout_ms = timeout_ms;
    pending_[record.id] = entry;
    return true;
}

// ============================================================================
// ExecApprovalManager::resolve
// 对齐 OpenClaw: resolve(recordId, decision, resolvedBy?)
// ============================================================================
bool ExecApprovalManager::resolve(
    const std::string& record_id,
    const ExecApprovalDecision& decision,
    const std::optional<std::string>& resolved_by
) {
    std::shared_ptr<PendingEntry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(record_id);
        if (it == pending_.end()) return false;
        if (it->second->decided) return false;  // Prevent double-resolve
        entry = it->second;
        entry->record.resolved_at_ms = now_ms();
        entry->record.decision = decision;
        entry->record.resolved_by = resolved_by;
        entry->decided = true;
    }
    // Notify waiters
    entry->cv.notify_all();

    // Schedule grace period cleanup
    // (In C++ we do this lazily in await_decision / get_snapshot)
    return true;
}

// ============================================================================
// ExecApprovalManager::expire
// 对齐 OpenClaw: expire(recordId, resolvedBy?)
// ============================================================================
bool ExecApprovalManager::expire(
    const std::string& record_id,
    const std::optional<std::string>& resolved_by
) {
    std::shared_ptr<PendingEntry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(record_id);
        if (it == pending_.end()) return false;
        if (it->second->decided) return false;
        entry = it->second;
        entry->record.resolved_at_ms = now_ms();
        entry->record.decision = std::nullopt;  // null decision on timeout
        entry->record.resolved_by = resolved_by;
        entry->decided = true;
    }
    entry->cv.notify_all();
    return true;
}

// ============================================================================
// ExecApprovalManager::await_decision
// 对齐 OpenClaw: awaitDecision(recordId) → Promise<ExecApprovalDecision | null>
// ============================================================================
std::optional<ExecApprovalDecision> ExecApprovalManager::await_decision(
    const std::string& record_id,
    int64_t timeout_ms
) {
    std::shared_ptr<PendingEntry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(record_id);
        if (it == pending_.end()) return std::nullopt;
        entry = it->second;
    }

    // Determine effective timeout
    int64_t effective_timeout = timeout_ms > 0 ? timeout_ms : entry->timeout_ms;
    if (effective_timeout <= 0) effective_timeout = 120000;  // 2 min default

    // Wait for decision
    {
        std::unique_lock<std::mutex> lock(mutex_);
        bool got_decision = entry->cv.wait_for(
            lock,
            std::chrono::milliseconds(effective_timeout),
            [&entry]() { return entry->decided; }
        );
        (void)got_decision;
    }

    // Cleanup after grace period (lazy)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(record_id);
        if (it != pending_.end() && it->second == entry && entry->decided) {
            int64_t resolved_at = entry->record.resolved_at_ms.value_or(0);
            if (now_ms() - resolved_at > RESOLVED_ENTRY_GRACE_MS) {
                pending_.erase(it);
            }
        }
    }

    if (!entry->decided) return std::nullopt;
    return entry->record.decision;  // may be nullopt (timeout/expire)
}

// ============================================================================
// ExecApprovalManager::get_snapshot
// 对齐 OpenClaw: getSnapshot(recordId)
// ============================================================================
std::optional<ExecApprovalRecord> ExecApprovalManager::get_snapshot(
    const std::string& record_id
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(record_id);
    if (it == pending_.end()) return std::nullopt;
    return it->second->record;
}

// ============================================================================
// ExecApprovalManager::consume_allow_once
// 对齐 OpenClaw: consumeAllowOnce(recordId)
// ============================================================================
bool ExecApprovalManager::consume_allow_once(const std::string& record_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(record_id);
    if (it == pending_.end()) return false;
    auto& record = it->second->record;
    if (!record.decision.has_value() || record.decision.value() != "allow-once") {
        return false;
    }
    // Atomically consume
    record.decision = std::nullopt;
    return true;
}

// ============================================================================
// ExecApprovalManager::lookup_pending_id
// 对齐 OpenClaw: lookupPendingId(input)
// ============================================================================
ExecApprovalIdLookupResult ExecApprovalManager::lookup_pending_id(
    const std::string& input
) const {
    std::string normalized = input;
    // trim
    while (!normalized.empty() && normalized.front() == ' ') normalized.erase(0, 1);
    while (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();

    if (normalized.empty()) {
        return {ExecApprovalIdLookupResult::Kind::None, "", {}};
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Exact match
    auto exact_it = pending_.find(normalized);
    if (exact_it != pending_.end()) {
        if (!exact_it->second->decided) {
            return {ExecApprovalIdLookupResult::Kind::Exact, normalized, {}};
        }
        return {ExecApprovalIdLookupResult::Kind::None, "", {}};
    }

    // Prefix match (case-insensitive)
    std::string lower_prefix = normalized;
    std::transform(lower_prefix.begin(), lower_prefix.end(), lower_prefix.begin(), ::tolower);

    std::vector<std::string> matches;
    for (const auto& [id, entry] : pending_) {
        if (entry->decided) continue;
        std::string lower_id = id;
        std::transform(lower_id.begin(), lower_id.end(), lower_id.begin(), ::tolower);
        if (lower_id.substr(0, lower_prefix.size()) == lower_prefix) {
            matches.push_back(id);
        }
    }

    if (matches.size() == 1) {
        return {ExecApprovalIdLookupResult::Kind::Prefix, matches[0], {}};
    }
    if (matches.size() > 1) {
        return {ExecApprovalIdLookupResult::Kind::Ambiguous, "", matches};
    }
    return {ExecApprovalIdLookupResult::Kind::None, "", {}};
}

// ============================================================================
// ExecApprovalManager::has_pending
// ============================================================================
bool ExecApprovalManager::has_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, entry] : pending_) {
        if (!entry->decided) return true;
    }
    return false;
}

} // namespace gateway
} // namespace hunterclaw
