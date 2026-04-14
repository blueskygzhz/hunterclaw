#pragma once

#include <optional>
#include <string>
#include <cstdint>
#include <functional>
#include <unordered_map>

// 严格对齐 OpenClaw:
//   src/auto-reply/reply/abort-cutoff.ts        (107行 — types + resolve + read + has + apply + shouldSkip + shouldPersist)
//   src/auto-reply/reply/abort-cutoff.runtime.ts (33行 — clearAbortCutoffInSessionRuntime)
//   src/auto-reply/reply/commands-session-store.ts (53行 — persistAbortTargetEntry)

namespace hunterclaw {

// Forward declare to avoid circular include
namespace sessions {
struct SessionEntry;
}

namespace react {

// ============================================================================
// AbortCutoff (对齐 abort-cutoff.ts:5-8)
// ============================================================================
struct AbortCutoff {
    std::optional<std::string> message_sid;  // ✅ 对齐 OC: messageSid
    std::optional<int64_t> timestamp_ms;     // ✅ 对齐 OC: timestamp
};

// ============================================================================
// resolveAbortCutoffFromContext (对齐 abort-cutoff.ts:12-21)
// 从当前消息 context 解析 abort cutoff (message_sid + timestamp)
// ============================================================================
AbortCutoff resolve_abort_cutoff_from_context(
    const std::optional<std::string>& message_sid,
    std::optional<int64_t> timestamp_ms = std::nullopt
);

// ============================================================================
// readAbortCutoffFromSessionEntry (对齐 abort-cutoff.ts:23-38)
// 从 SessionEntry 读取已存储的 abort cutoff
// ============================================================================
std::optional<AbortCutoff> read_abort_cutoff_from_session_entry(
    const sessions::SessionEntry* entry
);

// ============================================================================
// hasAbortCutoff (对齐 abort-cutoff.ts:40-42)
// ============================================================================
bool has_abort_cutoff(const sessions::SessionEntry* entry);

// ============================================================================
// applyAbortCutoffToSessionEntry (对齐 abort-cutoff.ts:44-50)
// 将 abort cutoff 写入 SessionEntry（传 nullopt 则清除）
// ============================================================================
void apply_abort_cutoff_to_session_entry(
    sessions::SessionEntry& entry,
    const std::optional<AbortCutoff>& cutoff
);

// ============================================================================
// shouldSkipMessageByAbortCutoff (对齐 abort-cutoff.ts:64-91)
// 判断消息是否应被 abort cutoff 跳过
// 优先比较 message_sid (BigInt 数值比较), fallback 到 timestamp
// ============================================================================
struct AbortCutoffCompareParams {
    std::optional<std::string> cutoff_message_sid;  // 对齐 OC: cutoffMessageSid
    std::optional<int64_t> cutoff_timestamp;        // 对齐 OC: cutoffTimestamp
    std::optional<std::string> message_sid;          // 对齐 OC: messageSid
    std::optional<int64_t> timestamp;                // 对齐 OC: timestamp
};

bool should_skip_message_by_abort_cutoff(const AbortCutoffCompareParams& params);

// ============================================================================
// shouldPersistAbortCutoff (对齐 abort-cutoff.ts:93-106)
// 判断是否需要持久化 abort cutoff
// 仅当 command source 和 target 是同一个 session 时才持久化
// ============================================================================
bool should_persist_abort_cutoff(
    const std::string& command_session_key,
    const std::string& target_session_key
);

// ============================================================================
// clearAbortCutoffInSessionRuntime (对齐 abort-cutoff.runtime.ts:5-33)
// 当一条新消息到达且比 cutoff 更新时，清除 session 上的 abort cutoff
// ============================================================================
struct ClearAbortCutoffParams {
    sessions::SessionEntry* session_entry = nullptr;
    std::unordered_map<std::string, sessions::SessionEntry>* session_store = nullptr;
    std::string session_key;
    std::string store_path;
    // 持久化回调 (对齐 OC: updateSessionStore)
    std::function<void(const std::string& store_path,
                       std::function<void(std::unordered_map<std::string, sessions::SessionEntry>&)>)>
        update_session_store;
};

bool clear_abort_cutoff_in_session_runtime(const ClearAbortCutoffParams& params);

// ============================================================================
// persistAbortTargetEntry (对齐 commands-session-store.ts:22-53)
// 将 abort cutoff 持久化到目标 session entry
// ============================================================================
struct PersistAbortTargetParams {
    sessions::SessionEntry* entry = nullptr;
    std::string key;
    std::unordered_map<std::string, sessions::SessionEntry>* session_store = nullptr;
    std::string store_path;
    std::optional<AbortCutoff> abort_cutoff;
    // 持久化回调
    std::function<void(const std::string& store_path,
                       std::function<void(std::unordered_map<std::string, sessions::SessionEntry>&)>)>
        update_session_store;
};

bool persist_abort_target_entry(const PersistAbortTargetParams& params);

}  // namespace react
}  // namespace hunterclaw
