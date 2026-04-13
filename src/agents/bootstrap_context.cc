// bootstrap_context.cc
// Aligned with openclaw:
//   src/agents/bootstrap-files.ts     (resolveBootstrapFilesForRun / resolveBootstrapContextForRun)
//   src/agents/bootstrap-budget.ts    (analyzeBootstrapBudget / buildBootstrapPromptWarning)
//   src/agents/workspace.ts           (loadWorkspaceBootstrapFiles / ensureAgentWorkspace)
//
// Provides:
//   - WorkspaceBootstrapFile
//   - BootstrapContextMode: Full / Lightweight
//   - load_workspace_bootstrap_files
//   - resolve_bootstrap_files_for_run
//   - resolve_bootstrap_context_for_run
//   - analyze_bootstrap_budget

#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <set>

namespace hunterclaw {
namespace agents {

namespace {

auto log() {
    static auto logger = spdlog::get("bootstrap_context");
    if (!logger) logger = spdlog::stdout_logger_mt("bootstrap_context");
    return logger;
}

// 🎯 对齐 OpenClaw: MAX_WORKSPACE_BOOTSTRAP_FILE_BYTES = 2MB
static constexpr size_t MAX_WORKSPACE_BOOTSTRAP_FILE_BYTES = 2 * 1024 * 1024;

// 🎯 对齐 OpenClaw: default bootstrap filenames
static const std::vector<std::string> DEFAULT_BOOTSTRAP_FILENAMES = {
    "AGENTS.md",
    "SOUL.md",
    "IDENTITY.md",
    "USER.md",
    "TOOLS.md",
    "BOOTSTRAP.md",
    "MEMORY.md",
    "HEARTBEAT.md",
};

// Alternative casing
static const std::vector<std::string> ALT_BOOTSTRAP_FILENAMES = {
    "memory.md",
};

// 🎯 对齐 OpenClaw: DEFAULT_BOOTSTRAP_NEAR_LIMIT_RATIO = 0.85
static constexpr double DEFAULT_BOOTSTRAP_NEAR_LIMIT_RATIO = 0.85;
// 🎯 对齐 OpenClaw: default max chars per file = 100K
static constexpr size_t DEFAULT_BOOTSTRAP_MAX_CHARS = 100000;
// 🎯 对齐 OpenClaw: default total max chars = 500K
static constexpr size_t DEFAULT_BOOTSTRAP_TOTAL_MAX_CHARS = 500000;

} // anonymous namespace

// ============================================================================
// BootstrapContextMode — 对齐 OpenClaw bootstrap-files.ts
// ============================================================================

enum class BootstrapContextMode {
    Full,          // All bootstrap files
    Lightweight    // Reduced (heartbeat-only for heartbeat runs, empty otherwise)
};

// ============================================================================
// WorkspaceBootstrapFile — 对齐 OpenClaw workspace.ts
// ============================================================================

struct WorkspaceBootstrapFile {
    std::string name;       // Display name (e.g., "AGENTS.md")
    std::string path;       // Absolute file path
    std::string content;    // File content (raw)
    bool missing = false;   // File doesn't exist
};

// ============================================================================
// BootstrapBudgetAnalysis — 对齐 OpenClaw bootstrap-budget.ts
// ============================================================================

struct BootstrapInjectionStat {
    std::string name;
    std::string path;
    bool missing = false;
    size_t raw_chars = 0;
    size_t injected_chars = 0;
    bool truncated = false;
    bool near_limit = false;
};

struct BootstrapBudgetAnalysis {
    std::vector<BootstrapInjectionStat> files;
    size_t total_raw_chars = 0;
    size_t total_injected_chars = 0;
    size_t total_truncated_chars = 0;
    bool has_truncation = false;
    bool total_near_limit = false;
    size_t max_chars_per_file = DEFAULT_BOOTSTRAP_MAX_CHARS;
    size_t total_max_chars = DEFAULT_BOOTSTRAP_TOTAL_MAX_CHARS;
};

// ============================================================================
// 🎯 对齐 OpenClaw: stripFrontMatter(content)
// ============================================================================

static std::string strip_front_matter(const std::string& content) {
    if (content.size() < 3 || content.substr(0, 3) != "---") {
        return content;
    }
    auto end_idx = content.find("\n---", 3);
    if (end_idx == std::string::npos) return content;
    size_t start = end_idx + 4;
    std::string result = content.substr(start);
    auto first = result.find_first_not_of("\n\r");
    return (first != std::string::npos) ? result.substr(first) : "";
}

// ============================================================================
// 🎯 对齐 OpenClaw: loadWorkspaceBootstrapFiles(workspaceDir)
// ============================================================================

std::vector<WorkspaceBootstrapFile> load_workspace_bootstrap_files(
    const std::string& workspace_dir
) {
    std::vector<WorkspaceBootstrapFile> files;
    std::set<std::string> seen;
    namespace fs = std::filesystem;

    auto try_load = [&](const std::string& filename) {
        std::string lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (seen.count(lower)) return;

        std::string file_path = workspace_dir + "/" + filename;
        WorkspaceBootstrapFile file;
        file.name = filename;
        file.path = file_path;

        try {
            if (!fs::exists(file_path)) {
                file.missing = true;
            } else {
                auto file_size = fs::file_size(file_path);
                if (file_size > MAX_WORKSPACE_BOOTSTRAP_FILE_BYTES) {
                    log()->warn("Bootstrap file too large, skipping: {} ({}KB)",
                        filename, file_size / 1024);
                    return;
                }
                // 🎯 对齐 OpenClaw: openBoundaryFile (symlink check + size guard)
                auto status = fs::symlink_status(file_path);
                if (fs::is_symlink(status)) {
                    log()->warn("Bootstrap file is symlink, skipping: {}", filename);
                    return;
                }

                std::ifstream ifs(file_path);
                if (ifs.good()) {
                    std::string content((std::istreambuf_iterator<char>(ifs)),
                                       std::istreambuf_iterator<char>());
                    file.content = strip_front_matter(content);
                } else {
                    file.missing = true;
                }
            }
        } catch (const std::exception& e) {
            log()->debug("Failed to load bootstrap file {}: {}", filename, e.what());
            file.missing = true;
        }

        seen.insert(lower);
        if (!file.missing) {
            files.push_back(file);
        }
    };

    for (const auto& name : DEFAULT_BOOTSTRAP_FILENAMES) {
        try_load(name);
    }
    for (const auto& name : ALT_BOOTSTRAP_FILENAMES) {
        try_load(name);
    }

    return files;
}

// ============================================================================
// 🎯 对齐 OpenClaw: filterBootstrapFilesForSession(files, sessionKey)
// ============================================================================

std::vector<WorkspaceBootstrapFile> filter_bootstrap_files_for_session(
    const std::vector<WorkspaceBootstrapFile>& files,
    const std::optional<std::string>& session_key
) {
    // 🎯 对齐 OpenClaw: subagent sessions and cron sessions get filtered files
    if (session_key.has_value()) {
        bool is_subagent = session_key->find("sub:") != std::string::npos;
        bool is_cron = session_key->find("cron:") == 0 ||
                       session_key->find("cron_") == 0;

        if (is_subagent || is_cron) {
            // Subagents/cron get minimal bootstrap (only AGENTS.md and TOOLS.md)
            std::vector<WorkspaceBootstrapFile> filtered;
            for (const auto& f : files) {
                std::string lower = f.name;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower == "agents.md" || lower == "tools.md") {
                    filtered.push_back(f);
                }
            }
            return filtered;
        }
    }
    return files;
}

// ============================================================================
// 🎯 对齐 OpenClaw: resolveBootstrapContextForRun
// ============================================================================

struct BootstrapContext {
    std::vector<WorkspaceBootstrapFile> bootstrap_files;
    std::vector<WorkspaceBootstrapFile> context_files; // After truncation
};

BootstrapContext resolve_bootstrap_context_for_run(
    const std::string& workspace_dir,
    const std::optional<std::string>& session_key,
    BootstrapContextMode mode,
    size_t max_chars_per_file,
    size_t total_max_chars
) {
    BootstrapContext result;

    // Load files
    auto raw_files = load_workspace_bootstrap_files(workspace_dir);

    // Filter for session type
    auto filtered = filter_bootstrap_files_for_session(raw_files, session_key);

    // Apply context mode filter
    if (mode == BootstrapContextMode::Lightweight) {
        // Only keep heartbeat for heartbeat runs
        std::vector<WorkspaceBootstrapFile> lightweight;
        for (const auto& f : filtered) {
            std::string lower = f.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == "heartbeat.md") {
                lightweight.push_back(f);
            }
        }
        filtered = lightweight;
    }

    result.bootstrap_files = filtered;

    // 🎯 对齐 OpenClaw: buildBootstrapContextFiles — truncation
    size_t total_chars = 0;
    for (auto& file : filtered) {
        size_t available = total_max_chars > total_chars ?
            (total_max_chars - total_chars) : 0;
        size_t limit = std::min(max_chars_per_file, available);

        if (file.content.size() > limit && limit > 0) {
            file.content = file.content.substr(0, limit) +
                "\n\n[... content truncated at " + std::to_string(limit / 1024) +
                "KB bootstrap limit]";
        }

        total_chars += file.content.size();
        result.context_files.push_back(file);
    }

    log()->debug("resolve_bootstrap_context_for_run: {} files, {} total chars",
        result.context_files.size(), total_chars);

    return result;
}

// ============================================================================
// 🎯 对齐 OpenClaw: analyzeBootstrapBudget
// ============================================================================

BootstrapBudgetAnalysis analyze_bootstrap_budget(
    const std::vector<WorkspaceBootstrapFile>& raw_files,
    const std::vector<WorkspaceBootstrapFile>& injected_files,
    size_t max_chars_per_file,
    size_t total_max_chars
) {
    BootstrapBudgetAnalysis analysis;
    analysis.max_chars_per_file = max_chars_per_file;
    analysis.total_max_chars = total_max_chars;

    for (size_t i = 0; i < raw_files.size(); ++i) {
        BootstrapInjectionStat stat;
        stat.name = raw_files[i].name;
        stat.path = raw_files[i].path;
        stat.missing = raw_files[i].missing;
        stat.raw_chars = raw_files[i].content.size();

        if (i < injected_files.size()) {
            stat.injected_chars = injected_files[i].content.size();
        }

        stat.truncated = stat.injected_chars < stat.raw_chars;
        stat.near_limit = stat.injected_chars >=
            static_cast<size_t>(max_chars_per_file * DEFAULT_BOOTSTRAP_NEAR_LIMIT_RATIO);

        if (stat.truncated) analysis.has_truncation = true;

        analysis.total_raw_chars += stat.raw_chars;
        analysis.total_injected_chars += stat.injected_chars;
        if (stat.truncated) {
            analysis.total_truncated_chars += (stat.raw_chars - stat.injected_chars);
        }

        analysis.files.push_back(stat);
    }

    analysis.total_near_limit = analysis.total_injected_chars >=
        static_cast<size_t>(total_max_chars * DEFAULT_BOOTSTRAP_NEAR_LIMIT_RATIO);

    return analysis;
}

} // namespace agents
} // namespace hunterclaw
