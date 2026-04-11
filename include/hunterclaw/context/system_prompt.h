#pragma once

// 严格对齐 OpenClaw agents/system-prompt.ts (822行)
// 主系统提示词构建引擎。

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hunterclaw {
namespace context {

// ============================================================================
// Types (对齐 system-prompt.ts:33-34, 36-44)
// ============================================================================

/// 对齐 PromptMode
enum class PromptMode {
  FULL,      // All sections (default, for main agent)
  MINIMAL,   // Reduced sections (Tooling, Workspace, Runtime) — subagents
  NONE       // Just basic identity line, no sections
};

/// 对齐 EmbeddedContextFile (pi-embedded-helpers.ts)
struct EmbeddedContextFile {
  std::string path;
  std::string content;
};

/// 对齐 EmbeddedSandboxInfo (pi-embedded-runner/types.ts)
struct EmbeddedSandboxInfo {
  bool enabled = false;
  std::optional<std::string> container_workspace_dir;
  std::optional<std::string> workspace_dir;
  std::optional<std::string> workspace_access;
  std::optional<std::string> agent_workspace_mount;
  std::optional<std::string> browser_bridge_url;
  std::optional<std::string> browser_novnc_url;
  std::optional<bool> host_browser_allowed;
  struct Elevated {
    bool allowed = false;
    std::string default_level = "ask";
  };
  std::optional<Elevated> elevated;
};

/// 对齐 RuntimeInfoInput (system-prompt-params.ts:12-26)
struct RuntimeInfoInput {
  std::optional<std::string> agent_id;
  std::string host;
  std::string os;
  std::string arch;
  std::string node;
  std::string model;
  std::optional<std::string> default_model;
  std::optional<std::string> shell;
  std::optional<std::string> channel;
  std::vector<std::string> capabilities;
  std::optional<std::string> repo_root;
  std::optional<std::string> provider;
};

/// 对齐 ProviderSystemPromptContribution (system-prompt-contribution.ts)
struct ProviderSystemPromptContribution {
  std::optional<std::string> stable_prefix;
  std::optional<std::string> dynamic_suffix;
  std::map<std::string, std::string> section_overrides;  // key: section_id
};

/// 对齐 ReactionGuidance
struct ReactionGuidance {
  std::string level;   // "minimal" | "extensive"
  std::string channel;
};

// ============================================================================
// Build Agent System Prompt (对齐 system-prompt.ts:317-777)
// ============================================================================

struct BuildAgentSystemPromptParams {
  std::string workspace_dir;
  std::optional<std::string> default_think_level;   // "off" | "on" | "stream"
  std::optional<std::string> reasoning_level;       // "off" | "low" | "medium" | "high"
  std::optional<std::string> extra_system_prompt;
  std::vector<std::string> owner_numbers;
  std::string owner_display = "raw";                // "raw" | "hash"
  std::optional<std::string> owner_display_secret;
  bool reasoning_tag_hint = false;
  std::vector<std::string> tool_names;
  std::vector<std::string> model_alias_lines;
  std::optional<std::string> user_timezone;
  std::optional<std::string> user_time;
  std::vector<EmbeddedContextFile> context_files;
  std::optional<std::string> skills_prompt;
  std::optional<std::string> heartbeat_prompt;
  std::optional<std::string> docs_path;
  std::vector<std::string> workspace_notes;
  std::optional<std::string> tts_hint;
  PromptMode prompt_mode = PromptMode::FULL;
  bool acp_enabled = true;
  std::optional<RuntimeInfoInput> runtime_info;
  std::vector<std::string> message_tool_hints;
  std::optional<EmbeddedSandboxInfo> sandbox_info;
  std::optional<ReactionGuidance> reaction_guidance;
  bool include_memory_section = true;
  std::string memory_citations_mode = "auto";
  std::optional<ProviderSystemPromptContribution> prompt_contribution;
};

/// Build the full agent system prompt.
/// 对齐 system-prompt.ts:317-777 buildAgentSystemPrompt
std::string build_agent_system_prompt(const BuildAgentSystemPromptParams& params);

/// Build the runtime info line.
/// 对齐 system-prompt.ts:779-821 buildRuntimeLine
std::string build_runtime_line(
    const std::optional<RuntimeInfoInput>& runtime_info,
    const std::optional<std::string>& runtime_channel = std::nullopt,
    const std::vector<std::string>& runtime_capabilities = {},
    const std::optional<std::string>& default_think_level = std::nullopt
);

// ============================================================================
// System Prompt Cache Boundary (对齐 system-prompt-cache-boundary.ts:1-47)
// ============================================================================

extern const std::string SYSTEM_PROMPT_CACHE_BOUNDARY;

/// Strip all cache boundary markers from text.
std::string strip_system_prompt_cache_boundary(const std::string& text);

/// Split text at the cache boundary.
struct CacheBoundarySplit {
  std::string stable_prefix;
  std::string dynamic_suffix;
};

std::optional<CacheBoundarySplit> split_system_prompt_cache_boundary(
    const std::string& text
);

/// Prepend system prompt addition after the cache boundary.
std::string prepend_system_prompt_addition_after_cache_boundary(
    const std::string& system_prompt,
    const std::optional<std::string>& system_prompt_addition = std::nullopt
);

// ============================================================================
// System Prompt Override (对齐 system-prompt-override.ts:1-28)
// ============================================================================

/// Resolve system prompt override from config.
std::optional<std::string> resolve_system_prompt_override(
    const std::string& config_json,
    const std::optional<std::string>& agent_id = std::nullopt
);

// ============================================================================
// Heartbeat System Prompt (对齐 heartbeat-system-prompt.ts:1-87)
// ============================================================================

/// Should include heartbeat guidance in system prompt?
bool should_include_heartbeat_guidance(
    const std::string& config_json,
    const std::optional<std::string>& agent_id = std::nullopt
);

/// Resolve the heartbeat prompt text for system prompt inclusion.
std::optional<std::string> resolve_heartbeat_prompt_for_system_prompt(
    const std::string& config_json,
    const std::optional<std::string>& agent_id = std::nullopt
);

// ============================================================================
// System Prompt Report (对齐 system-prompt-report.ts:1-130)
// ============================================================================

struct SystemPromptReportEntry {
  std::string name;
  int summary_chars;
  int schema_chars;
  std::optional<int> properties_count;
};

struct SystemPromptReport {
  std::string source;
  int64_t generated_at;
  std::optional<std::string> session_id;
  std::optional<std::string> session_key;
  std::optional<std::string> provider;
  std::optional<std::string> model;
  std::optional<std::string> workspace_dir;
  int bootstrap_max_chars;
  struct {
    int chars;
    int project_context_chars;
    int non_project_context_chars;
  } system_prompt;
  struct {
    int prompt_chars;
    std::vector<SystemPromptReportEntry> entries;
  } skills;
  struct {
    int list_chars;
    int schema_chars;
    std::vector<SystemPromptReportEntry> entries;
  } tools;
};

/// Build a system prompt report for diagnostics.
SystemPromptReport build_system_prompt_report(
    const std::string& system_prompt,
    const std::string& skills_prompt,
    const std::vector<std::string>& tool_names,
    int bootstrap_max_chars
);

}  // namespace context
}  // namespace hunterclaw
