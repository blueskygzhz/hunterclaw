#include "hunterclaw/tools/memory_tools.h"
#include "hunterclaw/core/logging.h"
#include <chrono>

namespace hunterclaw {
namespace tools {

using namespace hunterclaw::memory;

// ========================================
// 辅助函数 (严格对齐 OpenClaw)
// ========================================

// 对齐 OpenClaw: formatCitation(entry: MemorySearchResult): string
static std::string format_citation(const std::string& path, int start_line, int end_line) {
    std::string citation = path;
    if (start_line == end_line) {
        citation += "#L" + std::to_string(start_line);
    } else {
        citation += "#L" + std::to_string(start_line) + "-L" + std::to_string(end_line);
    }
    return citation;
}

// ========================================
// MemorySearchTool Implementation
// ========================================

MemorySearchTool::MemorySearchTool(std::shared_ptr<MemoryProvider> provider)
    : provider_(provider) {}

core::Result<json> MemorySearchTool::execute(const json& params) {
    auto logger = core::Logger::get();
    
    // 1. 提取参数 (对齐 OpenClaw)
    if (!params.contains("query") || !params["query"].is_string()) {
        return core::Err("Parameter 'query' is required and must be a string");
    }
    
    std::string query = params["query"];
    
    // 默认值对齐 OpenClaw
    SearchOptions options;
    options.limit = params.value("maxResults", 6);
    options.min_score = params.value("minScore", 0.35);
    
    logger->info("Memory search: query='{}' maxResults={} minScore={}", 
                 query, options.limit, options.min_score);
    
    // 2. 执行搜索
    auto result = provider_->search_memories(query, options);
    
    if (!result) {
        // OpenClaw: 返回 disabled=true 表示不可用
        logger->error("Memory search failed: {}", result.error());
        return core::Ok(json{
            {"results", json::array()},
            {"disabled", true},
            {"unavailable", true},
            {"error", result.error()},
            {"warning", "Memory search is unavailable due to an embedding/provider error."},
            {"action", "Check embedding provider configuration and retry memory_search."}
        });
    }
    
    auto memories = result.value();
    
    // 3. 转换为 OpenClaw 格式
    json results = json::array();
    
    // TODO: 从配置中获取 citations 模式
    bool include_citations = true;  // 默认 auto (在 direct chat 中显示)
    
    for (const auto& mem : memories) {
        // OpenClaw MemorySearchResult 格式
        int start_line = mem.metadata.count("start_line") ? std::stoi(mem.metadata.at("start_line")) : 1;
        int end_line = mem.metadata.count("end_line") ? std::stoi(mem.metadata.at("end_line")) : 1;
        std::string path = mem.metadata.count("path") ? mem.metadata.at("path") : "unknown";
        
        json item = {
            {"path", path},
            {"startLine", start_line},
            {"endLine", end_line},
            {"score", mem.score},
            {"snippet", mem.content},
            {"source", mem.metadata.count("source") ? mem.metadata.at("source") : "memory"}
        };
        
        // 严格对齐 OpenClaw: formatCitation + decorateCitations
        if (include_citations && mem.metadata.count("path")) {
            std::string citation = format_citation(path, start_line, end_line);
            item["citation"] = citation;
            
            // 对齐 OpenClaw: 在 snippet 末尾追加 Source:
            std::string decorated_snippet = mem.content;
            if (!decorated_snippet.empty() && decorated_snippet.back() != '\n') {
                decorated_snippet += "\n";
            }
            decorated_snippet += "\nSource: " + citation;
            item["snippet"] = decorated_snippet;
        }
        
        results.push_back(item);
    }
    
    // 4. 获取 Provider 信息
    auto stats = provider_->get_stats();
    
    json response = {
        {"results", results},
        {"provider", provider_->provider_id()},
        {"model", stats ? stats.value().source_counts.begin()->first : "unknown"},  // 简化
        {"citations", "auto"}  // HunterClaw 默认 auto
    };
    
    logger->info("Memory search completed: {} results", results.size());
    
    return core::Ok(response);
}

// ========================================
// MemoryGetTool Implementation
// ========================================

MemoryGetTool::MemoryGetTool(std::shared_ptr<MemoryProvider> provider)
    : provider_(provider) {}

core::Result<json> MemoryGetTool::execute(const json& params) {
    auto logger = core::Logger::get();
    
    // 1. 提取参数
    if (!params.contains("path") || !params["path"].is_string()) {
        return core::Err("Parameter 'path' is required and must be a string");
    }
    
    std::string rel_path = params["path"];
    int from = params.value("from", 0);
    int lines = params.value("lines", -1);  // -1 表示全部
    
    logger->info("Memory get: path='{}' from={} lines={}", rel_path, from, lines);
    
    // 2. 严格对齐 OpenClaw: 调用 manager.readFile
    // OpenClaw 实现: src/memory/builtin-manager.ts::readFile
    // 实际上应该直接读取文件系统,而不是通过 search
    // 但目前 HunterClaw MemoryProvider 没有 readFile 接口
    // 暂时通过 metadata 过滤模拟
    
    SearchOptions options;
    options.limit = 1000;  // 获取所有匹配的 chunks
    options.min_score = 0.0;  // 不过滤分数
    
    // TODO: 实现直接文件读取接口 (对齐 OpenClaw)
    // OpenClaw: manager.readFile({ relPath, from?, lines? })
    // HunterClaw: 需要在 MemoryProvider 中添加 read_file 方法
    
    // 临时方案: 通过 search + path 过滤
    options.filters["path"] = rel_path;
    auto result = provider_->search_memories("", options);
    
    if (!result) {
        logger->error("Memory get failed: {}", result.error());
        return core::Ok(json{
            {"path", rel_path},
            {"text", ""},
            {"disabled", true},
            {"error", result.error()}
        });
    }
    
    auto memories = result.value();
    
    if (memories.empty()) {
        logger->warn("Memory get: file not found - {}", rel_path);
        return core::Ok(json{
            {"path", rel_path},
            {"text", ""},
            {"error", "File not found in memory index"}
        });
    }
    
    // 3. 合并 chunks 并提取指定行 (对齐 OpenClaw)
    std::vector<std::string> all_lines;
    
    // 按 start_line 排序
    std::sort(memories.begin(), memories.end(), [](const Memory& a, const Memory& b) {
        int a_start = a.metadata.count("start_line") ? std::stoi(a.metadata.at("start_line")) : 0;
        int b_start = b.metadata.count("start_line") ? std::stoi(b.metadata.at("start_line")) : 0;
        return a_start < b_start;
    });
    
    // 合并文本 (OpenClaw 会重建完整文件)
    for (const auto& mem : memories) {
        auto content_lines = split_lines(mem.content);
        all_lines.insert(all_lines.end(), content_lines.begin(), content_lines.end());
    }
    
    // 4. 提取指定行范围 (严格对齐 OpenClaw)
    std::vector<std::string> selected_lines;
    
    if (from > 0 || lines > 0) {
        // from 是 1-indexed (对齐 OpenClaw)
        int start_idx = (from > 0) ? (from - 1) : 0;
        int end_idx = (lines > 0) ? (start_idx + lines) : static_cast<int>(all_lines.size());
        
        // 边界检查
        start_idx = std::max(0, start_idx);
        end_idx = std::min(end_idx, static_cast<int>(all_lines.size()));
        
        for (int i = start_idx; i < end_idx; ++i) {
            selected_lines.push_back(all_lines[i]);
        }
    } else {
        // 返回全部行
        selected_lines = all_lines;
    }
    
    // 5. 返回结果 (严格对齐 OpenClaw 格式)
    std::string text = join_lines(selected_lines);
    
    logger->info("Memory get completed: {} lines from {}", selected_lines.size(), rel_path);
    
    return core::Ok(json{
        {"path", rel_path},
        {"text", text}
    });
}

// 辅助函数
std::vector<std::string> MemoryGetTool::split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string MemoryGetTool::join_lines(const std::vector<std::string>& lines) {
    std::ostringstream oss;
    for (size_t i = 0; i < lines.size(); ++i) {
        oss << lines[i];
        if (i < lines.size() - 1) {
            oss << "\n";
        }
    }
    return oss.str();
}

// ========================================
// MemoryToolFactory Implementation
// ========================================

std::vector<std::shared_ptr<Tool>> MemoryToolFactory::create_memory_tools(
    std::shared_ptr<MemoryProvider> provider
) {
    return {
        create_memory_search(provider),
        create_memory_get(provider)
    };
}

std::shared_ptr<Tool> MemoryToolFactory::create_memory_search(
    std::shared_ptr<MemoryProvider> provider
) {
    return std::make_shared<MemorySearchTool>(provider);
}

std::shared_ptr<Tool> MemoryToolFactory::create_memory_get(
    std::shared_ptr<MemoryProvider> provider
) {
    return std::make_shared<MemoryGetTool>(provider);
}

} // namespace tools
} // namespace hunterclaw
