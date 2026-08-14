#include "hunterclaw/memory/active_memory_lane.h"
#include "hunterclaw/memory/trigger_recall.h"
#include "hunterclaw/memory/memory_index_manager.h"

// 🎯 对齐 OpenClaw extensions/active-memory/index.ts的 lane-1 编排核心。

namespace hunterclaw {
namespace memory {

ActiveMemoryLaneOneResult run_active_memory_lane_one(
    MemoryIndexManager* manager,
    const ActiveMemoryLaneOneParams& params) {
    ActiveMemoryLaneOneResult result;

    // 1. circuit breaker（对齐 isCircuitBreakerOpen 门控）。
    const std::string cb_key = build_circuit_breaker_key(
        params.agent_id, params.provider, params.model);
    if (is_circuit_breaker_open(
            cb_key, params.circuit_breaker_max_timeouts, params.circuit_breaker_cooldown_ms)) {
        result.circuit_open = true;
        // 熔断时lane-1 无强命中；escalation 仍按 !strongHit && recallIntent 评估。
        result.should_escalate = should_escalate_recall(
            params.mode, params.message, /*has_strong_lane_one_hit=*/false);
        return result;
    }

    // manager 不可用 → 安全降级（对齐 OC: skips backends that cannot enumerate）。
    if (!manager) {
        result.should_escalate = should_escalate_recall(
            params.mode, params.message, /*has_strong_lane_one_hit=*/false);
        return result;
    }

    // 2. 候选加载器（对齐 loadTriggerRecallCandidates）：
    //    trigger 候选 ∪ 常规检索候选，merge 去重。加载失败返回空（对齐 .catch(()=>[])）。
    const std::string query = params.search_query;
    const std::vector<std::string> project_keys = params.active_project_keys;
    CandidateLoader loader = [manager, query, project_keys]() -> std::vector<MemorySearchResult> {
        // trigger 候选（curated，带 triggers 注解）。
        std::optional<std::vector<std::string>> keys_opt;
        if (!project_keys.empty()) keys_opt = project_keys;
        std::vector<MemorySearchResult> trigger_candidates =
            manager->list_trigger_candidates(TRIGGER_CANDIDATE_LIMIT, keys_opt);
        // 常规检索补充（限定 memory 语义；lane-1 关注 curated，检索作为补充去重来源）。
        // 対齐 trigger-recall.ts:167-176：lane-1 每条消息都跑，必须确定性、本地、
        // 禁用 query embedding（lexicalOnly），qmd 走 search 模式，并带 activeProjectKeys。
        SearchOptions opts;
        opts.max_results = TRIGGER_CANDIDATE_LIMIT;
        opts.min_score = 0.0;
        opts.sources = std::vector<std::string>{"memory"};
        opts.lexical_only = true;
        opts.qmd_search_mode_override = std::string("search");
        opts.active_project_keys = project_keys;
        std::vector<MemorySearchResult> retrieved = manager->search(query, opts);
        return merge_trigger_candidates(trigger_candidates, retrieved);
    };

    // 3. 经 prewarm 缓存复用 → 强trigger 匹配 → 构建注入上下文。
    TriggerLookupKey key;
    key.cfg_token = params.cfg_token;
    key.agent_id = params.agent_id;
    key.query = query;
    key.active_project_keys = project_keys;
    key.run_id = params.run_id;

    TriggerRecallResult recall = resolve_trigger_recall_cached(params.message, key, loader);

    result.has_strong_hit = recall.has_strong_hit;
    result.injected_count = recall.injected_count;
    if (!recall.context.empty() && recall.injected_count > 0) {
        result.prepend_context = recall.context;
    }

    // 4. escalation 决策（对齐 shouldEscalateRecall）。
    result.should_escalate = should_escalate_recall(
        params.mode, params.message, result.has_strong_hit);

    return result;
}

void active_memory_forget_run(const std::optional<std::string>& run_id) {
    // 对齐 agent_end: forgetTriggerRecallPrewarm(runId)。
    forget_trigger_recall_prewarm(run_id);
}

} // namespace memory
} // namespace hunterclaw
