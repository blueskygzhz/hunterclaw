#pragma once

// ============================================================================
// Prompt Section Builder — 严格对齐 OpenClaw prompt-section.ts (38 行)
//
// 根据可用工具和 citations 模式动态构建 "## Memory Recall" 系统提示词段落。
// 插入到 Agent 系统提示词中，指导 LLM 何时/如何调用 memory_search / memory_get。
// ============================================================================

#include <string>
#include <vector>
#include <set>

namespace hunterclaw {
namespace memory {

/**
 * Build the "## Memory Recall" system prompt section.
 * (对齐 prompt-section.ts:3-38 buildPromptSection)
 *
 * @param available_tools Set of available tool names (e.g. "memory_search", "memory_get")
 * @param citations_mode "on" | "off" | "auto"
 * @return Lines to append to system prompt, empty if no memory tools available
 */
std::vector<std::string> build_memory_prompt_section(
    const std::set<std::string>& available_tools,
    const std::string& citations_mode = "auto"
);

}  // namespace memory
}  // namespace hunterclaw
