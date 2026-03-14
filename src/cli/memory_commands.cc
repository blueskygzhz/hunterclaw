#include "hunterclaw/cli/memory_commands.h"
#include "hunterclaw/cli/json_output.h"
#include "hunterclaw/memory/memory_manager.h"
#include "hunterclaw/memory/compaction.h"
#include <iostream>
#include <filesystem>

namespace hunterclaw {
namespace cli {

namespace fs = std::filesystem;

// ============================================================================
// MemorySearchCommand
// ============================================================================

std::vector<CommandOption> MemorySearchCommand::get_options() const {
    return {
        {"query", "q", "Search query", true, ""},
        {"max-results", "n", "Maximum number of results", false, "6"},
        {"min-score", "s", "Minimum score threshold", false, "0.35"}
    };
}

int MemorySearchCommand::execute(const CommandArgs& cmd_args) {
    std::string query = cmd_args.get_option_value("query");
    int max_results = std::stoi(cmd_args.get_option_value("max-results", "6"));
    double min_score = std::stod(cmd_args.get_option_value("min-score", "0.35"));
    
    // 获取 workspace 目录 (假设当前目录)
    std::string workspace_dir = fs::current_path().string();
    
    // 创建 MemoryManager
    auto memory_mgr = memory::MemoryManager::create(workspace_dir);
    
    // 执行搜索
    memory::SearchOptions options;
    options.limit = max_results;
    options.min_score = min_score;
    
    auto result = memory_mgr->search_memories(query, options);
    
    if (result.is_err()) {
        JsonOutput output{false, "error", result.error};
        output.print();
        return 1;
    }
    
    JsonOutput output{true, "success", ""};
    output.data["query"] = query;
    output.data["count"] = result.value.size();
    
    // 添加结果
    std::vector<std::map<std::string, std::string>> results;
    for (const auto& memory : result.value) {
        std::map<std::string, std::string> item;
        item["path"] = memory.metadata["path"];
        item["start_line"] = memory.metadata["start_line"];
        item["end_line"] = memory.metadata["end_line"];
        item["score"] = std::to_string(memory.score);
        item["snippet"] = memory.content.substr(0, 200);  // 前 200 字符
        results.push_back(item);
    }
    output.data["results"] = results;
    output.print();
    
    return 0;
}

// ============================================================================
// MemoryGetCommand
// ============================================================================

std::vector<CommandOption> MemoryGetCommand::get_options() const {
    return {
        {"path", "p", "Memory file path", true, ""},
        {"from", "f", "Start line (1-indexed)", false, "1"},
        {"lines", "n", "Number of lines to read", false, "0"}
    };
}

int MemoryGetCommand::execute(const CommandArgs& cmd_args) {
    std::string path = cmd_args.get_option_value("path");
    int from_line = std::stoi(cmd_args.get_option_value("from", "1"));
    int num_lines = std::stoi(cmd_args.get_option_value("lines", "0"));
    
    // 获取 workspace 目录
    std::string workspace_dir = fs::current_path().string();
    std::string full_path = workspace_dir + "/.codebuddy/" + path;
    
    // 检查文件是否存在
    if (!fs::exists(full_path)) {
        JsonOutput output{false, "not_found", "Memory file not found: " + path};
        output.print();
        return 1;
    }
    
    // 读取文件
    std::ifstream file(full_path);
    if (!file.is_open()) {
        JsonOutput output{false, "error", "Failed to open memory file"};
        output.print();
        return 1;
    }
    
    std::vector<std::string> lines;
    std::string line;
    int current_line = 0;
    
    while (std::getline(file, line)) {
        current_line++;
        
        if (current_line < from_line) {
            continue;
        }
        
        if (num_lines > 0 && lines.size() >= static_cast<size_t>(num_lines)) {
            break;
        }
        
        lines.push_back(line);
    }
    
    file.close();
    
    JsonOutput output{true, "success", ""};
    output.data["path"] = path;
    output.data["from_line"] = from_line;
    output.data["lines_read"] = lines.size();
    output.data["content"] = lines;
    output.print();
    
    return 0;
}

// ============================================================================
// MemoryListCommand
// ============================================================================

std::vector<CommandOption> MemoryListCommand::get_options() const {
    return {
        {"format", "f", "Output format (json|table)", false, "json"}
    };
}

int MemoryListCommand::execute(const CommandArgs& cmd_args) {
    std::string format = cmd_args.get_option_value("format");
    
    // 获取 workspace 目录
    std::string workspace_dir = fs::current_path().string();
    std::string memory_dir = workspace_dir + "/.codebuddy/memory";
    
    // 检查目录是否存在
    if (!fs::exists(memory_dir)) {
        JsonOutput output{false, "not_found", "Memory directory not found"};
        output.print();
        return 1;
    }
    
    // 列出所有文件
    std::vector<std::map<std::string, std::string>> files;
    
    for (const auto& entry : fs::recursive_directory_iterator(memory_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".md") {
            auto rel_path = fs::relative(entry.path(), workspace_dir / ".codebuddy");
            auto size = fs::file_size(entry.path());
            auto mtime = fs::last_write_time(entry.path());
            
            std::map<std::string, std::string> file_info;
            file_info["path"] = rel_path.string();
            file_info["size"] = std::to_string(size);
            files.push_back(file_info);
        }
    }
    
    if (format == "table") {
        // 表格输出
        std::cout << "Memory Files:\n";
        std::cout << std::string(80, '=') << "\n";
        std::cout << "Path                                    | Size (bytes)\n";
        std::cout << std::string(80, '-') << "\n";
        
        for (const auto& file : files) {
            std::cout << std::left << std::setw(40) << file["path"] 
                     << "| " << file["size"] << "\n";
        }
        
        std::cout << std::string(80, '=') << "\n";
        std::cout << "Total: " << files.size() << " files\n";
    } else {
        // JSON 输出
        JsonOutput output{true, "success", ""};
        output.data["files"] = files;
        output.data["count"] = files.size();
        output.print();
    }
    
    return 0;
}

// ============================================================================
// MemoryStatsCommand
// ============================================================================

std::vector<CommandOption> MemoryStatsCommand::get_options() const {
    return {};
}

int MemoryStatsCommand::execute(const CommandArgs& cmd_args) {
    // 获取 workspace 目录
    std::string workspace_dir = fs::current_path().string();
    
    // 创建 MemoryManager
    auto memory_mgr = memory::MemoryManager::create(workspace_dir);
    
    // 获取统计信息
    auto stats = memory_mgr->get_stats();
    
    if (!stats) {
        JsonOutput output{false, "error", "Failed to get memory statistics"};
        output.print();
        return 1;
    }
    
    JsonOutput output{true, "success", ""};
    output.data["provider"] = stats->provider;
    output.data["total_embeddings"] = stats->total_embeddings;
    output.data["total_memories"] = stats->total_memories;
    output.data["index_size_bytes"] = stats->index_size_bytes;
    output.data["last_indexed"] = stats->last_indexed_time;
    output.print();
    
    return 0;
}

// ============================================================================
// MemoryCompactCommand
// ============================================================================

std::vector<CommandOption> MemoryCompactCommand::get_options() const {
    return {
        {"days", "d", "Days threshold for compaction", false, "30"},
        {"dry-run", "n", "Show what would be done", false, "false"}
    };
}

int MemoryCompactCommand::execute(const CommandArgs& cmd_args) {
    int days = std::stoi(cmd_args.get_option_value("days", "30"));
    bool dry_run = cmd_args.get_option_value("dry-run") == "true";
    
    // 获取 workspace 目录
    std::string workspace_dir = fs::current_path().string();
    std::string memory_dir = workspace_dir + "/.codebuddy/memory";
    
    std::cout << (dry_run ? "Dry run: " : "") << "Compacting memory files older than " 
             << days << " days...\n";
    
    // 执行压缩
    memory::CompactionOptions options;
    options.days_threshold = days;
    options.dry_run = dry_run;
    
    auto result = memory::compact_memory_files(memory_dir, options);
    
    if (result.is_err()) {
        JsonOutput output{false, "error", result.error};
        output.print();
        return 1;
    }
    
    auto& stats = result.value;
    
    JsonOutput output{true, "success", dry_run ? "Dry run completed" : "Compaction completed"};
    output.data["files_processed"] = stats.files_processed;
    output.data["files_deleted"] = stats.files_deleted;
    output.data["bytes_saved"] = stats.bytes_saved;
    output.print();
    
    return 0;
}

// ============================================================================
// MemoryReindexCommand
// ============================================================================

std::vector<CommandOption> MemoryReindexCommand::get_options() const {
    return {
        {"force", "f", "Force full reindex", false, "false"}
    };
}

int MemoryReindexCommand::execute(const CommandArgs& cmd_args) {
    bool force = cmd_args.get_option_value("force") == "true";
    
    // 获取 workspace 目录
    std::string workspace_dir = fs::current_path().string();
    
    std::cout << (force ? "Force " : "") << "Reindexing memory embeddings...\n";
    
    // 创建 MemoryManager
    auto memory_mgr = memory::MemoryManager::create(workspace_dir);
    
    // 执行重新索引
    auto result = memory_mgr->reindex(force);
    
    if (result.is_err()) {
        JsonOutput output{false, "error", result.error};
        output.print();
        return 1;
    }
    
    JsonOutput output{true, "success", "Reindex completed"};
    output.data["embeddings_created"] = result.value;
    output.print();
    
    return 0;
}

// ============================================================================
// MemoryConfigCommand
// ============================================================================

std::vector<CommandOption> MemoryConfigCommand::get_options() const {
    return {};
}

int MemoryConfigCommand::execute(const CommandArgs& cmd_args) {
    // 获取 workspace 目录
    std::string workspace_dir = fs::current_path().string();
    
    // 创建 MemoryManager
    auto memory_mgr = memory::MemoryManager::create(workspace_dir);
    
    // 获取配置
    auto config = memory_mgr->get_config();
    
    JsonOutput output{true, "success", ""};
    output.data["provider"] = config.provider;
    output.data["embedding_model"] = config.embedding_model;
    output.data["chunk_size"] = config.chunk_size;
    output.data["chunk_overlap"] = config.chunk_overlap;
    output.data["enabled"] = config.enabled;
    output.print();
    
    return 0;
}

} // namespace cli
} // namespace hunterclaw
