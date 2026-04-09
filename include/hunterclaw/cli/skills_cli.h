#pragma once

// ============================================================================
// skills_cli.h
// 🎯 严格对齐 OpenClaw: src/cli/skills-cli.ts + skills-cli.format.ts
//
// Skills CLI 命令接口: list / info / check
// ============================================================================

#include "hunterclaw/core/types.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace cli {

// ============================================================================
// Skill Status Report (对齐 OpenClaw: SkillStatusReport + SkillStatusEntry)
// ============================================================================

struct SkillStatusEntry {
    std::string name;
    std::string skill_key;
    std::string description;
    std::string emoji;
    std::string source;          // "openclaw-bundled", "openclaw-managed", etc.
    std::string file_path;
    std::string homepage;
    std::string primary_env;
    bool bundled = false;
    bool eligible = false;
    bool disabled = false;
    bool blocked_by_allowlist = false;

    // Requirements (对齐 OpenClaw: requirements)
    std::vector<std::string> required_bins;
    std::vector<std::string> required_any_bins;
    std::vector<std::string> required_env;
    std::vector<std::string> required_config;
    std::vector<std::string> required_os;

    // Missing (对齐 OpenClaw: missing)
    std::vector<std::string> missing_bins;
    std::vector<std::string> missing_any_bins;
    std::vector<std::string> missing_env;
    std::vector<std::string> missing_config;
    std::vector<std::string> missing_os;

    // Install options
    struct InstallOption {
        std::string id;
        std::string kind;   // "brew", "node", "go", "uv", "download"
        std::string label;
    };
    std::vector<InstallOption> install;
};

struct SkillStatusReport {
    std::string workspace_dir;
    std::string managed_skills_dir;
    std::vector<SkillStatusEntry> skills;
};

// ============================================================================
// Options (对齐 OpenClaw: SkillsListOptions / SkillInfoOptions / SkillsCheckOptions)
// ============================================================================

struct SkillsListOptions {
    bool json = false;
    bool eligible_only = false;
    bool verbose = false;
};

struct SkillInfoOptions {
    bool json = false;
};

struct SkillsCheckOptions {
    bool json = false;
};

// ============================================================================
// Format functions (对齐 OpenClaw 3 个导出函数)
// ============================================================================

std::string format_skills_list(const SkillStatusReport& report,
                                const SkillsListOptions& opts);

std::string format_skill_info(const SkillStatusReport& report,
                               const std::string& skill_name,
                               const SkillInfoOptions& opts);

std::string format_skills_check(const SkillStatusReport& report,
                                 const SkillsCheckOptions& opts);

// ============================================================================
// CLI Registration (对齐 OpenClaw: registerSkillsCli)
// ============================================================================

// Forward declarations for CLI framework
struct CliArgs {
    std::string subcommand;
    std::vector<std::string> positional;
    std::string workspace_dir;
    std::shared_ptr<core::Config> config;
    bool has_flag(const std::string& name) const;
};

class CliProgram {
public:
    void register_command(const std::string& name,
                           const std::string& description,
                           std::function<int(const CliArgs&)> handler);
};

// 🎯 对齐 OpenClaw: registerSkillsCli(program)
void register_skills_cli(CliProgram& program);

// Helper: build status report (used by CLI and Gateway)
SkillStatusReport build_skills_status_report(const std::string& workspace_dir,
                                              std::shared_ptr<core::Config> config);

}  // namespace cli
}  // namespace hunterclaw
