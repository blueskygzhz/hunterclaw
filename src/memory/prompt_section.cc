#include "hunterclaw/memory/prompt_section.h"
#include "hunterclaw/memory/corpus_supplement.h"  // ✅ 对齐 memory-state.ts: capability + prompt supplements
#include <algorithm>

// 严格对齐 OpenClaw:
//   src/plugins/memory-state.ts:206-219 buildMemoryPromptSection
//   extensions/memory-core/src/prompt-section.ts (38 行)
//
// 架构（对齐 OC）：
//   1) primary section: 优先从 memory capability 的 promptBuilder 获取（例如由
//      memory-core 插件注册），其次才 fallback 到本文件内的 legacy 静态实现
//   2) supplements: listMemoryPromptSupplements() → 按 plugin_id 排序 → flatMap builder
//   3) 返回 [...primary, ...supplements]

namespace hunterclaw {
namespace memory {

namespace {

// 对齐 prompt-section.ts (extensions/memory-core)：legacy 默认 promptBuilder。
// 当没有任何 plugin 注册 capability.promptBuilder 时使用。
std::vector<std::string> default_memory_prompt_builder(
    const std::set<std::string>& available_tools,
    const std::string& citations_mode
) {
    // (对齐 prompt-section.ts:7-8)
    bool has_memory_search = available_tools.count("memory_search") > 0;
    bool has_memory_get    = available_tools.count("memory_get") > 0;

    // (对齐 prompt-section.ts:10-12)
    if (!has_memory_search && !has_memory_get) {
        return {};
    }

    // (对齐 prompt-section.ts:14-24)
    std::string tool_guidance;
    if (has_memory_search && has_memory_get) {
        tool_guidance =
            "Before answering anything about prior work, decisions, dates, people, preferences, "
            "or todos: run memory_search on MEMORY.md + memory/*.md + indexed session transcripts; "
            "then use memory_get to pull only the needed lines. If low confidence after search, say you checked.";
    } else if (has_memory_search) {
        tool_guidance =
            "Before answering anything about prior work, decisions, dates, people, preferences, "
            "or todos: run memory_search on MEMORY.md + memory/*.md + indexed session transcripts "
            "and answer from the matching results. If low confidence after search, say you checked.";
    } else {
        tool_guidance =
            "Before answering anything about prior work, decisions, dates, people, preferences, "
            "or todos that already point to a specific memory file or note: run memory_get to pull "
            "only the needed lines. If low confidence after reading them, say you checked.";
    }

    // (对齐 prompt-section.ts:26-36)
    std::vector<std::string> lines = {"## Memory Recall", tool_guidance};
    if (citations_mode == "off") {
        lines.push_back(
            "Citations are disabled: do not mention file paths or line numbers in replies "
            "unless the user explicitly asks.");
    } else {
        lines.push_back(
            "Citations: include Source: <path#line> when it helps the user verify memory snippets.");
    }
    lines.emplace_back("");  // trailing blank line
    return lines;
}

}  // anon

std::vector<std::string> build_memory_prompt_section(
    const std::set<std::string>& available_tools,
    const std::string& citations_mode
) {
    MemoryPromptSectionParams params;
    params.available_tools = available_tools;
    params.citations_mode  = citations_mode;

    // ────────────────────────────────────────────────────────────────────
    // ① Primary section — 对齐 memory-state.ts:210-213:
    //    const primary =
    //        memoryPluginState.capability?.capability.promptBuilder?.(params) ??
    //        memoryPluginState.promptBuilder?.(params) ??
    //        [];
    // HC 简化：capability.promptBuilder 优先；未注册则 fallback 到 static 实现
    // ────────────────────────────────────────────────────────────────────
    std::vector<std::string> primary;
    auto capability = get_memory_capability_registration();
    if (capability && capability->capability.prompt_builder) {
        try {
            primary = capability->capability.prompt_builder(params);
        } catch (...) {
            primary.clear();
        }
    }
    if (primary.empty()) {
        primary = default_memory_prompt_builder(available_tools, citations_mode);
    }

    // ────────────────────────────────────────────────────────────────────
    // ② Supplements — 对齐 memory-state.ts:214-218:
    //    const supplements = memoryPluginState.promptSupplements
    //      .toSorted((l, r) => l.pluginId.localeCompare(r.pluginId))
    //      .flatMap((registration) => registration.builder(params));
    // ────────────────────────────────────────────────────────────────────
    auto supplements_regs = list_memory_prompt_supplements();
    std::sort(supplements_regs.begin(), supplements_regs.end(),
        [](const MemoryPromptSupplementRegistration& a,
           const MemoryPromptSupplementRegistration& b) {
            return a.plugin_id < b.plugin_id;
        });

    std::vector<std::string> supplements;
    for (const auto& reg : supplements_regs) {
        if (!reg.builder) continue;
        try {
            auto lines = reg.builder(params);
            supplements.insert(supplements.end(),
                               std::make_move_iterator(lines.begin()),
                               std::make_move_iterator(lines.end()));
        } catch (...) {
            // Best-effort: 单个 supplement 异常不影响整体 prompt 组装
        }
    }

    // ────────────────────────────────────────────────────────────────────
    // ③ 合并 primary + supplements（对齐 return [...primary, ...supplements]）
    // ────────────────────────────────────────────────────────────────────
    std::vector<std::string> result;
    result.reserve(primary.size() + supplements.size());
    result.insert(result.end(),
                  std::make_move_iterator(primary.begin()),
                  std::make_move_iterator(primary.end()));
    result.insert(result.end(),
                  std::make_move_iterator(supplements.begin()),
                  std::make_move_iterator(supplements.end()));
    return result;
}

}  // namespace memory
}  // namespace hunterclaw
