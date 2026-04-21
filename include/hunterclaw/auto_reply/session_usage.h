#ifndef HUNTERCLAW_AUTO_REPLY_SESSION_USAGE_H
#define HUNTERCLAW_AUTO_REPLY_SESSION_USAGE_H

// ============================================================================
// 严格对齐 OpenClaw:
//   src/auto-reply/reply/session-run-accounting.ts (1.39KB)
//   src/auto-reply/reply/session-usage.ts           (7.20KB)
//   src/auto-reply/reply/session-updates.ts         (11.53KB)
//
// 本头文件暴露「每轮 LLM run 结束后把 usage 回写 session store」的 API。
// 这条链路在 HC 中历史性缺失（build_session_usage_patch 定义了但无调用点），
// 导致 preflight_memory 的 fresh 门控只能靠 agent_instance 启动期从 session
// store 首次加载到的值——实际效果近似「只在重启后第一轮生效」。
//
// 修复方式：ReactEngine 每轮结束后通过 on_run_complete_fn 回调调用 persist
// API，严格对齐 OC 的 updateSessionAccounting(...)。
// ============================================================================

#include "hunterclaw/sessions/session_manager.h"
#include <cstdint>
#include <optional>
#include <string>

namespace hunterclaw::auto_reply {

// 对齐 OC: session-run-accounting.ts SessionRunAccounting
struct SessionRunAccounting {
    int64_t started_at = 0;
    int64_t completed_at = 0;
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;
    int cache_write_tokens = 0;
    std::string model_provider;
    std::string model;
    bool is_heartbeat = false;
};

// 对齐 OC: session-usage.ts SessionUsagePatch
//
// ⚠️ total_tokens_fresh 必须是 bool（OC 原始类型是 boolean）：
//   true  = 本轮有实际 LLM usage，持久化的 totalTokens 是当前的
//   false = 本轮没跑 LLM（heartbeat / 跳过），totalTokens 沿用旧值但标记陈旧
struct SessionUsagePatch {
    std::optional<int> total_tokens;
    std::optional<bool> total_tokens_fresh;
    std::optional<int> input_tokens;
    std::optional<int> output_tokens;
    std::optional<int> compaction_count;
    std::optional<std::string> model_provider;
    std::optional<std::string> model;
    std::optional<int64_t> updated_at;
};

// 对齐 OC: session-run-accounting.ts computeRunDurationMs / computeTotalTokens
int64_t compute_run_duration_ms(const SessionRunAccounting& acct);
int     compute_total_tokens(const SessionRunAccounting& acct);

// 对齐 OC: session-usage.ts buildSessionUsagePatch
//
// existing_total_tokens: session entry 当前持久化的 totalTokens
// 返回 patch：
//   total_tokens = existing + run_total
//   total_tokens_fresh = (run_total > 0)   ← 布尔，对齐 OC
//   input/output 设为本次 run 的值（对齐 OC：per-run 覆盖，不累加）
//   model / model_provider / updated_at 记录本次 run 的模型与时间
SessionUsagePatch build_session_usage_patch(
    const SessionRunAccounting& acct,
    int existing_total_tokens);

// 对齐 OC: session-updates.ts applySessionUpdates
// 把 patch 应用到 SessionEntry 并写回 SessionManager。
// session_key 不存在时静默返回（与 OC 一致，避免 race）。
void persist_session_entry_update(
    sessions::SessionManager& session_manager,
    const std::string& session_key,
    const std::string& agent_id,
    const SessionUsagePatch& patch);

// 组合便利函数：一步完成「build + persist」，供 ReactEngine on_run_complete_fn
// 回调使用。对齐 OC agent-runner-utils.ts: updateSessionAccounting(...)。
void update_session_accounting(
    sessions::SessionManager& session_manager,
    const std::string& session_key,
    const std::string& agent_id,
    const SessionRunAccounting& acct);

}  // namespace hunterclaw::auto_reply

#endif  // HUNTERCLAW_AUTO_REPLY_SESSION_USAGE_H
