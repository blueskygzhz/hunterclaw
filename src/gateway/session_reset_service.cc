// ============================================================================
// session_reset_service.cc
// 🎯 严格对齐 OpenClaw: src/gateway/session-reset-service.ts (701 行)
//
// 会话重置的完整生命周期编排：
// - 中止运行中的 agent
// - 保留用户选择 (model override, auth profile)
// - 归档转录
// - 清理队列
// - 广播重置事件
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <chrono>
#include <fstream>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ── Constants ────────────────────────────────────────────────────────────────
// 🎯 对齐: ACP_RUNTIME_CLEANUP_TIMEOUT_MS = 15_000
static constexpr int64_t ACP_RUNTIME_CLEANUP_TIMEOUT_MS = 15'000;

// ── Types ────────────────────────────────────────────────────────────────────

// 🎯 对齐: ResetPreservedSelectionState
struct ResetPreservedSelectionState {
    std::optional<std::string> provider_override;
    std::optional<std::string> model_override;
    std::optional<std::string> model_override_source;
    std::optional<std::string> auth_profile_override;
    std::optional<std::string> auth_profile_override_source;
    std::optional<int> auth_profile_override_compaction_count;
};

// Session entry for reset logic
struct SessionResetEntry {
    std::string session_id;
    std::string session_key;
    std::string agent_id;
    std::string store_path;
    std::string session_file;
    std::string model;
    std::string model_provider;
    std::optional<int> context_tokens;
    std::optional<std::string> provider_override;
    std::optional<std::string> model_override;
    std::optional<std::string> model_override_source;
    std::optional<std::string> auth_profile_override;
    std::optional<std::string> auth_profile_override_source;
    std::optional<int> auth_profile_override_compaction_count;
};

// 🎯 对齐: ArchivedSessionTranscript
struct ArchivedSessionTranscript {
    std::string original_path;
    std::string archived_path;
    bool success = false;
};

struct SessionResetOptions {
    std::string session_key;
    std::string reason;
    bool preserve_selection = true;
    bool archive_transcripts = true;
    bool fire_hooks = true;
};

struct SessionResetResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> archived_paths;
    ResetPreservedSelectionState preserved;
};

// ── stripRuntimeModelState ──────────────────────────────────────────────────
// 🎯 对齐: stripRuntimeModelState(entry)

static SessionResetEntry strip_runtime_model_state(const SessionResetEntry& entry) {
    SessionResetEntry stripped = entry;
    stripped.model.clear();
    stripped.model_provider.clear();
    stripped.context_tokens = std::nullopt;
    return stripped;
}

// ── resolveResetPreservedSelection ──────────────────────────────────────────
// 🎯 对齐: resolveResetPreservedSelection({ entry })

static ResetPreservedSelectionState resolve_reset_preserved_selection(
    const SessionResetEntry* entry) {
    ResetPreservedSelectionState preserved;
    if (!entry) return preserved;

    bool preserve_legacy_user_model_override =
        (entry->model_override_source.has_value() &&
         *entry->model_override_source == "user") ||
        (!entry->model_override_source.has_value() &&
         entry->model_override.has_value() &&
         !entry->model_override->empty());

    if (preserve_legacy_user_model_override && entry->model_override.has_value()) {
        preserved.provider_override = entry->provider_override;
        preserved.model_override = entry->model_override;
        preserved.model_override_source = "user";
    }

    if (entry->auth_profile_override_source.has_value() &&
        *entry->auth_profile_override_source == "user" &&
        entry->auth_profile_override.has_value()) {
        preserved.auth_profile_override = entry->auth_profile_override;
        preserved.auth_profile_override_source = entry->auth_profile_override_source;
        preserved.auth_profile_override_compaction_count =
            entry->auth_profile_override_compaction_count;
    }

    return preserved;
}

// ── archiveSessionTranscriptsForSession ─────────────────────────────────────
// 🎯 对齐: archiveSessionTranscriptsForSession(params) → string[]

std::vector<ArchivedSessionTranscript> archive_session_transcripts_detailed(
    const std::string& session_id,
    const std::string& store_path,
    const std::string& session_file,
    const std::string& agent_id,
    const std::string& reason) {

    std::vector<ArchivedSessionTranscript> results;
    (void)session_id;
    (void)store_path;
    (void)agent_id;

    if (session_file.empty() || !std::filesystem::exists(session_file)) {
        return results;
    }

    try {
        auto now = std::chrono::system_clock::now();
        auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

        std::filesystem::path src(session_file);
        std::string archive_name = src.stem().string() +
            ".archive." + std::to_string(ts) +
            "." + reason + src.extension().string();
        std::filesystem::path archive_path = src.parent_path() / archive_name;

        std::filesystem::copy_file(session_file, archive_path.string(),
            std::filesystem::copy_options::overwrite_existing);

        ArchivedSessionTranscript archived;
        archived.original_path = session_file;
        archived.archived_path = archive_path.string();
        archived.success = true;
        results.push_back(std::move(archived));

    } catch (const std::exception& /*e*/) {
        ArchivedSessionTranscript failed;
        failed.original_path = session_file;
        failed.success = false;
        results.push_back(std::move(failed));
    }

    return results;
}

std::vector<std::string> archive_session_transcripts_for_session(
    const std::string& session_id,
    const std::string& store_path,
    const std::string& session_file,
    const std::string& agent_id,
    const std::string& reason) {

    auto detailed = archive_session_transcripts_detailed(
        session_id, store_path, session_file, agent_id, reason);

    std::vector<std::string> paths;
    for (const auto& entry : detailed) {
        if (entry.success) {
            paths.push_back(entry.archived_path);
        }
    }
    return paths;
}

// ── resetSession (simplified) ───────────────────────────────────────────────
// 🎯 对齐: resetSession() — 简化版（移除对内部模块的直接依赖）
// 完整版本在 gateway.cc 中集成，此处提供核心重置逻辑

SessionResetResult reset_session_core(
    const SessionResetOptions& opts,
    const std::function<void(const std::string&)>& abort_session,
    const std::function<void(const std::string&)>& clear_session_lane,
    const std::function<void(const core::EventFrame&)>& broadcast) {

    SessionResetResult result;

    try {
        // Step 1: 中止当前运行的 agent
        if (abort_session) {
            abort_session(opts.session_key);
        }

        // Step 2: 清理命令队列
        if (clear_session_lane) {
            clear_session_lane(opts.session_key);
        }

        // Step 3: 广播 session.reset 事件
        if (broadcast) {
            json reset_payload;
            reset_payload["sessionKey"] = opts.session_key;
            reset_payload["reason"] = opts.reason;
            reset_payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            core::EventFrame ef;
            ef.event = "session";
            ef.payload = core::JsonValue(reset_payload);
            broadcast(ef);
        }

        result.ok = true;

    } catch (const std::exception& e) {
        result.ok = false;
        result.error = e.what();
    }

    return result;
}

}  // namespace gateway
}  // namespace hunterclaw
