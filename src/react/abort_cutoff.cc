#include "hunterclaw/react/abort_cutoff.h"
#include "hunterclaw/sessions/session_manager.h"
#include <chrono>
#include <spdlog/spdlog.h>

// 严格对齐 OpenClaw:
//   src/auto-reply/reply/abort-cutoff.ts        (107行)
//   src/auto-reply/reply/abort-cutoff.runtime.ts (33行)
//   src/auto-reply/reply/commands-session-store.ts:22-53

namespace hunterclaw {
namespace react {

namespace {
auto log() {
    static auto logger = spdlog::get("abort_cutoff");
    if (!logger) logger = spdlog::stdout_logger_mt("abort_cutoff");
    return logger;
}

// ============================================================================
// toNumericMessageSid (对齐 abort-cutoff.ts:52-62)
// 将字符串 message SID 解析为 uint64_t 用于数值比较
// OC 用 BigInt，C++ 用 uint64_t (足够覆盖绝大多数平台的消息 ID)
// ============================================================================
std::optional<uint64_t> to_numeric_message_sid(const std::optional<std::string>& value) {
    if (!value || value->empty()) {
        return std::nullopt;
    }
    // 对齐 OC: trim + check all digits
    const std::string& trimmed = *value;
    if (trimmed.empty()) {
        return std::nullopt;
    }
    // Check all chars are digits
    for (char c : trimmed) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
    }
    try {
        return std::stoull(trimmed);
    } catch (...) {
        return std::nullopt;
    }
}

// normalizeOptionalString — helper: trim + empty→nullopt
std::optional<std::string> normalize_optional_string(const std::optional<std::string>& value) {
    if (!value || value->empty()) {
        return std::nullopt;
    }
    // Simple trim
    auto start = value->find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return std::nullopt;
    }
    auto end = value->find_last_not_of(" \t\n\r");
    std::string result = value->substr(start, end - start + 1);
    return result.empty() ? std::nullopt : std::optional<std::string>(result);
}
}  // anonymous namespace

// ============================================================================
// resolveAbortCutoffFromContext (对齐 abort-cutoff.ts:12-21)
// ============================================================================
AbortCutoff resolve_abort_cutoff_from_context(
    const std::optional<std::string>& message_sid,
    std::optional<int64_t> timestamp_ms
) {
    // 对齐 OC: normalizeOptionalString(ctx.MessageSidFull) ?? normalizeOptionalString(ctx.MessageSid)
    auto normalized_sid = normalize_optional_string(message_sid);

    // 对齐 OC: typeof ctx.Timestamp === "number" && Number.isFinite(ctx.Timestamp) ? ctx.Timestamp : undefined
    std::optional<int64_t> ts;
    if (timestamp_ms.has_value() && *timestamp_ms > 0) {
        ts = timestamp_ms;
    }

    // 对齐 OC: if (!messageSid && timestamp === undefined) return undefined;
    // HC: 返回空 cutoff（调用方通过 has_value() 判断是否有效）
    AbortCutoff cutoff;
    cutoff.message_sid = normalized_sid;
    cutoff.timestamp_ms = ts;
    return cutoff;
}

// ============================================================================
// readAbortCutoffFromSessionEntry (对齐 abort-cutoff.ts:23-38)
// ============================================================================
std::optional<AbortCutoff> read_abort_cutoff_from_session_entry(
    const sessions::SessionEntry* entry
) {
    if (!entry) {
        return std::nullopt;
    }

    auto message_sid = normalize_optional_string(entry->abort_cutoff_message_sid);

    std::optional<int64_t> timestamp;
    if (entry->abort_cutoff_ms.has_value() && *entry->abort_cutoff_ms > 0) {
        timestamp = entry->abort_cutoff_ms;
    }

    // 对齐 OC: 如果 messageSid 和 timestamp 都为空 → 返回 undefined
    if (!message_sid && !timestamp) {
        return std::nullopt;
    }

    AbortCutoff cutoff;
    cutoff.message_sid = message_sid;
    cutoff.timestamp_ms = timestamp;
    return cutoff;
}

// ============================================================================
// hasAbortCutoff (对齐 abort-cutoff.ts:40-42)
// ============================================================================
bool has_abort_cutoff(const sessions::SessionEntry* entry) {
    return read_abort_cutoff_from_session_entry(entry).has_value();
}

// ============================================================================
// applyAbortCutoffToSessionEntry (对齐 abort-cutoff.ts:44-50)
// 写入 cutoff 到 entry；传 nullopt 则清除
// ============================================================================
void apply_abort_cutoff_to_session_entry(
    sessions::SessionEntry& entry,
    const std::optional<AbortCutoff>& cutoff
) {
    if (cutoff.has_value()) {
        entry.abort_cutoff_message_sid = cutoff->message_sid;
        entry.abort_cutoff_ms = cutoff->timestamp_ms;
    } else {
        // 对齐 OC: cutoff=undefined → 清除字段
        entry.abort_cutoff_message_sid = std::nullopt;
        entry.abort_cutoff_ms = std::nullopt;
    }
}

// ============================================================================
// shouldSkipMessageByAbortCutoff (对齐 abort-cutoff.ts:64-91)
// 优先用 message_sid 数值比较，fallback 到 timestamp
// ============================================================================
bool should_skip_message_by_abort_cutoff(const AbortCutoffCompareParams& params) {
    auto cutoff_sid = normalize_optional_string(params.cutoff_message_sid);
    auto current_sid = normalize_optional_string(params.message_sid);

    // 对齐 OC: 先尝试 message SID 比较
    if (cutoff_sid && current_sid) {
        auto cutoff_numeric = to_numeric_message_sid(cutoff_sid);
        auto current_numeric = to_numeric_message_sid(current_sid);

        // 对齐 OC: 如果两者都能解析为数值，用数值比较
        if (cutoff_numeric.has_value() && current_numeric.has_value()) {
            return *current_numeric <= *cutoff_numeric;
        }

        // 对齐 OC: 如果 SID 相同 → 跳过
        if (*current_sid == *cutoff_sid) {
            return true;
        }
    }

    // 对齐 OC: fallback 到 timestamp 比较
    if (params.cutoff_timestamp.has_value() && *params.cutoff_timestamp > 0 &&
        params.timestamp.has_value() && *params.timestamp > 0) {
        return *params.timestamp <= *params.cutoff_timestamp;
    }

    return false;
}

// ============================================================================
// shouldPersistAbortCutoff (对齐 abort-cutoff.ts:93-106)
// ============================================================================
bool should_persist_abort_cutoff(
    const std::string& command_session_key,
    const std::string& target_session_key
) {
    if (command_session_key.empty() || target_session_key.empty()) {
        // 对齐 OC: 如果任一为空 → 返回 true (保守持久化)
        return true;
    }
    // 对齐 OC: Native targeted /stop can run from a slash/session-control key while
    // the actual target session uses different message id/timestamp spaces.
    // Persist cutoff only when command source and target are the same session.
    return command_session_key == target_session_key;
}

// ============================================================================
// clearAbortCutoffInSessionRuntime (对齐 abort-cutoff.runtime.ts:5-33)
// 当新消息比 cutoff 更新时，清除 session 上的 abort cutoff
// ============================================================================
bool clear_abort_cutoff_in_session_runtime(const ClearAbortCutoffParams& params) {
    if (!params.session_entry || !params.session_store ||
        params.session_key.empty() || !has_abort_cutoff(params.session_entry)) {
        return false;
    }

    // 对齐 OC: 清除内存中的 entry
    apply_abort_cutoff_to_session_entry(*params.session_entry, std::nullopt);
    params.session_entry->updated_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    (*params.session_store)[params.session_key] = *params.session_entry;

    // 对齐 OC: 持久化到 store
    if (!params.store_path.empty() && params.update_session_store) {
        auto session_key = params.session_key;
        auto session_entry_copy = *params.session_entry;
        params.update_session_store(params.store_path,
            [session_key, session_entry_copy](
                std::unordered_map<std::string, sessions::SessionEntry>& store) mutable {
            auto it = store.find(session_key);
            sessions::SessionEntry& existing =
                (it != store.end()) ? it->second : session_entry_copy;
            apply_abort_cutoff_to_session_entry(existing, std::nullopt);
            existing.updated_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            store[session_key] = existing;
        });
    }

    log()->debug("Cleared abort cutoff for session '{}'", params.session_key);
    return true;
}

// ============================================================================
// persistAbortTargetEntry (对齐 commands-session-store.ts:22-53)
// 将 abort cutoff 持久化到目标 session entry
// ============================================================================
bool persist_abort_target_entry(const PersistAbortTargetParams& params) {
    if (!params.entry || params.key.empty() || !params.session_store) {
        return false;
    }

    // 对齐 OC: entry.abortedLastRun = true
    params.entry->aborted_last_run = true;

    // 对齐 OC: applyAbortCutoffToSessionEntry(entry, abortCutoff)
    apply_abort_cutoff_to_session_entry(*params.entry, params.abort_cutoff);

    // 对齐 OC: entry.updatedAt = Date.now()
    params.entry->updated_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // 对齐 OC: sessionStore[key] = entry
    (*params.session_store)[params.key] = *params.entry;

    // 对齐 OC: 持久化到 store file
    if (!params.store_path.empty() && params.update_session_store) {
        auto key = params.key;
        auto entry_copy = *params.entry;
        auto abort_cutoff = params.abort_cutoff;
        params.update_session_store(params.store_path,
            [key, entry_copy, abort_cutoff](
                std::unordered_map<std::string, sessions::SessionEntry>& store) mutable {
            auto it = store.find(key);
            sessions::SessionEntry& next_entry =
                (it != store.end()) ? it->second : entry_copy;
            // 对齐 OC: nextEntry.abortedLastRun = true
            next_entry.aborted_last_run = true;
            // 对齐 OC: applyAbortCutoffToSessionEntry(nextEntry, abortCutoff)
            apply_abort_cutoff_to_session_entry(next_entry, abort_cutoff);
            next_entry.updated_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            store[key] = next_entry;
        });
    }

    log()->debug("Persisted abort target entry for session '{}'", params.key);
    return true;
}

}  // namespace react
}  // namespace hunterclaw
