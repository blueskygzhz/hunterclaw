#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/memory/types.h"
#include <memory>

namespace hunterclaw {
namespace tools {

// 🎯 100% 对齐 OpenClaw: memory_search Tool
class MemorySearchTool : public Tool {
public:
    explicit MemorySearchTool(std::shared_ptr<memory::MemorySearchManager> manager);

    std::string name() const override { return "memory_search"; }

    std::string description() const override {
        return "Mandatory recall step: semantically search MEMORY.md + memory/*.md "
               "(and optional session transcripts) before answering questions about "
               "prior work, decisions, dates, people, preferences, or todos. "
               "Optional `corpus=wiki` or `corpus=all` also searches registered "
               "compiled-wiki supplements. "
               "If response has disabled=true, memory retrieval is unavailable "
               "and should be surfaced to the user.";
    }

    Schema parameters() const override {
        return Schema{
            "object",
            "Memory search parameters",
            {
                {"query",      core::JsonValue::object({
                    {"type",        core::JsonValue("string")},
                    {"description", core::JsonValue("Search query text")}
                })},
                {"maxResults", core::JsonValue::object({
                    {"type",        core::JsonValue("number")},
                    {"description", core::JsonValue("Maximum number of results to return (default: 6)")}
                })},
                {"minScore",   core::JsonValue::object({
                    {"type",        core::JsonValue("number")},
                    {"description", core::JsonValue("Minimum similarity score (0-1, default: 0.35)")}
                })},
                {"corpus",     core::JsonValue::object({
                    {"type",        core::JsonValue("string")},
                    {"description", core::JsonValue("Corpus to search: \"memory\" (default), \"wiki\", or \"all\"")}
                })},
            },
            {"query"}
        };
    }

    // OpenClaw: returns { results: MemorySearchResult[], provider, model, fallback, citations, mode }
    ToolResult execute(const core::JsonValue& params) override;

private:
    std::shared_ptr<memory::MemorySearchManager> manager_;
};

// 🎯 100% 对齐 OpenClaw: memory_get Tool
class MemoryGetTool : public Tool {
public:
    explicit MemoryGetTool(std::shared_ptr<memory::MemorySearchManager> manager);

    std::string name() const override { return "memory_get"; }

    std::string description() const override {
        return "Safe snippet read from MEMORY.md or memory/*.md with optional "
               "from/lines; `corpus=wiki` reads from registered compiled-wiki "
               "supplements. Use after search to pull only the needed lines "
               "and keep context small.";
    }

    Schema parameters() const override {
        return Schema{
            "object",
            "Memory get parameters",
            {
                {"path",  core::JsonValue::object({
                    {"type",        core::JsonValue("string")},
                    {"description", core::JsonValue("Relative path to memory file (e.g., 'MEMORY.md' or 'memory/2024-03.md')")}
                })},
                {"from",  core::JsonValue::object({
                    {"type",        core::JsonValue("number")},
                    {"description", core::JsonValue("Starting line number (1-indexed, optional)")}
                })},
                {"lines", core::JsonValue::object({
                    {"type",        core::JsonValue("number")},
                    {"description", core::JsonValue("Number of lines to read (optional, default: all)")}
                })},
                {"corpus", core::JsonValue::object({
                    {"type",        core::JsonValue("string")},
                    {"description", core::JsonValue("Corpus to read from: \"memory\" (default), \"wiki\", or \"all\"")}
                })},
            },
            {"path"}
        };
    }

    // OpenClaw: returns { path, text, disabled?, error? }
    ToolResult execute(const core::JsonValue& params) override;

private:
    std::shared_ptr<memory::MemorySearchManager> manager_;
};

// 🎯 对齐 OpenClaw: Memory Tool Factory
class MemoryToolFactory {
public:
    // 创建 memory_search 和 memory_get 工具 (对齐 openclaw memory-tool.ts)
    static std::vector<std::shared_ptr<Tool>> create_memory_tools(
        std::shared_ptr<memory::MemorySearchManager> manager
    );

    // 单独创建
    static std::shared_ptr<Tool> create_memory_search(
        std::shared_ptr<memory::MemorySearchManager> manager
    );

    static std::shared_ptr<Tool> create_memory_get(
        std::shared_ptr<memory::MemorySearchManager> manager
    );
};

} // namespace tools
} // namespace hunterclaw
