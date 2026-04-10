#include "hunterclaw/memory/prompt_section.h"

// 严格对齐 OpenClaw extensions/memory-core/src/prompt-section.ts (38 行)

namespace hunterclaw {
namespace memory {

std::vector<std::string> build_memory_prompt_section(
    const std::set<std::string>& available_tools,
    const std::string& citations_mode
) {
    // (对齐 prompt-section.ts:7-8)
    bool has_memory_search = available_tools.count("memory_search") > 0;
    bool has_memory_get = available_tools.count("memory_get") > 0;

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

}  // namespace memory
}  // namespace hunterclaw
