// system_prompt_builder.cc
// Aligned with openclaw:
//   src/agents/system-prompt.ts              (buildSystemPrompt)
//   src/agents/system-prompt-params.ts       (buildSystemPromptParams)
//   src/agents/system-prompt-override.ts     (resolveSystemPromptOverride)
//   src/agents/system-prompt.types.ts        (PromptMode = "full" | "minimal" | "none")
//
// Provides:
//   - PromptMode: Full / Minimal / None
//   - ContextFileEntry: bootstrap context files
//   - SystemPromptRuntimeParams: OS, shell, model, timezone
//   - build_system_prompt: main entry
//   - build_system_prompt_params
//   - resolve_system_prompt_override
//   - sort_context_files_for_prompt

#include "hunterclaw/agents/types.h"
#include "hunterclaw/core/types.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <algorithm>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>
#include <sys/utsname.h>

namespace hunterclaw {
namespace agents {

namespace {

auto log() {
    static auto logger = spdlog::get("system_prompt_builder");
    if (!logger) logger = spdlog::stdout_logger_mt("system_prompt_builder");
    return logger;
}

// 🎯 对齐 OpenClaw: CONTEXT_FILE_ORDER
static const std::map<std::string, int> CONTEXT_FILE_ORDER = {
    {"agents.md", 10},
    {"soul.md", 20},
    {"identity.md", 30},
    {"user.md", 40},
    {"tools.md", 50},
    {"bootstrap.md", 60},
    {"memory.md", 70},
};

// 🎯 对齐 OpenClaw: SYSTEM_PROMPT_CACHE_BOUNDARY
static const std::string SYSTEM_PROMPT_CACHE_BOUNDARY =
    "<!-- SYSTEM_PROMPT_CACHE_BOUNDARY -->";

} // anonymous namespace

// ============================================================================
// PromptMode — 对齐 OpenClaw system-prompt.types.ts
// ============================================================================

enum class PromptMode {
    Full,     // All sections (default, for main agent)
    Minimal,  // Reduced sections (for subagents)
    None      // Just basic identity line
};

// ============================================================================
// ContextFileEntry — 对齐 OpenClaw EmbeddedContextFile
// ============================================================================

struct ContextFileEntry {
    std::string name;      // Display name (e.g., "AGENTS.md")
    std::string path;      // File path
    std::string content;   // File content (after frontmatter stripping)
    bool is_dynamic = false; // Dynamic context (e.g., HEARTBEAT.md)
};

// ============================================================================
// SystemPromptRuntimeParams — 对齐 OpenClaw RuntimeInfoInput
// ============================================================================

struct SystemPromptRuntimeParams {
    std::string agent_id;
    std::string host;
    std::string os;
    std::string arch;
    std::string shell;
    std::string model;
    std::string default_model;
    std::string channel;
    std::string user_timezone;
    std::string user_time;
    std::optional<std::string> repo_root;
    std::vector<std::string> capabilities;
};

// ============================================================================
// 🎯 对齐 OpenClaw: sort_context_files_for_prompt
// ============================================================================

std::vector<ContextFileEntry> sort_context_files_for_prompt(
    std::vector<ContextFileEntry> files
) {
    std::sort(files.begin(), files.end(), [](const ContextFileEntry& a, const ContextFileEntry& b) {
        // Normalize basename to lowercase
        auto base = [](const std::string& name) -> std::string {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            return lower;
        };

        std::string a_base = base(a.name);
        std::string b_base = base(b.name);

        int a_order = (CONTEXT_FILE_ORDER.count(a_base)) ? CONTEXT_FILE_ORDER.at(a_base) : 999;
        int b_order = (CONTEXT_FILE_ORDER.count(b_base)) ? CONTEXT_FILE_ORDER.at(b_base) : 999;

        if (a_order != b_order) return a_order < b_order;
        if (a_base != b_base) return a_base < b_base;
        return a.path < b.path;
    });
    return files;
}

// ============================================================================
// 🎯 对齐 OpenClaw: stripFrontMatter(content)
// ============================================================================

std::string strip_front_matter(const std::string& content) {
    if (content.substr(0, 3) != "---") {
        return content;
    }
    auto end_index = content.find("\n---", 3);
    if (end_index == std::string::npos) {
        return content;
    }
    size_t start = end_index + 4; // length of "\n---"
    std::string result = content.substr(start);
    // trim leading newlines
    auto first = result.find_first_not_of("\n\r");
    if (first != std::string::npos) {
        result = result.substr(first);
    }
    return result;
}

// ============================================================================
// 🎯 对齐 OpenClaw: buildSystemPromptParams
// ============================================================================

SystemPromptRuntimeParams build_system_prompt_params(
    const core::Config* config,
    const std::string& agent_id,
    const std::string& model_name,
    const std::optional<std::string>& workspace_dir
) {
    SystemPromptRuntimeParams params;
    params.agent_id = agent_id;
    params.model = model_name;

    // OS info
    struct utsname uts;
    if (uname(&uts) == 0) {
        params.os = uts.sysname;
        params.arch = uts.machine;
        params.host = uts.nodename;
    } else {
        params.os = "Linux";
        params.arch = "x86_64";
        params.host = "unknown";
    }

    // Shell
    const char* shell_env = std::getenv("SHELL");
    params.shell = shell_env ? shell_env : "/bin/bash";

    // Time
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    localtime_r(&time_t_now, &tm_buf);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", &tm_buf);
    params.user_time = time_str;
    params.user_timezone = tm_buf.tm_zone ? tm_buf.tm_zone : "UTC";

    // Repo root
    if (workspace_dir.has_value()) {
        params.repo_root = *workspace_dir;
    }

    return params;
}

// ============================================================================
// 🎯 对齐 OpenClaw: resolveSystemPromptOverride(config, agentId)
// ============================================================================

std::optional<std::string> resolve_system_prompt_override(
    const core::Config* config,
    const std::string& agent_id
) {
    if (!config) return std::nullopt;

    // Check agent-specific override first
    auto cfg_result = resolve_agent_config(*config, agent_id);
    if (cfg_result.is_ok()) {
        // Check extra.systemPromptOverride
        const auto& extra = cfg_result.get().extra;
        auto it = extra.find("systemPromptOverride");
        if (it != extra.end() && it->second.is_string()) {
            std::string override_val = it->second.as_string();
            auto start = override_val.find_first_not_of(" \t\n\r");
            if (start != std::string::npos) {
                return override_val.substr(start);
            }
        }
    }

    return std::nullopt;
}

// ============================================================================
// 🎯 对齐 OpenClaw: buildSystemPrompt — 系统提示词构建核心
// ============================================================================

std::string build_system_prompt(
    const SystemPromptRuntimeParams& params,
    const std::vector<ContextFileEntry>& context_files,
    PromptMode mode,
    const std::optional<std::string>& system_prompt_override,
    bool has_memory_tools,
    bool has_heartbeat
) {
    // 🎯 对齐 OpenClaw: "none" mode — just basic identity
    if (mode == PromptMode::None) {
        return "You are a helpful AI assistant.";
    }

    std::ostringstream ss;

    // ── Identity ──────────────────────────────────────────────────────────
    // 🎯 对齐 OpenClaw: identity line at the top
    if (system_prompt_override.has_value()) {
        ss << *system_prompt_override << "\n\n";
    } else {
        ss << "You are HunterClaw, an AI assistant that helps with coding, analysis, "
              "and creative tasks.\n\n";
    }

    // ── Project Context (bootstrap files) ─────────────────────────────────
    // 🎯 对齐 OpenClaw: buildProjectContextSection
    auto sorted_files = sort_context_files_for_prompt(
        std::vector<ContextFileEntry>(context_files.begin(), context_files.end())
    );

    // Static context files
    std::vector<ContextFileEntry> static_files;
    std::vector<ContextFileEntry> dynamic_files;
    for (const auto& f : sorted_files) {
        if (f.is_dynamic) {
            dynamic_files.push_back(f);
        } else {
            static_files.push_back(f);
        }
    }

    if (!static_files.empty()) {
        ss << "# Project Context\n\n";
        for (const auto& file : static_files) {
            ss << "## " << file.name << "\n";
            ss << file.content << "\n\n";
        }
    }

    // 🎯 对齐 OpenClaw: SYSTEM_PROMPT_CACHE_BOUNDARY
    ss << SYSTEM_PROMPT_CACHE_BOUNDARY << "\n\n";

    // Dynamic context files (after cache boundary)
    if (!dynamic_files.empty()) {
        ss << "# Dynamic Context\n\n";
        ss << "Below is live context that may change between turns. "
              "Re-read or refresh as needed.\n\n";
        for (const auto& file : dynamic_files) {
            ss << "## " << file.name << "\n";
            ss << file.content << "\n\n";
        }
    }

    // ── Full mode sections ────────────────────────────────────────────────
    if (mode == PromptMode::Full) {
        // Tooling section
        ss << "# Tooling\n\n";
        ss << "You have access to tools for reading/writing files, executing commands, "
              "searching the web, and managing tasks. Use them proactively.\n\n";

        // Workspace section
        ss << "# Workspace\n\n";
        if (params.repo_root.has_value()) {
            ss << "Working directory: " << *params.repo_root << "\n\n";
        }

        // 🎯 对齐 OpenClaw: Memory Recall section
        if (has_memory_tools) {
            ss << "## Memory Recall\n";
            ss << "Before answering anything about prior work, decisions, dates, people, "
                  "preferences, or todos: run memory_search on MEMORY.md + memory/*.md + "
                  "indexed session transcripts; then use memory_get to pull only the needed "
                  "lines. If low confidence after search, say you checked.\n\n"
                  "Citations: include Source: <path#line> when it helps the user verify "
                  "memory snippets.\n\n";
        }

        // Heartbeat guidance
        if (has_heartbeat) {
            ss << "## Heartbeat\n";
            ss << "You support a heartbeat mode for periodic proactive actions. "
                  "Read HEARTBEAT.md if it exists and follow its instructions. "
                  "Do not infer or repeat old tasks from prior chats. "
                  "If nothing needs attention, reply HEARTBEAT_OK.\n\n";
        }
    }

    // ── Runtime info ──────────────────────────────────────────────────────
    // 🎯 对齐 OpenClaw: Runtime info block
    ss << "# Runtime\n\n";
    ss << "- OS: " << params.os << " " << params.arch << "\n";
    ss << "- Shell: " << params.shell << "\n";
    ss << "- Model: " << params.model << "\n";
    if (!params.agent_id.empty() && params.agent_id != "main") {
        ss << "- Agent: " << params.agent_id << "\n";
    }
    ss << "- Time: " << params.user_time << " (" << params.user_timezone << ")\n";
    if (!params.capabilities.empty()) {
        ss << "- Capabilities: ";
        for (size_t i = 0; i < params.capabilities.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << params.capabilities[i];
        }
        ss << "\n";
    }
    ss << "\n";

    return ss.str();
}

} // namespace agents
} // namespace hunterclaw
