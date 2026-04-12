// ============================================================================
// session_compaction_checkpoints.cc
// 🎯 严格对齐 OpenClaw: src/gateway/session-compaction-checkpoints.ts (208 行)
//
// 会话压缩（compaction）检查点系统 — 上下文窗口管理的关键机制
// 在会话压缩（移除旧上下文以适配 token 窗口）之前保存恢复点
// 类似于数据库的 WAL 检查点机制
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

namespace {
static std::shared_ptr<spdlog::logger> cc_log() {
    static auto logger = spdlog::get("gateway");
    if (!logger) logger = spdlog::stdout_logger_mt("gateway_cc");
    return logger;
}
} // anonymous namespace

// ── Constants ────────────────────────────────────────────────────────────────
// 🎯 对齐: MAX_COMPACTION_CHECKPOINTS_PER_SESSION = 25
static constexpr int MAX_COMPACTION_CHECKPOINTS_PER_SESSION = 25;

// ── Types ────────────────────────────────────────────────────────────────────

// 🎯 对齐: SessionCompactionCheckpointReason
// "auto-threshold" | "manual" | "timeout-retry" | "overflow-retry"
using CompactionCheckpointReason = std::string;

// 🎯 对齐: SessionCompactionCheckpoint
struct SessionCompactionCheckpoint {
    std::string id;
    std::string session_file;
    std::string leaf_id;
    CompactionCheckpointReason reason;
    int64_t created_at = 0;
};

// 🎯 对齐: CapturedCompactionCheckpointSnapshot
struct CapturedCompactionCheckpointSnapshot {
    std::string session_id;
    std::string session_file;
    std::string leaf_id;
};

// ── trimSessionCheckpoints ──────────────────────────────────────────────────
// 🎯 对齐: trimSessionCheckpoints(checkpoints)
// 保留最近 MAX 个检查点（滑动窗口）

static std::vector<SessionCompactionCheckpoint> trim_session_checkpoints(
    const std::vector<SessionCompactionCheckpoint>& checkpoints) {
    if (checkpoints.empty()) return {};

    if (static_cast<int>(checkpoints.size()) <= MAX_COMPACTION_CHECKPOINTS_PER_SESSION) {
        return checkpoints;
    }

    // Keep the last N checkpoints
    int start = static_cast<int>(checkpoints.size()) - MAX_COMPACTION_CHECKPOINTS_PER_SESSION;
    return std::vector<SessionCompactionCheckpoint>(
        checkpoints.begin() + start, checkpoints.end());
}

// ── resolveSessionCompactionCheckpointReason ────────────────────────────────
// 🎯 对齐: resolveSessionCompactionCheckpointReason({ trigger, timedOut })
// 将触发条件映射为原因字符串

CompactionCheckpointReason resolve_session_compaction_checkpoint_reason(
    const std::string& trigger,
    bool timed_out) {

    if (trigger == "manual") {
        return "manual";
    }
    if (timed_out) {
        return "timeout-retry";
    }
    if (trigger == "overflow") {
        return "overflow-retry";
    }
    return "auto-threshold";
}

// ── captureCompactionCheckpointSnapshot ─────────────────────────────────────
// 🎯 对齐: captureCompactionCheckpointSnapshot({ sessionManager, sessionFile })
//
// 核心快照逻辑：
// 1. 获取当前 session 的 leafId
// 2. 复制会话文件为 <name>.checkpoint.<uuid>.jsonl
// 3. 验证复制的文件可正常打开
// 4. 验证失败则清理副本

std::optional<CapturedCompactionCheckpointSnapshot> capture_compaction_checkpoint_snapshot(
    const std::string& session_file,
    const std::string& leaf_id) {

    if (session_file.empty() || leaf_id.empty()) {
        return std::nullopt;
    }

    if (!std::filesystem::exists(session_file)) {
        return std::nullopt;
    }

    // 🎯 对齐: snapshotFile = path.join(dir, `${name}.checkpoint.${randomUUID()}${ext}`)
    std::filesystem::path src_path(session_file);
    std::string snapshot_name = src_path.stem().string() +
        ".checkpoint." + core::utils::generate_uuid() +
        (src_path.extension().empty() ? ".jsonl" : src_path.extension().string());
    std::filesystem::path snapshot_path = src_path.parent_path() / snapshot_name;

    // Copy session file
    try {
        std::filesystem::copy_file(session_file, snapshot_path.string(),
            std::filesystem::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        cc_log()->warn("[compaction_checkpoints] Failed to copy session file: {}", e.what());
        return std::nullopt;
    }

    // 🎯 对齐: Verify the copied file can be reopened
    // In full implementation, we'd use SessionManager.open() to validate.
    // For now, verify the file exists and has non-zero size.
    try {
        auto file_size = std::filesystem::file_size(snapshot_path);
        if (file_size == 0) {
            // Invalid copy — cleanup
            try { std::filesystem::remove(snapshot_path); } catch (...) {}
            return std::nullopt;
        }
    } catch (...) {
        // Best-effort cleanup
        try { std::filesystem::remove(snapshot_path); } catch (...) {}
        return std::nullopt;
    }

    // Extract session ID from the session file (if available)
    std::string session_id;
    try {
        std::ifstream in(snapshot_path.string());
        std::string first_line;
        if (std::getline(in, first_line) && !first_line.empty()) {
            auto j = json::parse(first_line, nullptr, false);
            if (!j.is_discarded() && j.contains("sessionId")) {
                session_id = j["sessionId"].get<std::string>();
            }
        }
    } catch (...) {
        // Session ID extraction is best-effort
    }

    CapturedCompactionCheckpointSnapshot snapshot;
    snapshot.session_id = session_id;
    snapshot.session_file = snapshot_path.string();
    snapshot.leaf_id = leaf_id;

    cc_log()->debug("[compaction_checkpoints] Captured checkpoint: file={}, leafId={}",
        snapshot_path.string(), leaf_id);

    return snapshot;
}

// ── persistCompactionCheckpoint ─────────────────────────────────────────────
// 🎯 对齐: persistCompactionCheckpoint 部分（session store 更新）
// 将检查点信息追加到 session store 的 compactionCheckpoints 数组

bool persist_compaction_checkpoint(
    const std::string& session_key,
    const CapturedCompactionCheckpointSnapshot& snapshot,
    const CompactionCheckpointReason& reason,
    std::vector<SessionCompactionCheckpoint>& existing_checkpoints) {

    // Create new checkpoint entry
    SessionCompactionCheckpoint checkpoint;
    checkpoint.id = core::utils::generate_uuid();
    checkpoint.session_file = snapshot.session_file;
    checkpoint.leaf_id = snapshot.leaf_id;
    checkpoint.reason = reason;
    checkpoint.created_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    existing_checkpoints.push_back(checkpoint);

    // Trim to max
    existing_checkpoints = trim_session_checkpoints(existing_checkpoints);

    cc_log()->info("[compaction_checkpoints] Persisted checkpoint: session={}, id={}, reason={}",
        session_key, checkpoint.id, reason);

    return true;
}

// ── JSON serialization ──────────────────────────────────────────────────────

json compaction_checkpoint_to_json(const SessionCompactionCheckpoint& cp) {
    json j;
    j["id"] = cp.id;
    j["sessionFile"] = cp.session_file;
    j["leafId"] = cp.leaf_id;
    j["reason"] = cp.reason;
    j["createdAt"] = cp.created_at;
    return j;
}

SessionCompactionCheckpoint compaction_checkpoint_from_json(const json& j) {
    SessionCompactionCheckpoint cp;
    cp.id = j.value("id", "");
    cp.session_file = j.value("sessionFile", "");
    cp.leaf_id = j.value("leafId", "");
    cp.reason = j.value("reason", "auto-threshold");
    cp.created_at = j.value("createdAt", int64_t(0));
    return cp;
}

}  // namespace gateway
}  // namespace hunterclaw
