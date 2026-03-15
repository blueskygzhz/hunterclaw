#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <optional>
#include <set>

namespace hunterclaw {
namespace memory {

// 严格对齐 OpenClaw src/memory/backend-config.ts

// ============================================================================
// QMD Collection配置 (对齐 ResolvedQmdCollection)
// ============================================================================
struct ResolvedQmdCollection {
    std::string name;
    std::string path;
    std::string pattern;
    std::string kind;  // "memory" | "custom" | "sessions"
};

// ============================================================================
// QMD Update配置 (对齐 ResolvedQmdUpdateConfig)
// ============================================================================
struct ResolvedQmdUpdateConfig {
    int interval_ms = 300000;          // 5m默认
    int debounce_ms = 15000;           // 15s
    bool on_boot = true;
    bool wait_for_boot_sync = false;
    int embed_interval_ms = 3600000;   // 60m
    int command_timeout_ms = 30000;    // 30s
    int update_timeout_ms = 120000;    // 120s
    int embed_timeout_ms = 120000;     // 120s
};

// ============================================================================
// QMD Limits配置 (对齐 ResolvedQmdLimitsConfig)
// ============================================================================
struct ResolvedQmdLimitsConfig {
    int max_results = 6;
    int max_snippet_chars = 700;
    int max_injected_chars = 4000;
    int timeout_ms = 4000;
};

// ============================================================================
// QMD Session配置 (对齐 ResolvedQmdSessionConfig)
// ============================================================================
struct ResolvedQmdSessionConfig {
    bool enabled = false;
    std::optional<std::string> export_dir;
    std::optional<int> retention_days;
};

// ============================================================================
// QMD MCPorter配置 (对齐 ResolvedQmdMcporterConfig)
// ============================================================================
struct ResolvedQmdMcporterConfig {
    bool enabled = false;
    std::string server_name = "qmd";
    bool start_daemon = true;
};

// ============================================================================
// Session作用域规则 (对齐 SessionSendPolicyConfig)
// ============================================================================
struct SessionScopeRule {
    std::string action;  // "allow" | "deny"
    struct Match {
        std::optional<std::string> chat_type;  // "direct" | "channel" | "group"
        std::optional<std::string> channel;
    } match;
};

struct SessionSendPolicyConfig {
    std::string default_action = "deny";  // "allow" | "deny"
    std::vector<SessionScopeRule> rules;
};

// ============================================================================
// QMD完整配置 (对齐 ResolvedQmdConfig)
// ============================================================================
struct ResolvedQmdConfig {
    std::string command = "qmd";
    ResolvedQmdMcporterConfig mcporter;
    std::string search_mode = "search";  // "search" | "vsearch" | "query"
    std::vector<ResolvedQmdCollection> collections;
    ResolvedQmdSessionConfig sessions;
    ResolvedQmdUpdateConfig update;
    ResolvedQmdLimitsConfig limits;
    bool include_default_memory = true;
    std::optional<SessionSendPolicyConfig> scope;
};

// ============================================================================
// Backend配置 (对齐 ResolvedMemoryBackendConfig)
// ============================================================================
struct ResolvedMemoryBackendConfig {
    std::string backend = "builtin";  // "builtin" | "qmd"
    std::string citations = "auto";   // "auto" | "always" | "never"
    std::optional<ResolvedQmdConfig> qmd;
};

// ============================================================================
// 配置解析参数
// ============================================================================
struct ResolveBackendParams {
    std::string agent_id;
    std::string workspace_dir;
    
    // Memory backend配置
    std::optional<std::string> backend;          // "builtin" | "qmd"
    std::optional<std::string> citations;        // "auto" | "always" | "never"
    
    // QMD配置(如果backend=qmd)
    std::optional<std::string> qmd_command;
    std::optional<bool> qmd_include_default_memory;
    std::optional<std::string> qmd_search_mode;
    
    // QMD collections
    struct CustomPath {
        std::string path;
        std::string pattern = "**/*.md";
        std::optional<std::string> name;
    };
    std::vector<CustomPath> qmd_custom_paths;
    
    // QMD子配置
    std::optional<ResolvedQmdMcporterConfig> qmd_mcporter;
    std::optional<ResolvedQmdSessionConfig> qmd_sessions;
    std::optional<ResolvedQmdUpdateConfig> qmd_update;
    std::optional<ResolvedQmdLimitsConfig> qmd_limits;
    std::optional<SessionSendPolicyConfig> qmd_scope;
};

// ============================================================================
// 核心API (对齐 resolveMemoryBackendConfig)
// ============================================================================

// 解析Memory Backend配置
ResolvedMemoryBackendConfig resolve_memory_backend_config(
    const ResolveBackendParams& params
);

// ============================================================================
// 辅助函数
// ============================================================================

// 路径解析 (对齐 resolvePath)
std::string resolve_path(const std::string& raw, const std::string& workspace_dir);

// 名称规范化 (对齐 sanitizeName)
std::string sanitize_name(const std::string& input);

// 作用域化Collection名称 (对齐 scopeCollectionBase)
std::string scope_collection_base(const std::string& base, const std::string& agent_id);

// 确保唯一名称 (对齐 ensureUniqueName)
std::string ensure_unique_name(
    const std::string& base, 
    std::set<std::string>& existing
);

// 解析默认Collection (对齐 resolveDefaultCollections)
std::vector<ResolvedQmdCollection> resolve_default_collections(
    bool include,
    const std::string& workspace_dir,
    std::set<std::string>& existing,
    const std::string& agent_id
);

// 解析自定义Path (对齐 resolveCustomPaths)
std::vector<ResolvedQmdCollection> resolve_custom_paths(
    const std::vector<ResolveBackendParams::CustomPath>& raw_paths,
    const std::string& workspace_dir,
    std::set<std::string>& existing,
    const std::string& agent_id
);

// 解析时间间隔 (对齐 resolveIntervalMs)
int resolve_interval_ms(const std::optional<std::string>& raw);

// 解析Embedding间隔
int resolve_embed_interval_ms(const std::optional<std::string>& raw);

// 解析超时时间
int resolve_timeout_ms(const std::optional<int>& raw, int fallback);

} // namespace memory
} // namespace hunterclaw
