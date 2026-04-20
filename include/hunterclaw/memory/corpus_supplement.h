#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

// 严格对齐 OpenClaw src/plugins/memory-state.ts (328行)
// + extensions/memory-core/src/tools.shared.ts (185行)
// Corpus Supplement 注册 + search/get 路由

namespace hunterclaw {
namespace memory {

// ============================================================================
// MemoryCorpusSearchResult (对齐 memory-state.ts:10-26)
// ============================================================================
struct MemoryCorpusSearchResult {
    std::string corpus;
    std::string path;
    std::optional<std::string> title;
    std::optional<std::string> kind;
    double score = 0.0;
    std::string snippet;
    std::optional<std::string> id;
    std::optional<int> start_line;
    std::optional<int> end_line;
    std::optional<std::string> citation;
    std::optional<std::string> source;
    std::optional<std::string> provenance_label;
    std::optional<std::string> source_type;
    std::optional<std::string> source_path;
    std::optional<std::string> updated_at;
};

// ============================================================================
// MemoryCorpusGetResult (对齐 memory-state.ts:28-41)
// ============================================================================
struct MemoryCorpusGetResult {
    std::string corpus;
    std::string path;
    std::optional<std::string> title;
    std::optional<std::string> kind;
    std::string content;
    int from_line = 0;
    int line_count = 0;
    std::optional<std::string> id;
    std::optional<std::string> provenance_label;
    std::optional<std::string> source_type;
    std::optional<std::string> source_path;
    std::optional<std::string> updated_at;
};

// ============================================================================
// MemoryCorpusSupplement (对齐 memory-state.ts:43-55)
// ============================================================================
struct CorpusSearchParams {
    std::string query;
    std::optional<int> max_results;
    std::optional<std::string> agent_session_key;
};

struct CorpusGetParams {
    std::string lookup;
    std::optional<int> from_line;
    std::optional<int> line_count;
    std::optional<std::string> agent_session_key;
};

class MemoryCorpusSupplement {
public:
    virtual ~MemoryCorpusSupplement() = default;

    virtual std::vector<MemoryCorpusSearchResult> search(
        const CorpusSearchParams& params) = 0;

    virtual std::optional<MemoryCorpusGetResult> get(
        const CorpusGetParams& params) = 0;
};

// ============================================================================
// MemoryCorpusSupplementRegistration (对齐 memory-state.ts:57-60)
// ============================================================================
struct MemoryCorpusSupplementRegistration {
    std::string plugin_id;
    std::shared_ptr<MemoryCorpusSupplement> supplement;
};

// ============================================================================
// Registration API (对齐 memory-state.ts:159-168 + 186-188)
// ============================================================================

// 注册 (对齐 registerMemoryCorpusSupplement)
// 去重逻辑：按 plugin_id 过滤旧的，再 push 新的
void register_memory_corpus_supplement(
    const std::string& plugin_id,
    std::shared_ptr<MemoryCorpusSupplement> supplement
);

// 列举 (对齐 listMemoryCorpusSupplements)
std::vector<MemoryCorpusSupplementRegistration> list_memory_corpus_supplements();

// ============================================================================
// Search/Get 路由 (对齐 tools.shared.ts:139-184)
// ============================================================================

// 对齐 searchMemoryCorpusSupplements (tools.shared.ts:139-165)
// corpus == "memory" → 返回空
// 否则调用所有已注册 supplement 的 search()，合并结果按 score 降序，取 top N
std::vector<MemoryCorpusSearchResult> search_memory_corpus_supplements(
    const std::string& query,
    std::optional<int> max_results = std::nullopt,
    const std::string& agent_session_key = "",
    const std::string& corpus = "all"
);

// 对齐 getMemoryCorpusSupplementResult (tools.shared.ts:167-184)
// corpus == "memory" → 返回 nullopt
// 遍历所有 supplement，第一个返回非 null 的结果即终止
std::optional<MemoryCorpusGetResult> get_memory_corpus_supplement_result(
    const std::string& lookup,
    std::optional<int> from_line = std::nullopt,
    std::optional<int> line_count = std::nullopt,
    const std::string& agent_session_key = "",
    const std::string& corpus = "all"
);

// ============================================================================
// Prompt Supplement (对齐 memory-state.ts:5-8 + 195-227)
//
// 与 CorpusSupplement 对应的"静态提示"侧，由插件往 system prompt 追加一段文案
// （例如 memory-wiki 往 system 加 "## Compiled Wiki" 工具指引 + digest 摘要）。
//
// 区别：
//   CorpusSupplement  = LLM 按需调 memory_search/memory_get → 触发检索
//   PromptSupplement  = Agent run 启动时组装 system prompt → 告诉 LLM "有 wiki 可用"
// ============================================================================

struct MemoryPromptSectionParams {
    std::set<std::string> available_tools;   // 如 {"memory_search", "memory_get", "wiki_search"}
    std::string citations_mode = "auto";      // "on" | "off" | "auto"
};

// 对齐 memory-state.ts:5-8 MemoryPromptSectionBuilder:
//   (params) => string[]
using MemoryPromptSectionBuilder =
    std::function<std::vector<std::string>(const MemoryPromptSectionParams&)>;

struct MemoryPromptSupplementRegistration {
    std::string plugin_id;
    MemoryPromptSectionBuilder builder;
};

// 注册 (对齐 registerMemoryPromptSupplement memory-state.ts:195-204)
// 按 plugin_id 去重后 push
void register_memory_prompt_supplement(
    const std::string& plugin_id,
    MemoryPromptSectionBuilder builder
);

// 列举 (对齐 listMemoryPromptSupplements memory-state.ts:225-227)
// 返回的顺序按注册时序；聚合时由 build_memory_prompt_section 按 plugin_id 再排序
std::vector<MemoryPromptSupplementRegistration> list_memory_prompt_supplements();

// ============================================================================
// Memory Plugin Capability (对齐 memory-state.ts:127-184)
//
// OC 里 registerMemoryCapability 是 "unified registration"——一个插件一次性声明
// 它的 promptBuilder / flushPlanResolver / runtime / publicArtifacts。HC 简化：
// 当前只用到 promptBuilder（flushPlan 已用 build_memory_flush_plan 走默认实现）。
//
// capability.promptBuilder 的优先级高于 legacy 的静态 build_memory_prompt_section()。
// ============================================================================

struct MemoryPluginCapability {
    // primary promptBuilder（插件独占唯一槽位；后注册覆盖前注册）
    MemoryPromptSectionBuilder prompt_builder;
    // 其它字段（flushPlanResolver / runtime / publicArtifacts）暂不接入
    // 留空以便后续按需扩展
};

struct MemoryPluginCapabilityRegistration {
    std::string plugin_id;
    MemoryPluginCapability capability;
};

// 对齐 registerMemoryCapability (memory-state.ts:170-175)
// 全局 exclusive slot：同名 plugin_id 会覆盖
void register_memory_capability(
    const std::string& plugin_id,
    MemoryPluginCapability capability
);

// 对齐 getMemoryCapabilityRegistration (memory-state.ts:177-184)
std::optional<MemoryPluginCapabilityRegistration> get_memory_capability_registration();

}  // namespace memory
}  // namespace hunterclaw
