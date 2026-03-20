#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/memory/types.h"
#include "hunterclaw/core/result.h"
#include <memory>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

// 🎯 100% 对齐 OpenClaw: memory_search Tool
class MemorySearchTool : public Tool {
public:
    explicit MemorySearchTool(std::shared_ptr<memory::MemorySearchManager> manager);
    
    std::string name() const override { return "memory_search"; }
    
    std::string description() const override {
        return "Mandatory recall step: semantically search MEMORY.md + memory/*.md "
               "(and optional session transcripts) before answering questions about "
               "prior work, decisions, dates, people, preferences, or todos; "
               "returns top snippets with path + lines. "
               "If response has disabled=true, memory retrieval is unavailable "
               "and should be surfaced to the user.";
    }
    
    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"query", {
                    {"type", "string"},
                    {"description", "Search query text"}
                }},
                {"maxResults", {
                    {"type", "number"},
                    {"description", "Maximum number of results to return (default: 6)"}
                }},
                {"minScore", {
                    {"type", "number"},
                    {"description", "Minimum similarity score (0-1, default: 0.35)"}
                }}
            }},
            {"required", {"query"}}
        };
    }
    
    // OpenClaw: returns { results: MemorySearchResult[], provider, model, citations, mode }
    core::Result<json> execute(const json& params) override;

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
               "from/lines; use after memory_search to pull only the needed lines "
               "and keep context small.";
    }
    
    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description", "Relative path to memory file (e.g., 'MEMORY.md' or 'memory/2024-03.md')"}
                }},
                {"from", {
                    {"type", "number"},
                    {"description", "Starting line number (1-indexed, optional)"}
                }},
                {"lines", {
                    {"type", "number"},
                    {"description", "Number of lines to read (optional, default: all)"}
                }}
            }},
            {"required", {"path"}}
        };
    }
    
    // OpenClaw: returns { path, text, disabled?, error? }
    core::Result<json> execute(const json& params) override;

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
