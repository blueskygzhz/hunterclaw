#include "hunterclaw/cli/memory_commands.h"
#include "hunterclaw/cli/json_output.h"
#include "hunterclaw/memory/memory_index_manager.h"
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

    std::string workspace_dir = fs::current_path().string();

    // 对齐 openclaw manager.ts: MemoryIndexManager::get()
    memory::MemoryIndexManager::Params params;
    params.agent_id = "cli";
    params.workspace_dir = workspace_dir;

    auto mgr_result = memory::MemoryIndexManager::get(params);
    if (mgr_result.is_err()) {
        JsonOutput output{false, "error", mgr_result.error};
        output.print();
        return 1;
    }
    auto& mgr = mgr_result.get();

    memory::SearchOptions options;
    options.max_results = max_results;
    options.min_score = min_score;

    auto results = mgr->search(query, options);

    JsonOutput output{true, "success", ""};
    output.data["query"] = query;
    output.data["count"] = results.size();

    std::vector<std::map<std::string, std::string>> items;
    for (const auto& r : results) {
        std::map<std::string, std::string> item;
        item["path"] = r.path;
        item["start_line"] = std::to_string(r.start_line);
        item["end_line"] = std::to_string(r.end_line);
        item["score"] = std::to_string(r.score);
        item["snippet"] = r.snippet.substr(0, 200);
        items.push_back(item);
    }
    output.data["results"] = items;
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

    std::string workspace_dir = fs::current_path().string();

    // 对齐 openclaw manager.ts: MemoryIndexManager::get()
    memory::MemoryIndexManager::Params params;
    params.agent_id = "cli";
    params.workspace_dir = workspace_dir;

    auto mgr_result = memory::MemoryIndexManager::get(params);
    if (mgr_result.is_err()) {
        JsonOutput output{false, "error", mgr_result.error};
        output.print();
        return 1;
    }
    auto& mgr = mgr_result.get();

    // 对齐 openclaw manager.ts: readFile()
    memory::ReadFileParams read_params;
    read_params.rel_path = path;
    if (from_line > 1) read_params.from = from_line;
    if (num_lines > 0) read_params.lines = num_lines;

    auto read_result = mgr->read_file(read_params);

    JsonOutput output{true, "success", ""};
    output.data["path"] = read_result.path;
    output.data["content"] = read_result.text;
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

    std::string workspace_dir = fs::current_path().string();
    std::string memory_dir = workspace_dir + "/.openclaw/memory";

    if (!fs::exists(memory_dir)) {
        JsonOutput output{false, "not_found", "Memory directory not found"};
        output.print();
        return 1;
    }

    std::vector<std::map<std::string, std::string>> files;
    for (const auto& entry : fs::recursive_directory_iterator(memory_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".md") {
            auto rel_path = fs::relative(entry.path(), workspace_dir / ".openclaw");
            auto size = fs::file_size(entry.path());

            std::map<std::string, std::string> file_info;
            file_info["path"] = rel_path.string();
            file_info["size"] = std::to_string(size);
            files.push_back(file_info);
        }
    }

    if (format == "table") {
        std::cout << "Memory Files:\n";
        std::cout << std::string(80, '=') << "\n";
        std::cout << "Path                                    | Size (bytes)\n";
        std::cout << std::string(80, '-') << "\n";
        for (const auto& file : files) {
            std::cout << std::left << std::setw(40) << file.at("path")
                     << "| " << file.at("size") << "\n";
        }
        std::cout << std::string(80, '=') << "\n";
        std::cout << "Total: " << files.size() << " files\n";
    } else {
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
    std::string workspace_dir = fs::current_path().string();

    // 对齐 openclaw manager.ts: MemoryIndexManager::get() + status()
    memory::MemoryIndexManager::Params params;
    params.agent_id = "cli";
    params.workspace_dir = workspace_dir;

    auto mgr_result = memory::MemoryIndexManager::get(params);
    if (mgr_result.is_err()) {
        JsonOutput output{false, "error", mgr_result.error};
        output.print();
        return 1;
    }
    auto& mgr = mgr_result.get();

    auto status = mgr->status();

    JsonOutput output{true, "success", ""};
    output.data["backend"] = status.backend;
    output.data["provider"] = status.provider;
    output.data["files"] = status.files.value_or(0);
    output.data["chunks"] = status.chunks.value_or(0);
    output.data["dirty"] = status.dirty.value_or(false);
    output.data["workspace_dir"] = status.workspace_dir.value_or("");
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

    std::string workspace_dir = fs::current_path().string();
    std::string memory_dir = workspace_dir + "/.openclaw/memory";

    std::cout << (dry_run ? "Dry run: " : "") << "Compacting memory files older than "
             << days << " days...\n";

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

    std::string workspace_dir = fs::current_path().string();

    std::cout << (force ? "Force " : "") << "Reindexing memory embeddings...\n";

    // 对齐 openclaw manager.ts: MemoryIndexManager::get() + sync(force)
    memory::MemoryIndexManager::Params params;
    params.agent_id = "cli";
    params.workspace_dir = workspace_dir;

    auto mgr_result = memory::MemoryIndexManager::get(params);
    if (mgr_result.is_err()) {
        JsonOutput output{false, "error", mgr_result.error};
        output.print();
        return 1;
    }
    auto& mgr = mgr_result.get();

    memory::SyncParams sync_params;
    sync_params.reason = "reindex";
    sync_params.force = force;
    mgr->sync(sync_params);

    JsonOutput output{true, "success", "Reindex completed"};
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
    std::string workspace_dir = fs::current_path().string();

    // 对齐 openclaw manager.ts: MemoryIndexManager::get() + status()
    memory::MemoryIndexManager::Params params;
    params.agent_id = "cli";
    params.workspace_dir = workspace_dir;

    auto mgr_result = memory::MemoryIndexManager::get(params);
    if (mgr_result.is_err()) {
        JsonOutput output{false, "error", mgr_result.error};
        output.print();
        return 1;
    }
    auto& mgr = mgr_result.get();

    auto status = mgr->status();

    JsonOutput output{true, "success", ""};
    output.data["provider"] = status.provider;
    output.data["model"] = status.model.value_or("");
    output.data["backend"] = status.backend;
    if (status.vector.has_value()) {
        output.data["vector_enabled"] = status.vector->enabled;
    }
    if (status.fts.has_value()) {
        output.data["fts_enabled"] = status.fts->enabled;
    }
    output.print();

    return 0;
}

} // namespace cli
} // namespace hunterclaw
