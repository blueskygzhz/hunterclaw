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
// ⚠️ total_tokens 语义（严格对齐 OC agents/usage.ts::deriveSessionTotalTokens）：
//
//   SessionEntry.totalTokens 是 "**当前 context 大小的 snapshot**"，
//   **不是** 跨 run 累加值。派生公式：
//
//       totalTokens = input + cacheRead + cacheWrite
//                     （不含 output —— output 不进下一轮 context input）
//
//   数据源优先级：
//     1. promptTokens override（如手动指定）
//     2. lastCallUsage（run 末最后一次 LLM 调用，反映真实当前 context）
//     3. 累加 usage（兜底，HC 当前使用此近似）
//
//   每次 run 结束**覆盖写入**，而非累加。heartbeat/skip 类空转时
//   不更新此字段（保留上次可信值）。
//
// ⚠️ total_tokens_fresh：
//   OC 原始语义：`totalTokensFresh = typeof totalTokens === "number"`
//   即：patch 里存在 totalTokens 数值 → fresh=true；undefined → 不写
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

// 对齐 OC: agents/usage.ts:deriveSessionTotalTokens
//
// 从 run accounting 派生 SessionEntry.totalTokens 值：
//   input + cacheRead + cacheWrite（不含 output）
// 非正值返回 0（对齐 OC "undefined" 语义）。
int derive_session_total_tokens(const SessionRunAccounting& acct);

// 对齐 OC: session-usage.ts:persistSessionUsageUpdate 核心逻辑
//
// ⚠️ 与旧实现的关键差异：
//   - totalTokens **不再累加**（existing_total_tokens 参数忽略）
//   - totalTokens = derive_session_total_tokens(acct) —— per-turn 覆盖
//   - run_total=0（heartbeat/skip）时 patch 不设 totalTokens/fresh，保留旧值
//
// existing_total_tokens 参数保留只为 API 向后兼容。
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
