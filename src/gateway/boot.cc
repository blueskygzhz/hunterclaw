// ============================================================================
// boot.cc
// 🎯 严格对齐 OpenClaw: src/gateway/boot.ts (205 行)
//
// 网关启动引导流程：
// 1. 读取工作区的 BOOT.md 文件
// 2. 快照当前会话映射
// 3. 通过 Agent 执行 BOOT.md 指令
// 4. 恢复原始会话映射（boot 不应改变持久化状态）
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <string>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ── Constants ────────────────────────────────────────────────────────────────
// 🎯 对齐: BOOT_FILENAME = "BOOT.md"
static const std::string BOOT_FILENAME = "BOOT.md";
static const std::string SILENT_REPLY_TOKEN = "NO_REPLY";

// ── Types ────────────────────────────────────────────────────────────────────

// 🎯 对齐: BootRunResult
struct BootRunResult {
    enum class Status { Skipped, Ran, Failed };
    Status status;
    std::string reason;  // "missing" | "empty" for Skipped; error message for Failed
};

// 🎯 对齐: SessionMappingSnapshot
struct SessionMappingSnapshot {
    std::string store_path;
    std::string session_key;
    bool can_restore = false;
    bool had_entry = false;
    json entry;  // saved session entry for restore
};

// ── generateBootSessionId ───────────────────────────────────────────────────
// 🎯 对齐: generateBootSessionId()
// Format: "boot-{timestamp}-{uuid8}"

static std::string generate_boot_session_id() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    gmtime_r(&time_t, &tm_buf);

    std::ostringstream oss;
    oss << "boot-"
        << std::put_time(&tm_buf, "%Y-%m-%d_%H-%M-%S")
        << "-" << core::utils::generate_uuid().substr(0, 8);
    return oss.str();
}

// ── buildBootPrompt ─────────────────────────────────────────────────────────
// 🎯 对齐: buildBootPrompt(content)
// Constructs the prompt from BOOT.md content

static std::string build_boot_prompt(const std::string& content) {
    std::ostringstream oss;
    oss << "You are running a boot check. Follow BOOT.md instructions exactly.\n"
        << "\n"
        << "BOOT.md:\n"
        << content << "\n"
        << "\n"
        << "If BOOT.md asks you to send a message, use the message tool "
        << "(action=send with channel + target).\n"
        << "Use the `target` field (not `to`) for message tool destinations.\n"
        << "After sending with the message tool, reply with ONLY: "
        << SILENT_REPLY_TOKEN << ".\n"
        << "If nothing needs attention, reply with ONLY: "
        << SILENT_REPLY_TOKEN << ".";
    return oss.str();
}

// ── loadBootFile ────────────────────────────────────────────────────────────
// 🎯 对齐: loadBootFile(workspaceDir)

struct BootFileResult {
    std::string status;  // "ok" | "missing" | "empty"
    std::string content;
};

static BootFileResult load_boot_file(const std::string& workspace_dir) {
    std::string boot_path = (std::filesystem::path(workspace_dir) / BOOT_FILENAME).string();

    if (!std::filesystem::exists(boot_path)) {
        return {"missing", ""};
    }

    std::ifstream in(boot_path);
    if (!in.is_open()) {
        return {"missing", ""};
    }

    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

    // Trim
    size_t start = content.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return {"empty", ""};
    }
    size_t end = content.find_last_not_of(" \t\n\r");
    std::string trimmed = content.substr(start, end - start + 1);

    if (trimmed.empty()) {
        return {"empty", ""};
    }

    return {"ok", trimmed};
}

// ── runBootOnce ─────────────────────────────────────────────────────────────
// 🎯 对齐: runBootOnce(params)
//
// 完整流程:
// 1. 读取 BOOT.md
// 2. 如果缺失/空则跳过
// 3. 快照会话映射
// 4. 构建 prompt 并执行 Agent
// 5. 恢复会话映射
// 6. 返回结果

BootRunResult run_boot_once(
    const std::string& workspace_dir,
    const std::string& agent_id,
    const std::function<bool(const std::string& message, const std::string& session_id,
                             const std::string& session_key)>& run_agent) {

    // Step 1: Read BOOT.md
    BootFileResult boot_file;
    try {
        boot_file = load_boot_file(workspace_dir);
    } catch (const std::exception& e) {
        return {BootRunResult::Status::Failed, std::string("Failed to read BOOT.md: ") + e.what()};
    }

    // Step 2: Skip if missing or empty
    if (boot_file.status == "missing") {
        return {BootRunResult::Status::Skipped, "missing"};
    }
    if (boot_file.status == "empty") {
        return {BootRunResult::Status::Skipped, "empty"};
    }

    // Step 3: Build prompt
    std::string prompt = build_boot_prompt(boot_file.content);
    std::string session_id = generate_boot_session_id();

    // 🎯 对齐: session key resolution
    // In full implementation, resolve from config. For now use agent-based key.
    std::string session_key = agent_id.empty()
        ? "main" : ("agent:" + agent_id + ":main");

    // Step 4: Execute agent
    std::string agent_failure;
    try {
        if (run_agent) {
            bool ok = run_agent(prompt, session_id, session_key);
            if (!ok) {
                agent_failure = "agent run returned failure";
            }
        }
    } catch (const std::exception& e) {
        agent_failure = e.what();
    }

    // Step 5: 恢复会话映射
    // 🎯 对齐: restoreMainSessionMapping(snapshot)
    // NOTE: Full implementation would snapshot/restore the session store.
    // Boot should not leave persistent state changes.

    // Step 6: Return result
    if (agent_failure.empty()) {
        return {BootRunResult::Status::Ran, ""};
    }
    return {BootRunResult::Status::Failed,
            "agent run failed: " + agent_failure};
}

// ── JSON serialization ──────────────────────────────────────────────────────

json boot_run_result_to_json(const BootRunResult& result) {
    json j;
    switch (result.status) {
        case BootRunResult::Status::Skipped:
            j["status"] = "skipped";
            j["reason"] = result.reason;
            break;
        case BootRunResult::Status::Ran:
            j["status"] = "ran";
            break;
        case BootRunResult::Status::Failed:
            j["status"] = "failed";
            j["reason"] = result.reason;
            break;
    }
    return j;
}

}  // namespace gateway
}  // namespace hunterclaw
