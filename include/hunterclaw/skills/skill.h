#pragma once

#include "hunterclaw/core/types.h"
// #include <yaml-cpp/yaml.h>  // Commented out - not available
#include <filesystem>
#include <regex>
#include <set>

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

// ✅ 便捷别名，对齐 manager.cc 和 skills.cc 中的使用
using SkillInstallKind = SkillInstallSpec::Kind;

struct SkillRequirements {
    std::vector<std::string> bins;        // Required binaries
    std::vector<std::string> any_bins;    // Any of these (OR logic)
    std::vector<std::string> env;         // Required env vars
    std::vector<std::string> config;      // Required config paths
    std::vector<std::string> os;          // Supported OS list (empty = all)
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

// ✅ 对齐 OpenClaw types.ts: SkillInvocationPolicy
// Controls whether skill can be model-invoked and/or user-slash-command invoked
struct SkillInvocationPolicy {
    bool user_invocable = true;             // frontmatter: user-invocable (default: true)
    bool disable_model_invocation = false;  // frontmatter: disable-model-invocation (default: false)
};

// ✅ 对齐 OpenClaw types.ts: ParsedSkillFrontmatter (raw key-value map)
using ParsedSkillFrontmatter = std::unordered_map<std::string, std::string>;

struct SkillEntry {
    Skill skill;

    // ✅ 对齐 OpenClaw SkillEntry:
    // { skill, frontmatter, metadata?, invocation? }
    ParsedSkillFrontmatter frontmatter;          // Raw frontmatter key-value pairs
    OpenClawSkillMetadata metadata;              // Resolved openclaw metadata
    std::optional<SkillInvocationPolicy> invocation; // Resolved invocation policy
    
    // Runtime state (HunterClaw extension)
    bool eligible = false;
    std::string ineligible_reason;
};

// ✅ 对齐 OpenClaw types.ts: SkillSnapshot
// Snapshot of eligible skills for injection into agent system prompt
struct SkillSnapshotEntry {
    std::string name;
    std::string primary_env;             // from metadata.primary_env
    std::vector<std::string> required_env; // from metadata.requires.env
};

struct SkillSnapshot {
    std::string prompt;                          // formatSkillsForPrompt() output (XML block)
    std::vector<SkillSnapshotEntry> skills;      // eligible skill metadata list
    std::optional<std::vector<std::string>> skill_filter; // agent-level filter
    std::vector<Skill> resolved_skills;          // actual Skill objects in prompt
    std::optional<int64_t> version;              // snapshot version for dirty detection
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

// ✅ Aligned with OpenClaw types.ts: SkillCommandDispatch
struct SkillCommandDispatch {
    std::string kind;       // "tool"
    std::string tool_name;  // e.g. "bash", "read"
    std::string arg_mode;   // "raw"
};

// ✅ Aligned with OpenClaw types.ts: SkillCommandSpec
// Generated by buildWorkspaceSkillCommandSpecs for slash command registration
struct SkillCommandSpec {
    std::string name;        // Sanitized command name (≤32 chars, [a-z0-9_])
    std::string skill_name;  // Original skill.name
    std::string description; // ≤100 chars (Discord limit)
    std::optional<SkillCommandDispatch> dispatch; // Optional tool dispatch
};

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
    // ✅ Aligned with OpenClaw filterWorkspaceSkillEntries
    std::vector<SkillEntry> filter_eligible_skills(
        const SkillEligibilityContext& ctx,
        const std::vector<std::string>& skill_filter = {}
    ) const;

    // ✅ Aligned with OpenClaw filterWorkspaceSkillEntries
    std::vector<SkillEntry> filter_workspace_skill_entries(
        const std::vector<SkillEntry>& entries,
        const SkillEligibilityContext& ctx
    ) const;
    
    // Prompt building (for LLM)
    // ✅ Aligned with OpenClaw buildWorkspaceSkillsPrompt
    std::string build_skills_prompt(
        const SkillEligibilityContext& ctx,
        const std::vector<std::string>& skill_filter = {}
    ) const;

    // ✅ Aligned with OpenClaw buildWorkspaceSkillSnapshot
    // Builds a SkillSnapshot with versioning for dirty detection.
    // Used by ensureSkillSnapshot() before each chat turn.
    SkillSnapshot build_workspace_skill_snapshot(
        const SkillEligibilityContext& ctx,
        const std::vector<std::string>& skill_filter = {},
        std::optional<int64_t> snapshot_version = std::nullopt
    ) const;

    // ✅ Aligned with OpenClaw resolveSkillsPromptForRun
    // Priority: snapshot.prompt → entries (real-time build) → ""
    std::string resolve_skills_prompt_for_run(
        const std::optional<SkillSnapshot>& snapshot,
        const std::optional<std::vector<SkillEntry>>& entries = std::nullopt
    ) const;

    // ✅ Aligned with OpenClaw applySkillEnvOverrides
    // Inject skill apiKey/env into process environment.
    // Returns a restore function that reverts the env changes.
    std::function<void()> apply_skill_env_overrides(
        const std::vector<SkillEntry>& skills
    );

    // ✅ Aligned with OpenClaw applySkillEnvOverridesFromSnapshot
    std::function<void()> apply_skill_env_overrides_from_snapshot(
        const SkillSnapshot& snapshot
    );

    // ✅ Aligned with OpenClaw compactSkillPaths
    // Replace HOME prefix with "~/" to reduce LLM token usage.
    static std::vector<Skill> compact_skill_paths(const std::vector<Skill>& skills);
    
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
    
    // Watch for changes (aligned with OpenClaw ensureSkillsWatcher)
    void start_watch();
    void stop_watch();

    // ✅ Aligned with OpenClaw bumpSkillsSnapshotVersion / getSkillsSnapshotVersion
    // Version counter bumped when SKILL.md changes detected
    int64_t get_snapshot_version() const;
    int64_t bump_snapshot_version();  // Returns new version

    // ✅ Aligned with OpenClaw buildWorkspaceSkillCommandSpecs (workspace.ts:775-881)
    // Generates slash command specs from eligible, user-invocable skills.
    // Reads frontmatter: command-dispatch, command-tool, command-arg-mode
    std::vector<SkillCommandSpec> build_workspace_skill_command_specs(
        const SkillEligibilityContext& ctx,
        const std::vector<std::string>& skill_filter = {},
        const std::optional<std::set<std::string>>& reserved_names = std::nullopt
    ) const;

    // ✅ Aligned with OpenClaw syncSkillsToWorkspace (workspace.ts:710-766)
    // Copies all skills from source workspace to target workspace (sandbox/container).
    // Serialized to prevent race conditions. source==target is a no-op.
    struct SyncSkillsParams {
        std::string source_workspace_dir;
        std::string target_workspace_dir;
    };
    core::Result<bool> sync_skills_to_workspace(const SyncSkillsParams& params);
    
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

    // ✅ Aligned with OpenClaw parseFrontmatter (frontmatter.ts)
    // Returns raw key-value map from YAML frontmatter block
    static ParsedSkillFrontmatter parse_raw_frontmatter(const std::string& content);

    // ✅ Aligned with OpenClaw resolveOpenClawMetadata (frontmatter.ts)
    // Extracts OpenClawSkillMetadata from parsed frontmatter
    static std::optional<OpenClawSkillMetadata> resolve_openclaw_metadata(
        const ParsedSkillFrontmatter& frontmatter);

    // ✅ Aligned with OpenClaw resolveSkillInvocationPolicy (frontmatter.ts)
    // Reads: user-invocable (default: true), disable-model-invocation (default: false)
    static SkillInvocationPolicy resolve_invocation_policy(
        const ParsedSkillFrontmatter& frontmatter);
    
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

// ============================================================================
// ClawHub Registry Types (对齐 OC skills-clawhub.ts)
// ============================================================================

struct ClawHubSkillOrigin {
    int version = 1;
    std::string registry;          // base URL
    std::string slug;
    std::string installed_version;
    int64_t installed_at = 0;      // epoch ms
};

struct ClawHubSkillsLockfile {
    int version = 1;
    struct LockEntry {
        std::string version;
        int64_t installed_at = 0;
    };
    std::map<std::string, LockEntry> skills;
};

struct ClawHubSkillSearchResult {
    double score = 0;
    std::string slug;
    std::string display_name;
    std::string summary;
    std::string version;
    int64_t updated_at = 0;
};

struct ClawHubSkillDetail {
    struct SkillInfo {
        std::string slug;
        std::string display_name;
        std::string summary;
        int64_t created_at = 0;
        int64_t updated_at = 0;
    };
    std::optional<SkillInfo> skill;
    struct VersionInfo {
        std::string version;
        int64_t created_at = 0;
        std::string changelog;
    };
    std::optional<VersionInfo> latest_version;
};

struct InstallClawHubSkillResult {
    bool ok = false;
    std::string slug;
    std::string version;
    std::string target_dir;
    std::string error;
};

struct UpdateClawHubSkillResult {
    bool ok = false;
    std::string slug;
    std::string previous_version;
    std::string version;
    bool changed = false;
    std::string target_dir;
    std::string error;
};

// ============================================================================
// ClawHub Manager (对齐 OC skills-clawhub.ts)
// ============================================================================

class ClawHubManager {
public:
    ClawHubManager();
    ~ClawHubManager();

    // 对齐 OC: searchSkillsFromClawHub
    std::vector<ClawHubSkillSearchResult> search(
        const std::string& query = "*",
        int limit = 20,
        const std::string& base_url = ""
    ) const;

    // 对齐 OC: installSkillFromClawHub
    InstallClawHubSkillResult install(
        const std::string& workspace_dir,
        const std::string& slug,
        const std::string& version = "",
        const std::string& base_url = "",
        bool force = false
    );

    // 对齐 OC: updateSkillsFromClawHub
    std::vector<UpdateClawHubSkillResult> update(
        const std::string& workspace_dir,
        const std::string& slug = "",
        const std::string& base_url = ""
    );

    // 对齐 OC: readTrackedClawHubSkillSlugs
    std::vector<std::string> list_tracked_slugs(const std::string& workspace_dir) const;

    // 对齐 OC: computeSkillFingerprint
    static std::string compute_fingerprint(const std::string& skill_dir);

    // Lockfile read/write
    static ClawHubSkillsLockfile read_lockfile(const std::string& workspace_dir);
    static void write_lockfile(const std::string& workspace_dir, const ClawHubSkillsLockfile& lock);

    // Origin read/write
    static std::optional<ClawHubSkillOrigin> read_origin(const std::string& skill_dir);
    static void write_origin(const std::string& skill_dir, const ClawHubSkillOrigin& origin);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Skill Status Report (对齐 OC skills-status.ts)
// ============================================================================

struct SkillInstallOption {
    std::string id;
    SkillInstallSpec::Kind kind;
    std::string label;
    std::vector<std::string> bins;
};

struct SkillStatusEntry {
    std::string name;
    std::string description;
    std::string source;
    bool bundled = false;
    std::string file_path;
    std::string base_dir;
    std::string skill_key;
    std::string primary_env;
    std::string emoji;
    std::string homepage;
    bool always = false;
    bool disabled = false;
    bool blocked_by_allowlist = false;
    bool eligible = false;
    SkillRequirements requirements;
    SkillRequirements missing;
    std::vector<SkillInstallOption> install;
};

struct SkillStatusReport {
    std::string workspace_dir;
    std::string managed_skills_dir;
    std::vector<SkillStatusEntry> skills;
};

// 对齐 OC: buildWorkspaceSkillStatus
SkillStatusReport build_workspace_skill_status(
    const std::string& workspace_dir,
    std::shared_ptr<core::Config> config = nullptr,
    const SkillEligibilityContext* eligibility = nullptr
);

// ============================================================================
// Skills Remote (对齐 OC infra/skills-remote.ts)
// ============================================================================

struct RemoteNodeRecord {
    std::string node_id;
    std::string display_name;
    std::string platform;
    std::string device_family;
    std::vector<std::string> commands;
    std::set<std::string> bins;
    std::string remote_ip;
};

class SkillsRemote {
public:
    static SkillsRemote& instance();

    // 对齐 OC: recordRemoteNodeInfo / removeRemoteNodeInfo
    void record_node(const RemoteNodeRecord& node);
    void remove_node(const std::string& node_id);

    // 对齐 OC: recordRemoteNodeBins
    void record_bins(const std::string& node_id, const std::vector<std::string>& bins);

    // 对齐 OC: getRemoteSkillEligibility
    std::optional<RemoteSkillEligibility> get_eligibility(bool advertise_exec_node = true) const;

    // 对齐 OC: refreshRemoteNodeBins
    void refresh_bins(const std::string& node_id, std::shared_ptr<core::Config> config);

    // 对齐 OC: primeRemoteSkillsCache
    void prime_cache();

private:
    SkillsRemote();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace skills
} // namespace hunterclaw
