#pragma once

// 🎯 对齐 OpenClaw extensions/active-memory/index.ts 的 lane-1 编排核心
//   （before_prompt_build 处理器的召回决策链，剥离 gateway 特有的
//    config/subagent/timeout 细节，保留可移植的行为对齐部分）。
//
//   流程：
//     1. circuit breaker 打开 → 跳过 lane-1（返回空 prepend_context）；
//     2. 经prewarm 缓存加载 trigger 候选 ∪ 常规检索候选（runId 级复用）；
//     3. 对 message 做强 trigger 匹配 → 命中则构建 prompt prefix 注入；
//     4. escalation 决策：!strongHit && recallIntent → 标记需升级 lane-2。
//
//   circuit breaker 的 record/reset 由调用方按 lane 结果（超时/成功）驱动。

#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/active_memory_escalation.h"
#include "hunterclaw/memory/active_memory_circuit_breaker.h"
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace memory {

class MemoryIndexManager;

// lane-1 输入（对齐 before_prompt_build 处理器读取的字段）。
struct ActiveMemoryLaneOneParams {
    std::string message;                       // event.prompt
    std::string search_query;// buildSearchQuery(...) 的结果
    std::string agent_id;                      // effectiveAgentId
    std::vector<std::string> active_project_keys;
    std::optional<std::string> run_id;         // ctx.runId（用于 prewarm 缓存复用）
    std::optional<std::string> provider;       // circuit breaker key
    std::optional<std::string> model;          // circuit breaker key
    ActiveMemoryMode mode = ActiveMemoryMode::Escalate;
    // circuit breaker 配置（默认对齐 OC）。
    int circuit_breaker_max_timeouts = DEFAULT_CIRCUIT_BREAKER_MAX_TIMEOUTS;
    int64_t circuit_breaker_cooldown_ms = DEFAULT_CIRCUIT_BREAKER_COOLDOWN_MS;
    // cfg 引用相等的稳定 token（对齐 OC cfg 引用；同一 run 内应稳定）。
    std::string cfg_token;
};

// lane-1 结果（对齐 before_prompt_build 处理器的产出决策）。
struct ActiveMemoryLaneOneResult {
    // 注入到 PluginHookBeforePromptBuildResult.prepend_context 的内容（可空）。
    std::optional<std::string> prepend_context;
    bool has_strong_hit = false;   // lane-1 是否强命中（喂给 escalation）
    int injected_count = 0;        // 注入条数
    bool circuit_open = false;     // 熔断是否打开（跳过了 lane-1）
    bool should_escalate = false;  // 是否需要升级 lane-2
};

// 运行 lane-1 召回编排。manager 为空或熔断打开时安全降级。
ActiveMemoryLaneOneResult run_active_memory_lane_one(
    MemoryIndexManager* manager,
    const ActiveMemoryLaneOneParams& params);

// 对齐 agent_end: forgetTriggerRecallPrewarm(runId)。
void active_memory_forget_run(const std::optional<std::string>& run_id);

} // namespace memory
} // namespace hunterclaw
