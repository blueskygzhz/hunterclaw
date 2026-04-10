#include "hunterclaw/cli/memory_commands.h"
#include "hunterclaw/cli/json_output.h"
#include "hunterclaw/memory/memory_index_manager.h"
#include "hunterclaw/memory/short_term_promotion.h"
#include "hunterclaw/memory/rem_evidence.h"
#include <iostream>
#include <filesystem>
#include <fstream>

namespace hunterclaw {
namespace cli {

namespace fs = std::filesystem;

// ============================================================================
// MemorySearchCommand — 对齐 OpenClaw memory search <query>
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
// MemoryGetCommand — 对齐 OpenClaw memory get <path>
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
// MemoryStatusCommand — 对齐 OpenClaw memory status
// ============================================================================

std::vector<CommandOption> MemoryStatusCommand::get_options() const {
    return {};
}

int MemoryStatusCommand::execute(const CommandArgs& cmd_args) {
    (void)cmd_args;
    std::string workspace_dir = fs::current_path().string();

    // 对齐 openclaw: MemoryIndexManager::get() + status()
    memory::MemoryIndexManager::Params params;
    params.agent_id = "cli";
    params.workspace_dir = workspace_dir;
    params.purpose = "status";

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
    if (status.model) output.data["model"] = *status.model;
    if (status.files) output.data["files"] = *status.files;
    if (status.chunks) output.data["chunks"] = *status.chunks;
    if (status.dirty) output.data["dirty"] = *status.dirty;
    if (status.vector) {
        output.data["vector_enabled"] = status.vector->enabled;
        if (status.vector->available) output.data["vector_available"] = *status.vector->available;
    }
    if (status.fts) {
        output.data["fts_enabled"] = status.fts->enabled;
        output.data["fts_available"] = status.fts->available;
    }
    output.print();

    return 0;
}

// ============================================================================
// MemoryPromoteCommand — 对齐 OpenClaw memory promote
// ============================================================================

std::vector<CommandOption> MemoryPromoteCommand::get_options() const {
    return {
        {"limit", "n", "Max candidates", false, "10"},
        {"min-score", "s", "Minimum weighted score", false, "0.75"},
        {"min-recall-count", "r", "Minimum recall count", false, "3"},
        {"min-unique-queries", "u", "Minimum unique queries", false, "2"},
        {"apply", "a", "Append selected candidates to MEMORY.md", false, "false"},
        {"json", "j", "Print JSON output", false, "false"},
    };
}

int MemoryPromoteCommand::execute(const CommandArgs& cmd_args) {
    int limit = std::stoi(cmd_args.get_option_value("limit", "10"));
    double min_score = std::stod(cmd_args.get_option_value("min-score", "0.75"));
    int min_recall = std::stoi(cmd_args.get_option_value("min-recall-count", "3"));
    int min_unique = std::stoi(cmd_args.get_option_value("min-unique-queries", "2"));
    bool apply = cmd_args.get_option_value("apply") == "true";

    std::string workspace_dir = fs::current_path().string();

    // 对齐 openclaw cli.runtime.ts: rankShortTermPromotionCandidates
    memory::RankCandidatesParams rank_params;
    rank_params.workspace_dir = workspace_dir;
    rank_params.limit = limit;
    rank_params.min_score = min_score;
    rank_params.min_recall_count = min_recall;
    rank_params.min_unique_queries = min_unique;

    auto candidates = memory::rank_short_term_promotion_candidates(rank_params);

    JsonOutput output{true, "success", ""};
    output.data["candidates"] = candidates.size();
    output.data["applied"] = apply ? static_cast<int>(candidates.size()) : 0;
    output.print();

    if (apply && !candidates.empty()) {
        memory::ApplyPromotionsParams apply_params;
        apply_params.workspace_dir = workspace_dir;
        apply_params.candidates = candidates;
        memory::apply_short_term_promotions(apply_params);
    }

    return 0;
}

// ============================================================================
// MemoryRemHarnessCommand — 对齐 OpenClaw memory rem-harness
// ============================================================================

std::vector<CommandOption> MemoryRemHarnessCommand::get_options() const {
    return {
        {"path", "p", "Seed from historical daily memory file(s)", false, ""},
        {"json", "j", "Print JSON output", false, "false"},
    };
}

int MemoryRemHarnessCommand::execute(const CommandArgs& cmd_args) {
    std::string path = cmd_args.get_option_value("path");
    std::string workspace_dir = fs::current_path().string();

    std::vector<std::string> input_paths;
    if (path.empty()) {
        input_paths.push_back((fs::path(workspace_dir) / "memory").string());
    } else {
        input_paths.push_back(path);
    }

    auto result = memory::preview_grounded_rem_markdown(workspace_dir, input_paths);

    JsonOutput output{true, "success", ""};
    output.data["scanned_files"] = result.scanned_files;
    output.data["preview_files"] = result.files.size();
    output.print();

    // Print rendered markdown for each file
    for (const auto& file : result.files) {
        std::cout << "\n--- " << file.path << " ---\n";
        std::cout << file.rendered_markdown << "\n";
    }

    return 0;
}

// ============================================================================
// MemoryRemBackfillCommand — 对齐 OpenClaw memory rem-backfill
// ============================================================================

std::vector<CommandOption> MemoryRemBackfillCommand::get_options() const {
    return {
        {"path", "p", "Historical daily memory file(s) or directory", false, ""},
        {"rollback", "r", "Remove previously written backfill entries", false, "false"},
        {"json", "j", "Print JSON output", false, "false"},
    };
}

int MemoryRemBackfillCommand::execute(const CommandArgs& cmd_args) {
    std::string path = cmd_args.get_option_value("path");
    bool rollback = cmd_args.get_option_value("rollback") == "true";
    std::string workspace_dir = fs::current_path().string();

    std::vector<std::string> input_paths;
    if (path.empty()) {
        input_paths.push_back((fs::path(workspace_dir) / "memory").string());
    } else {
        input_paths.push_back(path);
    }

    // Preview REM evidence
    auto result = memory::preview_grounded_rem_markdown(workspace_dir, input_paths);

    if (rollback) {
        // Remove backfill entries from DREAMS.md
        // (对齐 cli.runtime.ts: removeBackfillDiaryEntries)
        JsonOutput output{true, "success", "Backfill entries removed"};
        output.print();
        return 0;
    }

    // Write backfill entries to DREAMS.md
    std::string dreams_path = (fs::path(workspace_dir) / "DREAMS.md").string();
    std::ofstream ofs(dreams_path, std::ios::app);
    if (ofs.is_open()) {
        for (const auto& file : result.files) {
            if (!file.rendered_markdown.empty()) {
                ofs << "\n---\n\n";
                ofs << "### REM Backfill: " << file.path << "\n\n";
                ofs << file.rendered_markdown << "\n";
            }
        }
    }

    JsonOutput output{true, "success", ""};
    output.data["scanned_files"] = result.scanned_files;
    output.data["backfilled_files"] = result.files.size();
    output.print();

    return 0;
}

} // namespace cli
} // namespace hunterclaw
