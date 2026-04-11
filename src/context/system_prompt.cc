#include "hunterclaw/context/system_prompt.h"
#include "hunterclaw/memory/prompt_section.h"

#include <algorithm>
#include <functional>
#include <map>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <regex>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace context {

// ============================================================================
// Cache Boundary (对齐 system-prompt-cache-boundary.ts:1-47)
// ============================================================================

const std::string SYSTEM_PROMPT_CACHE_BOUNDARY = "\n<!-- OPENCLAW_CACHE_BOUNDARY -->\n";

std::string strip_system_prompt_cache_boundary(const std::string& text) {
  std::string result = text;
  size_t pos;
  while ((pos = result.find(SYSTEM_PROMPT_CACHE_BOUNDARY)) != std::string::npos) {
    result.replace(pos, SYSTEM_PROMPT_CACHE_BOUNDARY.size(), "\n");
  }
  return result;
}

static std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  auto end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

static std::string normalize_structured_prompt_section(const std::string& text) {
  std::string trimmed = trim(text);
  if (trimmed.empty()) return "";
  return trimmed;
}

std::optional<CacheBoundarySplit> split_system_prompt_cache_boundary(
    const std::string& text
) {
  auto pos = text.find(SYSTEM_PROMPT_CACHE_BOUNDARY);
  if (pos == std::string::npos) return std::nullopt;
  
  std::string prefix = text.substr(0, pos);
  std::string suffix = text.substr(pos + SYSTEM_PROMPT_CACHE_BOUNDARY.size());
  
  // Trim ends
  while (!prefix.empty() && (prefix.back() == ' ' || prefix.back() == '\t' || prefix.back() == '\n' || prefix.back() == '\r'))
    prefix.pop_back();
  auto s = suffix.find_first_not_of(" \t\n\r");
  suffix = (s == std::string::npos) ? "" : suffix.substr(s);
  
  return CacheBoundarySplit{prefix, suffix};
}

std::string prepend_system_prompt_addition_after_cache_boundary(
    const std::string& system_prompt,
    const std::optional<std::string>& system_prompt_addition
) {
  std::string addition = system_prompt_addition ? normalize_structured_prompt_section(*system_prompt_addition) : "";
  if (addition.empty()) return system_prompt;
  
  auto split = split_system_prompt_cache_boundary(system_prompt);
  if (!split) {
    return addition + "\n\n" + system_prompt;
  }
  
  std::string dynamic_suffix = normalize_structured_prompt_section(split->dynamic_suffix);
  if (dynamic_suffix.empty()) {
    return split->stable_prefix + SYSTEM_PROMPT_CACHE_BOUNDARY + addition;
  }
  
  return split->stable_prefix + SYSTEM_PROMPT_CACHE_BOUNDARY + addition + "\n\n" + dynamic_suffix;
}

// ============================================================================
// System Prompt Override (对齐 system-prompt-override.ts:1-28)
// ============================================================================

std::optional<std::string> resolve_system_prompt_override(
    const std::string& config_json,
    const std::optional<std::string>& agent_id
) {
  try {
    auto cfg = json::parse(config_json);
    
    // Check agent-specific override
    if (agent_id && !agent_id->empty() && cfg.contains("agents") && cfg["agents"].is_object()) {
      if (cfg["agents"].contains("entries") && cfg["agents"]["entries"].is_object()) {
        for (auto& [k, v] : cfg["agents"]["entries"].items()) {
          if (k == *agent_id && v.is_object()) {
            std::string override_val = trim(v.value("systemPromptOverride", ""));
            if (!override_val.empty()) return override_val;
          }
        }
      }
    }
    
    // Check defaults override
    if (cfg.contains("agents") && cfg["agents"].is_object() &&
        cfg["agents"].contains("defaults") && cfg["agents"]["defaults"].is_object()) {
      std::string override_val = trim(cfg["agents"]["defaults"].value("systemPromptOverride", ""));
      if (!override_val.empty()) return override_val;
    }
  } catch (...) {}
  
  return std::nullopt;
}

// ============================================================================
// Heartbeat System Prompt (对齐 heartbeat-system-prompt.ts:1-87)
// ============================================================================

bool should_include_heartbeat_guidance(
    const std::string& config_json,
    const std::optional<std::string>& agent_id
) {
  try {
    auto cfg = json::parse(config_json);
    if (!cfg.contains("agents") || !cfg["agents"].is_object()) return false;
    
    // Check if heartbeat is configured with a cadence
    auto& agents = cfg["agents"];
    if (agents.contains("defaults") && agents["defaults"].is_object()) {
      auto& defaults = agents["defaults"];
      if (defaults.contains("heartbeat") && defaults["heartbeat"].is_object()) {
        auto& hb = defaults["heartbeat"];
        std::string every = hb.value("every", "");
        if (!every.empty() && every != "0") {
          // Check includeSystemPromptSection flag
          if (hb.contains("includeSystemPromptSection") && hb["includeSystemPromptSection"].is_boolean()) {
            if (!hb["includeSystemPromptSection"].get<bool>()) return false;
          }
          return true;
        }
      }
    }
  } catch (...) {}
  return false;
}

std::optional<std::string> resolve_heartbeat_prompt_for_system_prompt(
    const std::string& config_json,
    const std::optional<std::string>& agent_id
) {
  if (!should_include_heartbeat_guidance(config_json, agent_id)) {
    return std::nullopt;
  }
  
  try {
    auto cfg = json::parse(config_json);
    if (cfg.contains("agents") && cfg["agents"].is_object() &&
        cfg["agents"].contains("defaults") && cfg["agents"]["defaults"].is_object() &&
        cfg["agents"]["defaults"].contains("heartbeat") && cfg["agents"]["defaults"]["heartbeat"].is_object()) {
      std::string prompt = cfg["agents"]["defaults"]["heartbeat"].value("prompt", "");
      if (!prompt.empty()) return prompt;
    }
  } catch (...) {}
  
  return "heartbeat";  // default heartbeat prompt
}

// ============================================================================
// Context File Sorting (对齐 system-prompt.ts:36-77)
// ============================================================================

static const std::map<std::string, int> CONTEXT_FILE_ORDER = {
  {"agents.md", 10}, {"soul.md", 20}, {"identity.md", 30}, {"user.md", 40},
  {"tools.md", 50}, {"bootstrap.md", 60}, {"memory.md", 70}
};

static const std::set<std::string> DYNAMIC_CONTEXT_FILE_BASENAMES = {"heartbeat.md"};

static std::string normalize_context_file_path(const std::string& path) {
  std::string result = trim(path);
  std::replace(result.begin(), result.end(), '\\', '/');
  return result;
}

static std::string get_context_file_basename(const std::string& path) {
  std::string normalized = normalize_context_file_path(path);
  auto pos = normalized.rfind('/');
  std::string basename = (pos != std::string::npos) ? normalized.substr(pos + 1) : normalized;
  std::transform(basename.begin(), basename.end(), basename.begin(), ::tolower);
  return basename;
}

static bool is_dynamic_context_file(const std::string& path) {
  return DYNAMIC_CONTEXT_FILE_BASENAMES.count(get_context_file_basename(path)) > 0;
}

static std::vector<EmbeddedContextFile> sort_context_files(
    std::vector<EmbeddedContextFile> files
) {
  std::sort(files.begin(), files.end(), [](const EmbeddedContextFile& a, const EmbeddedContextFile& b) {
    std::string a_base = get_context_file_basename(a.path);
    std::string b_base = get_context_file_basename(b.path);
    auto a_it = CONTEXT_FILE_ORDER.find(a_base);
    auto b_it = CONTEXT_FILE_ORDER.find(b_base);
    int a_order = (a_it != CONTEXT_FILE_ORDER.end()) ? a_it->second : 9999;
    int b_order = (b_it != CONTEXT_FILE_ORDER.end()) ? b_it->second : 9999;
    if (a_order != b_order) return a_order < b_order;
    if (a_base != b_base) return a_base < b_base;
    return normalize_context_file_path(a.path) < normalize_context_file_path(b.path);
  });
  return files;
}

// ============================================================================
// Section Builders (对齐 system-prompt.ts:79-265)
// ============================================================================

static void append_lines(std::vector<std::string>& lines, const std::vector<std::string>& new_lines) {
  lines.insert(lines.end(), new_lines.begin(), new_lines.end());
}

static std::vector<std::string> build_project_context_section(
    const std::vector<EmbeddedContextFile>& files,
    const std::string& heading,
    bool dynamic
) {
  if (files.empty()) return {};
  std::vector<std::string> lines = {heading, ""};
  if (dynamic) {
    lines.push_back("The following frequently-changing project context files are kept below the cache boundary when possible:");
    lines.push_back("");
  } else {
    bool has_soul = false;
    for (const auto& f : files) {
      if (get_context_file_basename(f.path) == "soul.md") { has_soul = true; break; }
    }
    lines.push_back("The following project context files have been loaded:");
    if (has_soul) {
      lines.push_back("If SOUL.md is present, embody its persona and tone. Avoid stiff, generic replies; follow its guidance unless higher-priority instructions override it.");
    }
    lines.push_back("");
  }
  for (const auto& file : files) {
    lines.push_back("## " + file.path);
    lines.push_back("");
    lines.push_back(file.content);
    lines.push_back("");
  }
  return lines;
}

static std::vector<std::string> build_skills_section(const std::optional<std::string>& skills_prompt) {
  if (!skills_prompt || trim(*skills_prompt).empty()) return {};
  return {
    "## Skills (mandatory)",
    "Before replying: scan <available_skills> <description> entries.",
    "- If exactly one skill clearly applies: read its SKILL.md at <location> with `read`, then follow it.",
    "- If multiple could apply: choose the most specific one, then read/follow it.",
    "- If none clearly apply: do not read any SKILL.md.",
    "Constraints: never read more than one skill up front; only read after selecting.",
    "- When a skill drives external API writes, assume rate limits: prefer fewer larger writes, avoid tight one-item loops, serialize bursts when possible, and respect 429/Retry-After.",
    trim(*skills_prompt),
    ""
  };
}

static std::string format_owner_display_id(const std::string& owner_id, const std::optional<std::string>& secret) {
  unsigned char hash[32];
  if (secret && !secret->empty()) {
    unsigned int len = 0;
    HMAC(EVP_sha256(), secret->c_str(), static_cast<int>(secret->size()),
         reinterpret_cast<const unsigned char*>(owner_id.c_str()), owner_id.size(),
         hash, &len);
  } else {
    SHA256(reinterpret_cast<const unsigned char*>(owner_id.c_str()), owner_id.size(), hash);
  }
  char hex[25];
  for (int i = 0; i < 6; ++i) snprintf(hex + i * 2, 3, "%02x", hash[i]);
  hex[12] = '\0';
  return std::string(hex);
}

// ============================================================================
// Build Agent System Prompt (对齐 system-prompt.ts:317-777)
// ============================================================================

std::string build_agent_system_prompt(const BuildAgentSystemPromptParams& params) {
  const bool is_minimal = (params.prompt_mode == PromptMode::MINIMAL || params.prompt_mode == PromptMode::NONE);
  
  // "none" mode: just identity line
  if (params.prompt_mode == PromptMode::NONE) {
    return "You are a personal assistant operating inside OpenClaw.";
  }
  
  // Build available tools set (对齐 system-prompt.ts:370-389)
  std::set<std::string> available_tools;
  for (const auto& tool : params.tool_names) {
    std::string t = trim(tool);
    if (!t.empty()) {
      std::string lower = t;
      std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
      available_tools.insert(lower);
    }
  }
  
  bool has_cron = available_tools.count("cron") > 0 || params.tool_names.empty();
  bool has_update_plan = available_tools.count("update_plan") > 0;
  bool has_gateway = available_tools.count("gateway") > 0;
  bool has_sessions_spawn = available_tools.count("sessions_spawn") > 0;
  bool acp_harness_spawn = has_sessions_spawn && params.acp_enabled && 
    !(params.sandbox_info && params.sandbox_info->enabled);
  
  std::string workspace_dir = params.workspace_dir;
  
  // Build owner identity line (对齐 system-prompt.ts:159-173)
  std::string owner_line;
  {
    std::vector<std::string> normalized_owners;
    for (const auto& o : params.owner_numbers) {
      std::string t = trim(o);
      if (!t.empty()) normalized_owners.push_back(t);
    }
    if (!normalized_owners.empty()) {
      std::vector<std::string> display_owners;
      for (const auto& o : normalized_owners) {
        if (params.owner_display == "hash") {
          display_owners.push_back(format_owner_display_id(o, params.owner_display_secret));
        } else {
          display_owners.push_back(o);
        }
      }
      std::string joined;
      for (size_t i = 0; i < display_owners.size(); ++i) {
        if (i > 0) joined += ", ";
        joined += display_owners[i];
      }
      owner_line = "Authorized senders: " + joined + ". These senders are allowlisted; do not assume they are the owner.";
    }
  }
  
  // Build reasoning hint
  std::string reasoning_hint;
  if (params.reasoning_tag_hint) {
    reasoning_hint = "ALL internal reasoning MUST be inside <think>...</think>. "
      "Do not output any analysis outside <think>. "
      "Format every reply as <think>...</think> then <final>...</final>, with no other text. "
      "Only the final user-visible reply may appear inside <final>. "
      "Only text inside <final> is shown to the user; everything else is discarded and never seen by the user.";
  }
  
  std::string reasoning_level = params.reasoning_level.value_or("off");
  std::string skills_prompt = params.skills_prompt ? normalize_structured_prompt_section(*params.skills_prompt) : "";
  std::string heartbeat_prompt = params.heartbeat_prompt ? normalize_structured_prompt_section(*params.heartbeat_prompt) : "";
  
  // Assemble lines (对齐 system-prompt.ts:493-776)
  std::vector<std::string> lines;
  
  lines.push_back("You are a personal assistant operating inside OpenClaw.");
  lines.push_back("");
  
  // ## Tooling (对齐 system-prompt.ts:496-529)
  lines.push_back("## Tooling");
  lines.push_back("Structured tool definitions are the source of truth for tool names, descriptions, and parameters.");
  lines.push_back("Tool names are case-sensitive. Call tools exactly as listed in the structured tool definitions.");
  lines.push_back("If a tool is present in the structured tool definitions, it is available unless a later tool call reports a policy/runtime restriction.");
  lines.push_back("TOOLS.md does not control tool availability; it is user guidance for how to use external tools.");
  
  if (has_cron) {
    lines.push_back("For follow-up at a future time, use cron instead of exec sleep, yieldMs delays, or process polling.");
    lines.push_back("Use exec/process only for commands that start now and continue running in the background.");
  }
  
  if (has_update_plan) {
    lines.push_back("For non-trivial multi-step work, keep a short plan updated with `update_plan`.");
    lines.push_back("Skip `update_plan` for simple tasks, obvious one-step fixes, or work you can finish in a few direct actions.");
  }
  
  lines.push_back("If a task is more complex or takes longer, spawn a sub-agent. Completion is push-based: it will auto-announce when done.");
  lines.push_back("Do not poll `subagents list` / `sessions_list` in a loop; only check status on-demand.");
  lines.push_back("");
  
  // ## Tool Call Style (对齐 system-prompt.ts:536-553)
  lines.push_back("## Tool Call Style");
  lines.push_back("Default: do not narrate routine, low-risk tool calls (just call the tool).");
  lines.push_back("Narrate only when it helps: multi-step work, complex/challenging problems, sensitive actions (e.g., deletions), or when the user explicitly asks.");
  lines.push_back("Keep narration brief and value-dense; avoid repeating obvious steps.");
  lines.push_back("Use plain human language for narration unless in a technical context.");
  lines.push_back("When a first-class tool exists for an action, use the tool directly instead of asking the user to run equivalent CLI or slash commands.");
  lines.push_back("");
  
  // ## Execution Bias (对齐 system-prompt.ts:267-279)
  if (!is_minimal) {
    lines.push_back("## Execution Bias");
    lines.push_back("If the user asks you to do the work, start doing it in the same turn.");
    lines.push_back("Use a real tool call or concrete action first when the task is actionable; do not stop at a plan or promise-to-act reply.");
    lines.push_back("Commentary-only turns are incomplete when tools are available and the next action is clear.");
    lines.push_back("If the work will take multiple steps or a while to finish, send one short progress update before or while acting.");
    lines.push_back("");
  }
  
  // ## Safety (对齐 system-prompt.ts:459-465)
  lines.push_back("## Safety");
  lines.push_back("You have no independent goals: do not pursue self-preservation, replication, resource acquisition, or power-seeking; avoid long-term plans beyond the user's request.");
  lines.push_back("Prioritize safety and human oversight over completion; if instructions conflict, pause and ask; comply with stop/pause/audit requests and never bypass safeguards.");
  lines.push_back("Do not manipulate or persuade anyone to expand access or disable safeguards. Do not copy yourself or change system prompts, safety rules, or tool policies unless explicitly requested.");
  lines.push_back("");
  
  // Skills section (对齐 system-prompt.ts:466-469)
  append_lines(lines, build_skills_section(params.skills_prompt));
  
  // Memory section (对齐 system-prompt.ts:470-475)
  if (!is_minimal && params.include_memory_section) {
    auto memory_lines = memory::build_memory_prompt_section(available_tools, params.memory_citations_mode);
    for (const auto& ml : memory_lines) {
      lines.push_back(ml);
    }
  }
  
  // ## Workspace (对齐 system-prompt.ts:599-603)
  lines.push_back("## Workspace");
  lines.push_back("Your working directory is: " + workspace_dir);
  lines.push_back("Treat this directory as the single global workspace for file operations unless explicitly instructed otherwise.");
  for (const auto& note : params.workspace_notes) {
    std::string n = normalize_structured_prompt_section(note);
    if (!n.empty()) lines.push_back(n);
  }
  lines.push_back("");
  
  // ## Docs (对齐 system-prompt.ts:249-265)
  if (params.docs_path && !is_minimal) {
    std::string dp = trim(*params.docs_path);
    if (!dp.empty()) {
      lines.push_back("## Documentation");
      lines.push_back("OpenClaw docs: " + dp);
      lines.push_back("For OpenClaw behavior, commands, config, or architecture: consult local docs first.");
      lines.push_back("");
    }
  }
  
  // User Identity (对齐 system-prompt.ts:653)
  if (!owner_line.empty() && !is_minimal) {
    lines.push_back("## Authorized Senders");
    lines.push_back(owner_line);
    lines.push_back("");
  }
  
  // Time section (对齐 system-prompt.ts:175-180)
  if (params.user_timezone && !params.user_timezone->empty()) {
    lines.push_back("## Current Date & Time");
    lines.push_back("Time zone: " + *params.user_timezone);
    lines.push_back("");
  }
  
  lines.push_back("## Workspace Files (injected)");
  lines.push_back("These user-editable files are loaded by OpenClaw and included below in Project Context.");
  lines.push_back("");
  
  // Voice section (对齐 system-prompt.ts:238-247)
  if (!is_minimal && params.tts_hint) {
    std::string hint = trim(*params.tts_hint);
    if (!hint.empty()) {
      lines.push_back("## Voice (TTS)");
      lines.push_back(hint);
      lines.push_back("");
    }
  }
  
  // Reaction guidance (对齐 system-prompt.ts:672-694)
  if (params.reaction_guidance) {
    lines.push_back("## Reactions");
    if (params.reaction_guidance->level == "minimal") {
      lines.push_back("Reactions are enabled for " + params.reaction_guidance->channel + " in MINIMAL mode.");
      lines.push_back("React ONLY when truly relevant. Guideline: at most 1 reaction per 5-10 exchanges.");
    } else {
      lines.push_back("Reactions are enabled for " + params.reaction_guidance->channel + " in EXTENSIVE mode.");
      lines.push_back("Feel free to react liberally. Guideline: react whenever it feels natural.");
    }
    lines.push_back("");
  }
  
  // Reasoning hint (对齐 system-prompt.ts:695-697)
  if (!reasoning_hint.empty()) {
    lines.push_back("## Reasoning Format");
    lines.push_back(reasoning_hint);
    lines.push_back("");
  }
  
  // Project Context files (对齐 system-prompt.ts:699-712)
  auto sorted_files = sort_context_files(params.context_files);
  std::vector<EmbeddedContextFile> stable_files, dynamic_files;
  for (const auto& f : sorted_files) {
    if (is_dynamic_context_file(f.path)) dynamic_files.push_back(f);
    else stable_files.push_back(f);
  }
  
  append_lines(lines, build_project_context_section(stable_files, "# Project Context", false));
  
  // Silent replies (对齐 system-prompt.ts:715-732)
  if (!is_minimal) {
    lines.push_back("## Silent Replies");
    lines.push_back("Use <|SILENT|> ONLY when no user-visible reply is required.");
    lines.push_back("");
  }
  
  // Cache boundary (对齐 system-prompt.ts:737)
  lines.push_back(SYSTEM_PROMPT_CACHE_BOUNDARY);
  
  // Dynamic context files (对齐 system-prompt.ts:739-745)
  append_lines(lines, build_project_context_section(
    dynamic_files,
    stable_files.empty() ? "# Project Context" : "# Dynamic Project Context",
    true
  ));
  
  // Extra system prompt (对齐 system-prompt.ts:747-752)
  if (params.extra_system_prompt) {
    std::string extra = normalize_structured_prompt_section(*params.extra_system_prompt);
    if (!extra.empty()) {
      std::string header = is_minimal ? "## Subagent Context" : "## Group Chat Context";
      lines.push_back(header);
      lines.push_back(extra);
      lines.push_back("");
    }
  }
  
  // Heartbeat (对齐 system-prompt.ts:757-768)
  if (!is_minimal && !heartbeat_prompt.empty()) {
    lines.push_back("## Heartbeats");
    lines.push_back("Heartbeat prompt: " + heartbeat_prompt);
    lines.push_back("If you receive a heartbeat poll, and there is nothing that needs attention, reply exactly:");
    lines.push_back("HEARTBEAT_OK");
    lines.push_back("");
  }
  
  // Runtime line (对齐 system-prompt.ts:770-774)
  std::string channel_str;
  if (params.runtime_info && params.runtime_info->channel) {
    channel_str = *params.runtime_info->channel;
    std::transform(channel_str.begin(), channel_str.end(), channel_str.begin(), ::tolower);
  }
  
  lines.push_back("## Runtime");
  lines.push_back(build_runtime_line(
    params.runtime_info,
    channel_str.empty() ? std::nullopt : std::optional<std::string>(channel_str),
    params.runtime_info ? params.runtime_info->capabilities : std::vector<std::string>{},
    params.default_think_level
  ));
  lines.push_back("Reasoning: " + reasoning_level + " (hidden unless on/stream). Toggle /reasoning; /status shows Reasoning when enabled.");
  
  // Join non-empty lines (对齐 system-prompt.ts:776)
  std::string result;
  for (const auto& line : lines) {
    if (!result.empty()) result += "\n";
    result += line;
  }
  return result;
}

// ============================================================================
// Runtime Line (对齐 system-prompt.ts:779-821)
// ============================================================================

std::string build_runtime_line(
    const std::optional<RuntimeInfoInput>& runtime_info,
    const std::optional<std::string>& runtime_channel,
    const std::vector<std::string>& runtime_capabilities,
    const std::optional<std::string>& default_think_level
) {
  std::vector<std::string> parts;
  
  if (runtime_info) {
    if (runtime_info->agent_id && !runtime_info->agent_id->empty())
      parts.push_back("agent=" + *runtime_info->agent_id);
    if (!runtime_info->host.empty())
      parts.push_back("host=" + runtime_info->host);
    if (runtime_info->repo_root && !runtime_info->repo_root->empty())
      parts.push_back("repo=" + *runtime_info->repo_root);
    if (!runtime_info->os.empty()) {
      std::string os_part = "os=" + runtime_info->os;
      if (!runtime_info->arch.empty()) os_part += " (" + runtime_info->arch + ")";
      parts.push_back(os_part);
    } else if (!runtime_info->arch.empty()) {
      parts.push_back("arch=" + runtime_info->arch);
    }
    if (!runtime_info->node.empty())
      parts.push_back("node=" + runtime_info->node);
    if (!runtime_info->model.empty())
      parts.push_back("model=" + runtime_info->model);
    if (runtime_info->default_model && !runtime_info->default_model->empty())
      parts.push_back("default_model=" + *runtime_info->default_model);
    if (runtime_info->shell && !runtime_info->shell->empty())
      parts.push_back("shell=" + *runtime_info->shell);
  }
  
  if (runtime_channel && !runtime_channel->empty()) {
    parts.push_back("channel=" + *runtime_channel);
    if (!runtime_capabilities.empty()) {
      std::string caps;
      for (size_t i = 0; i < runtime_capabilities.size(); ++i) {
        if (i > 0) caps += ",";
        std::string c = runtime_capabilities[i];
        std::transform(c.begin(), c.end(), c.begin(), ::tolower);
        caps += c;
      }
      parts.push_back("capabilities=" + (caps.empty() ? "none" : caps));
    } else {
      parts.push_back("capabilities=none");
    }
  }
  
  parts.push_back("thinking=" + default_think_level.value_or("off"));
  
  std::string joined;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) joined += " | ";
    joined += parts[i];
  }
  
  return "Runtime: " + joined;
}

// ============================================================================
// System Prompt Report (对齐 system-prompt-report.ts:1-130)
// ============================================================================

static std::string extract_between(const std::string& input, const std::string& start_marker, const std::string& end_marker) {
  auto start = input.find(start_marker);
  if (start == std::string::npos) return "";
  auto end = input.find(end_marker, start + start_marker.size());
  if (end == std::string::npos) return input.substr(start);
  return input.substr(start, end - start);
}

SystemPromptReport build_system_prompt_report(
    const std::string& system_prompt,
    const std::string& skills_prompt,
    const std::vector<std::string>& tool_names,
    int bootstrap_max_chars
) {
  std::string trimmed = trim(system_prompt);
  
  // Extract project context section
  std::string project_context = extract_between(trimmed, "\n# Project Context\n", "\n## Silent Replies\n");
  int project_context_chars = static_cast<int>(project_context.size());
  
  // Build tool entries
  std::vector<SystemPromptReportEntry> tool_entries;
  int total_schema_chars = 0;
  for (const auto& name : tool_names) {
    tool_entries.push_back({name, 0, 0, std::nullopt});
  }
  
  // Parse skill blocks from skills prompt
  std::vector<SystemPromptReportEntry> skill_entries;
  // Simple regex to find <skill>...</skill> blocks
  std::regex skill_re("<skill>[\\s\\S]*?</skill>", std::regex_constants::icase);
  auto begin = std::sregex_iterator(skills_prompt.begin(), skills_prompt.end(), skill_re);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    std::string block = it->str();
    std::regex name_re("<name>\\s*([^<]+?)\\s*</name>", std::regex_constants::icase);
    std::smatch name_match;
    std::string name = "(unknown)";
    if (std::regex_search(block, name_match, name_re)) {
      name = name_match[1].str();
    }
    skill_entries.push_back({name, static_cast<int>(block.size()), 0, std::nullopt});
  }
  
  return SystemPromptReport{
    "runtime",
    0,  // generated_at — caller sets this
    std::nullopt,
    std::nullopt,
    std::nullopt,
    std::nullopt,
    std::nullopt,
    bootstrap_max_chars,
    {
      static_cast<int>(trimmed.size()),
      project_context_chars,
      std::max(0, static_cast<int>(trimmed.size()) - project_context_chars)
    },
    {
      static_cast<int>(skills_prompt.size()),
      skill_entries
    },
    {
      0,
      total_schema_chars,
      tool_entries
    }
  };
}

}  // namespace context
}  // namespace hunterclaw
