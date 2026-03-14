#pragma once

#include "hunterclaw/core/types.h"
// #include <yaml-cpp/yaml.h>  // Commented out - not available
#include <filesystem>
#include <regex>

namespace hunterclaw {
namespace skills {

namespace fs = std::filesystem;

// ============================================================================
// Skill Metadata (parsed from YAML frontmatter)
// ============================================================================

struct SkillInstallSpec {
    std::string id;
    enum class Kind {
        Brew,
        Node,
        Go,
        Uv,
        Download
    } kind;
    
    std::string label;
    std::vector<std::string> bins;
    std::vector<std::string> os;  // darwin, linux, win32
    
    // Package manager specific
    std::string formula;   // Homebrew
    std::string package;   // npm/uv
    std::string module;    // Go
    
    // Download specific
    std::string url;
    std::string archive;
    bool extract = false;
    int strip_components = 0;
    std::string target_dir;
};

struct SkillRequirements {
    std::vector<std::string> bins;        // Required binaries
    std::vector<std::string> any_bins;    // Any of these (OR logic)
    std::vector<std::string> env;         // Required env vars
    std::vector<std::string> config;      // Required config paths
};

struct OpenClawSkillMetadata {
    bool always = false;             // Always include
    std::string skill_key;
    std::string primary_env;
    std::string emoji;
    std::string homepage;
    std::vector<std::string> os;
    
    SkillRequirements requires;
    std::vector<SkillInstallSpec> install;
};

struct SkillFrontmatter {
    std::string name;
    std::string description;
    OpenClawSkillMetadata metadata;
};

// ============================================================================
// Skill Entry
// ============================================================================

enum class SkillSource {
    Bundled,    // Built-in skills (系统内置)
    Managed,    // ~/.openclaw/skills or ~/.hunterclaw/skills (平台管理)
    Personal,   // ~/.agents/skills (租户/用户)
    Plugin,     // ~/.openclaw/extensions/<plugin>/skills (扩展插件)
    Project,    // <workspace>/.agents/skills (项目隐藏)
    Workspace   // <workspace>/skills (项目公共)
};

struct Skill {
    std::string name;
    std::string description;
    std::string content;  // Markdown body (for LLM prompt)
    
    fs::path file_path;
    fs::path base_dir;
    SkillSource source;
    
    SkillFrontmatter frontmatter;
};

struct SkillEntry {
    Skill skill;
    OpenClawSkillMetadata metadata;
    
    // Runtime state
    bool eligible = false;
    std::string ineligible_reason;
};

// ============================================================================
// Skill Eligibility Context
// ============================================================================

struct RemoteSkillEligibility {
    std::vector<std::string> platforms;  // Available platforms (darwin, linux)
    std::function<bool(const std::string&)> has_bin;
    std::function<bool(const std::vector<std::string>&)> has_any_bin;
    std::string note;
};

struct SkillEligibilityContext {
    std::string current_os;
    std::map<std::string, std::string> env_vars;
    std::shared_ptr<core::Config> config;
    
    std::function<bool(const std::string&)> has_bin;
    std::function<bool(const std::string&)> has_env;
    std::function<bool(const std::string&)> is_config_path_truthy;
    
    std::optional<RemoteSkillEligibility> remote;
};

// ============================================================================
// Skill Manager
// ============================================================================

class SkillManager {
public:
    // Factory method
    static std::shared_ptr<SkillManager> create(const core::SkillConfig& config);
    
    explicit SkillManager(std::shared_ptr<core::Config> config);
    ~SkillManager();
    
    // Loading
    core::Result<bool> load_skills(const std::string& workspace_dir);
    core::Result<bool> load_from_roots();  // Load from default roots
    core::Result<bool> reload_skills();
    
    // Querying
    std::vector<SkillEntry> list_skills() const;
    std::vector<SkillEntry> list() const { return list_skills(); }  // Alias for convenience
    std::optional<SkillEntry> get_skill(const std::string& name) const;
    
    // Filtering
    std::vector<SkillEntry> filter_eligible_skills(
        const SkillEligibilityContext& ctx,
        const std::vector<std::string>& skill_filter = {}
    ) const;
    
    // Prompt building (for LLM)
    std::string build_skills_prompt(
        const SkillEligibilityContext& ctx,
        const std::vector<std::string>& skill_filter = {}
    ) const;
    
    // Installation
    core::Result<bool> install_skill(
        const std::string& skill_name,
        const std::optional<std::string>& install_id = std::nullopt
    );
    
    // Security scanning
    struct ScanResult {
        struct Warning {
            std::string severity;  // critical, high, medium, low
            std::string file;
            std::string message;
            size_t line;
        };
        std::vector<Warning> warnings;
        
        bool has_critical() const {
            return std::any_of(warnings.begin(), warnings.end(),
                [](const Warning& w) { return w.severity == "critical"; });
        }
    };
    
    core::Result<ScanResult> scan_skill(const std::string& skill_name);
    
    // Watch for changes
    void start_watch();
    void stop_watch();
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Skill Parser (parse SKILL.md files)
// ============================================================================

class SkillParser {
public:
    // Parse SKILL.md file
    static core::Result<Skill> parse_file(const fs::path& skill_path);
    
    // Parse frontmatter
    static core::Result<SkillFrontmatter> parse_frontmatter(const std::string& content);
    
    // Validate skill structure
    static core::Result<bool> validate_skill(const Skill& skill);
    
private:
    // Split frontmatter and body
    static std::pair<std::string, std::string> split_frontmatter(const std::string& content);
};

// ============================================================================
// Skill Installer
// ============================================================================

class SkillInstaller {
public:
    struct InstallPreferences {
        bool prefer_brew = true;
        std::string node_manager = "pnpm";
    };
    
    explicit SkillInstaller(const InstallPreferences& prefs);
    ~SkillInstaller();
    
    // Install skill dependencies
    core::Result<bool> install(
        const SkillEntry& entry,
        const std::optional<std::string>& install_id = std::nullopt,
        core::Duration timeout = std::chrono::seconds(300)
    );
    
    // Check if dependencies are installed
    bool check_dependencies(const SkillEntry& entry) const;
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Skill Scanner (security checks)
// ============================================================================

class SkillScanner {
public:
    using ScanResult = SkillManager::ScanResult;
    
    // Scan skill for dangerous patterns
    static core::Result<ScanResult> scan(const SkillEntry& entry);
    
private:
    struct DangerousPattern {
        std::string rule_id;
        std::string severity;
        std::string message;
        std::regex pattern;
        std::optional<std::regex> requires_context;
    };
    
    static std::vector<DangerousPattern> get_patterns();
};

} // namespace skills
} // namespace hunterclaw
