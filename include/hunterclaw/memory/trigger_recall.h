#pragma once

// 🎯 严格对齐 OpenClaw extensions/active-memory/trigger-recall.ts
//   query 命中 memory 条目的 trigger 词时，额外召回对应 curated chunk（lane-1）。
//   本头文件提供可移植的纯算法层：trigger 短语打分、强匹配筛选、候选合并、
//   注解载体剥离与上下文构建。DB 侧候选拉取见
//   SQLiteVectorStorage::list_curated_trigger_candidates。

#include "hunterclaw/memory/types.h"
#include <string>
#include <vector>
#include <functional>
#include <optional>

namespace hunterclaw {
namespace memory {

// ============================================================================
// 常量（对齐 trigger-recall.ts顶部常量）
// ============================================================================
constexpr int    TRIGGER_CANDIDATE_LIMIT   = 24;    // 检索候选上限
constexpr int    TRIGGER_INJECTION_LIMIT   = 3;     // 最多注入条数
constexpr size_t MAX_TRIGGER_CONTEXT_CHARS = 1800;  // 注入上下文字符上限
// 强匹配阈值：0.65（见trigger-recall.ts 注释的合成语料测量）。
constexpr double STRONG_TRIGGER_MATCH_SCORE = 0.65;

// ============================================================================
// TriggerRecallMatch (对齐 trigger-recall.ts:TriggerRecallMatch)
//   MemorySearchResult + matchScore
// ============================================================================
struct TriggerRecallMatch {
    MemorySearchResult entry;
    double match_score = 0.0;
};

// ============================================================================
// 纯函数打分（对齐 trigger-recall.ts）
// ============================================================================

// 对齐 normalizeWords: 提取 \p{L}\p{N}_ 词，lowercase，长度>1 保留。
std::vector<std::string> normalize_words(const std::string& value);

// 对齐 splitTriggerPhrases: 按 \n; | 分割、trim、去空。
std::vector<std::string> split_trigger_phrases(const std::string& value);

// 对齐 scoreTriggerPhrase:
//   单词触发：message 含该词→0.85，否则 0。
//   多词触发：精确连续序列→1；否则 coverage*0.8 + min(1,overlap/2)*0.2。
double score_trigger_phrase(const std::string& message, const std::string& phrase);

// 对齐 scoreTriggerMatch:
//   无 triggers→0；否则 max(phraseScores)*0.8 + clamp(entry.score,0,1)*0.2。
double score_trigger_match(const std::string& message, const MemorySearchResult& entry);

// 对齐 isPromotedTrustedMemoryEntry:
//   有 project_key 时要求其所有 key 均在 active_project_keys 内；
//   origin_class 为 owner/agent→可信；
//   否则要求 source=memory 且 path 归一化后为 MEMORY.MD/USER.MD。
bool is_promoted_trusted_memory_entry(
    const MemorySearchResult& entry,
    const std::vector<std::string>& active_project_keys = {});

// 对齐 selectStrongTriggerMatches:
//   过滤可信条目→打分→阈值筛选→排序（score desc, path asc, startLine asc）→取前 3。
std::vector<TriggerRecallMatch> select_strong_trigger_matches(
    const std::string& message,
    const std::vector<MemorySearchResult>& entries,
    const std::vector<std::string>& active_project_keys = {});

// 对齐 loadTriggerRecallCandidates 的合并去重：
//   triggerCandidates ∪ retrieved，key = source:path:startLine:endLine（trigger 优先）。
std::vector<MemorySearchResult> merge_trigger_candidates(
    const std::vector<MemorySearchResult>& trigger_candidates,
    const std::vector<MemorySearchResult>& retrieved);

// 对齐 internal.ts:stripMemoryAnnotationCarriers:
//   剥离 <!-- trigger|importance|project: ... --> 载体注释，并清理行尾空白。
std::string strip_memory_annotation_carriers(const std::string& text);

// ============================================================================
// Prompt prefix 包裹（对齐 active-memory/prompt.ts + types.ts 常量）
// ============================================================================
// ACTIVE_MEMORY_PLUGIN_TAG = "active_memory_plugin"
// ACTIVE_MEMORY_CONTEXT_HEADER = "Context:"

// 对齐 escapeXml: & < > " ' →实体。
std::string escape_xml(const std::string& str);

// 对齐 buildPromptPrefix:
//   summary 为空→返回空串；否则:
//     Context:\n<active_memory_plugin>\n{escapeXml(summary)}\n</active_memory_plugin>
std::string build_active_memory_prompt_prefix(const std::string& summary);

// 对齐 buildTriggerRecallContext:
//   将强匹配拼成"- {snippet} (Source: {path}#L{startLine})"、截断到
//   MAX_TRIGGER_CONTEXT_CHARS，再经 buildPromptPrefix 包裹为可注入前缀。
//   空匹配返回空串。
std::string build_trigger_recall_context(const std::vector<TriggerRecallMatch>& matches);

// ============================================================================
// TriggerRecallResult (对齐 resolveTriggerRecall 返回类型)
// ============================================================================
struct TriggerRecallResult {
    std::string context;        // 注入上下文正文（无强匹配时为空）
    bool has_strong_hit = false;
    int injected_count = 0;
};

// 对齐 resolveTriggerRecall (trigger-recall.ts):
//   在已取得 trigger_candidates（listTriggerCandidates）与 retrieved（search）后，
//   合并去重 → 对 message 做强匹配筛选 → 构建注入上下文。
//   纯计算，不做 IO；IO 由调用方（lane-1）先行完成。
TriggerRecallResult resolve_trigger_recall(
    const std::string& message,
    const std::vector<MemorySearchResult>& trigger_candidates,
    const std::vector<MemorySearchResult>& retrieved,
    const std::vector<std::string>& active_project_keys = {});

// ============================================================================
// Prewarm 缓存（对齐 trigger-recall.ts 的 triggerRecallPrewarms 机制）
//
//   OC 用Map<runId, {cfg, agentId, query, activeProjectKeys, promise}> 缓存
//   "候选加载"的 Promise，使 agent_start 期间的预热与 lane-1 的实际召回复用
//   同一次lexical+trigger-candidate 查询。C++ 侧以 std::shared_future 承载，
//   加载 IO 通过 CandidateLoader 回调注入，保持本模块与 manager 解耦。
//
//   参数一致性判定（对齐 OC existing 复用条件）：
//     cfg_token（对齐 cfg 引用相等）、agent_id、query、active_project_keys 逐项相等。
// ============================================================================

// 合并去重后的候选加载器（对齐 loadTriggerRecallCandidates 的产出）。
//   实现方通常执行 manager.search(query, lexicalOnly) ∪ manager.listTriggerCandidates，
//   再 merge_trigger_candidates 去重。加载失败可返回空 vector（对齐 OC .catch(()=>[])）。
using CandidateLoader =
    std::function<std::vector<MemorySearchResult>()>;

struct TriggerLookupKey {
    std::string cfg_token;                // 对齐 OC cfg 引用相等（调用方传稳定 token）
    std::string agent_id;
    std::string query;
    std::vector<std::string> active_project_keys;
    std::optional<std::string> run_id;           // 无 run_id → 不缓存，直接加载
};

// 对齐 resolveTriggerRecallCandidates:
//   无 run_id → 直接调用 loader；有 run_id → 命中且参数全一致则复用 future，
//   否则新建并存入缓存（加载失败时若仍是同一 entry 则自动移除）。
//   返回合并去重后的候选。
std::vector<MemorySearchResult> resolve_trigger_recall_candidates(
    const TriggerLookupKey& key,
    const CandidateLoader& loader);

// 对齐 prewarmTriggerRecall: 触发一次候选加载并缓存（结果被丢弃，仅为预热）。
void prewarm_trigger_recall(
    const TriggerLookupKey& key,
    const CandidateLoader& loader);

// 对齐 resolveTriggerRecall 的完整流程（含缓存复用）：
//   候选加载（经缓存）→ 强匹配筛选 → 注入上下文。
TriggerRecallResult resolve_trigger_recall_cached(
    const std::string& message,
    const TriggerLookupKey& key,
    const CandidateLoader& loader);

// 对齐 forgetTriggerRecallPrewarm: 移除某 run_id 的预热缓存（run_id 空则无操作）。
void forget_trigger_recall_prewarm(const std::optional<std::string>& run_id);

// 对齐 resetTriggerRecallPrewarmsForTests: 清空全部预热缓存。
void reset_trigger_recall_prewarms();

} // namespace memory
} // namespace hunterclaw
