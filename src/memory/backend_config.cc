#include <optional>
#include "hunterclaw/memory/backend_config.h"
#include <regex>
#include <set>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

// ============================================================================
// 常量定义 (对齐 backend-config.ts defaults)
// ============================================================================

static const char* DEFAULT_BACKEND = "builtin";
static const char* DEFAULT_CITATIONS = "auto";
static const int DEFAULT_QMD_INTERVAL_MS = 300000;  // 5m
static const int DEFAULT_QMD_DEBOUNCE_MS = 15000;
static const int DEFAULT_QMD_TIMEOUT_MS = 4000;
static const char* DEFAULT_QMD_SEARCH_MODE = "search";
static const int DEFAULT_QMD_EMBED_INTERVAL_MS = 3600000;  // 60m
static const int DEFAULT_QMD_COMMAND_TIMEOUT_MS = 30000;
static const int DEFAULT_QMD_UPDATE_TIMEOUT_MS = 120000;
static const int DEFAULT_QMD_EMBED_TIMEOUT_MS = 120000;

// 默认Limits
static const ResolvedQmdLimitsConfig DEFAULT_QMD_LIMITS = {
    .max_results = 6,
    .max_snippet_chars = 700,
    .max_injected_chars = 4000,
    .timeout_ms = DEFAULT_QMD_TIMEOUT_MS,
};

// 默认MCPorter
static const ResolvedQmdMcporterConfig DEFAULT_QMD_MCPORTER = {
    .enabled = false,
    .server_name = "qmd",
    .start_daemon = true,
};

// 默认Scope (对齐 DEFAULT_QMD_SCOPE)
static const SessionSendPolicyConfig DEFAULT_QMD_SCOPE = {
    .default_action = "deny",
    .rules = {
        {
            .action = "allow",
            .match = { .chat_type = "direct", .channel = std::nullopt }
        }
    }
};

// ============================================================================
// 辅助函数实现
// ============================================================================

std::string sanitize_name(const std::string& input) {
    // 转小写 + 非字母数字替换为"-"
    std::string lower;
    for (char c : input) {
        if (std::isalnum(c)) {
            lower += std::tolower(c);
        } else {
            lower += '-';
        }
    }
    
    // 去除开头和结尾的"-"
    size_t start = lower.find_first_not_of('-');
    size_t end = lower.find_last_not_of('-');
    
    if (start == std::string::npos) {
        return "collection";
    }
    
    std::string trimmed = lower.substr(start, end - start + 1);
    return trimmed.empty() ? "collection" : trimmed;
}

std::string scope_collection_base(const std::string& base, const std::string& agent_id) {
    return base + "-" + sanitize_name(agent_id);
}

std::string ensure_unique_name(
    const std::string& base, 
    std::set<std::string>& existing
) {
    std::string name = sanitize_name(base);
    if (existing.find(name) == existing.end()) {
        existing.insert(name);
        return name;
    }
    
    // 添加后缀
    int suffix = 2;
    std::string candidate;
    do {
        candidate = name + "-" + std::to_string(suffix);
        suffix++;
    } while (existing.find(candidate) != existing.end());
    
    existing.insert(candidate);
    return candidate;
}

std::string resolve_path(const std::string& raw, const std::string& workspace_dir) {
    std::string trimmed = raw;
    // 去除空白
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
    
    if (trimmed.empty()) {
        throw std::invalid_argument("path required");
    }
    
    // 处理~开头
    if (trimmed[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            trimmed = std::string(home) + trimmed.substr(1);
        }
    }
    
    // 绝对路径
    if (fs::path(trimmed).is_absolute()) {
        return fs::path(trimmed).lexically_normal().string();
    }
    
    // 相对路径
    return (fs::path(workspace_dir) / trimmed).lexically_normal().string();
}

// 解析时间间隔 (支持 "5m", "60s" 等)
int parse_duration_ms(const std::string& value, char default_unit = 'm') {
    std::regex duration_regex(R"((\d+)([smh])?)");
    std::smatch match;
    
    if (!std::regex_match(value, match, duration_regex)) {
        return -1;  // 解析失败
    }
    
    int num = std::stoi(match[1].str());
    char unit = match[2].length() > 0 ? match[2].str()[0] : default_unit;
    
    switch (unit) {
        case 's': return num * 1000;
        case 'm': return num * 60000;
        case 'h': return num * 3600000;
        default: return -1;
    }
}

int resolve_interval_ms(const std::optional<std::string>& raw) {
    if (!raw || raw->empty()) {
        return DEFAULT_QMD_INTERVAL_MS;
    }
    
    int result = parse_duration_ms(*raw, 'm');
    return result > 0 ? result : DEFAULT_QMD_INTERVAL_MS;
}

int resolve_embed_interval_ms(const std::optional<std::string>& raw) {
    if (!raw || raw->empty()) {
        return DEFAULT_QMD_EMBED_INTERVAL_MS;
    }
    
    int result = parse_duration_ms(*raw, 'm');
    return result > 0 ? result : DEFAULT_QMD_EMBED_INTERVAL_MS;
}

int resolve_timeout_ms(const std::optional<int>& raw, int fallback) {
    if (raw && *raw > 0) {
        return *raw;
    }
    return fallback;
}

// ============================================================================
// Collection解析
// ============================================================================

std::vector<ResolvedQmdCollection> resolve_default_collections(
    bool include,
    const std::string& workspace_dir,
    std::set<std::string>& existing,
    const std::string& agent_id
) {
    if (!include) {
        return {};
    }
    
    std::vector<ResolvedQmdCollection> collections;
    
    // 对齐 OpenClaw: MEMORY.md, memory.md, memory/**/*.md
    struct Entry {
        std::string path;
        std::string pattern;
        std::string base;
    };
    
    std::vector<Entry> entries = {
        { workspace_dir, "MEMORY.md", "memory-root" },
        { workspace_dir, "memory.md", "memory-alt" },
        { (fs::path(workspace_dir) / "memory").string(), "**/*.md", "memory-dir" }
    };
    
    for (const auto& entry : entries) {
        collections.push_back({
            .name = ensure_unique_name(
                scope_collection_base(entry.base, agent_id), 
                existing
            ),
            .path = entry.path,
            .pattern = entry.pattern,
            .kind = "memory"
        });
    }
    
    return collections;
}

std::vector<ResolvedQmdCollection> resolve_custom_paths(
    const std::vector<ResolveBackendParams::CustomPath>& raw_paths,
    const std::string& workspace_dir,
    std::set<std::string>& existing,
    const std::string& agent_id
) {
    std::vector<ResolvedQmdCollection> collections;
    
    for (size_t i = 0; i < raw_paths.size(); i++) {
        const auto& entry = raw_paths[i];
        
        if (entry.path.empty()) {
            continue;
        }
        
        std::string resolved;
        try {
            resolved = resolve_path(entry.path, workspace_dir);
        } catch (...) {
            continue;  // 跳过无效路径
        }
        
        std::string pattern = entry.pattern.empty() ? "**/*.md" : entry.pattern;
        
        std::string base_name = entry.name && !entry.name->empty() 
            ? *entry.name 
            : "custom-" + std::to_string(i + 1);
        
        std::string scoped_base = scope_collection_base(base_name, agent_id);
        std::string unique_name = ensure_unique_name(scoped_base, existing);
        
        collections.push_back({
            .name = unique_name,
            .path = resolved,
            .pattern = pattern,
            .kind = "custom"
        });
    }
    
    return collections;
}

// ============================================================================
// 核心配置解析 (对齐 resolveMemoryBackendConfig)
// ============================================================================

ResolvedMemoryBackendConfig resolve_memory_backend_config(
    const ResolveBackendParams& params
) {
    std::string backend = params.backend.value_or(DEFAULT_BACKEND);
    std::string citations = params.citations.value_or(DEFAULT_CITATIONS);
    
    if (backend != "qmd") {
        return {
            .backend = "builtin",
            .citations = citations,
            .qmd = std::nullopt
        };
    }
    
    // QMD配置
    ResolvedQmdConfig qmd_config;
    qmd_config.command = params.qmd_command.value_or("qmd");
    qmd_config.search_mode = params.qmd_search_mode.value_or(DEFAULT_QMD_SEARCH_MODE);
    qmd_config.include_default_memory = params.qmd_include_default_memory.value_or(true);
    
    // MCPorter
    qmd_config.mcporter = params.qmd_mcporter.value_or(DEFAULT_QMD_MCPORTER);
    
    // Sessions
    if (params.qmd_sessions) {
        qmd_config.sessions = *params.qmd_sessions;
    }
    
    // Update
    if (params.qmd_update) {
        qmd_config.update = *params.qmd_update;
    } else {
        qmd_config.update = {
            .interval_ms = DEFAULT_QMD_INTERVAL_MS,
            .debounce_ms = DEFAULT_QMD_DEBOUNCE_MS,
            .on_boot = true,
            .wait_for_boot_sync = false,
            .embed_interval_ms = DEFAULT_QMD_EMBED_INTERVAL_MS,
            .command_timeout_ms = DEFAULT_QMD_COMMAND_TIMEOUT_MS,
            .update_timeout_ms = DEFAULT_QMD_UPDATE_TIMEOUT_MS,
            .embed_timeout_ms = DEFAULT_QMD_EMBED_TIMEOUT_MS,
        };
    }
    
    // Limits
    qmd_config.limits = params.qmd_limits.value_or(DEFAULT_QMD_LIMITS);
    
    // Scope
    qmd_config.scope = params.qmd_scope.value_or(DEFAULT_QMD_SCOPE);
    
    // Collections
    std::set<std::string> name_set;
    std::vector<ResolvedQmdCollection> collections;
    
    // 默认collections
    auto default_cols = resolve_default_collections(
        qmd_config.include_default_memory,
        params.workspace_dir,
        name_set,
        params.agent_id
    );
    collections.insert(collections.end(), default_cols.begin(), default_cols.end());
    
    // 自定义collections
    auto custom_cols = resolve_custom_paths(
        params.qmd_custom_paths,
        params.workspace_dir,
        name_set,
        params.agent_id
    );
    collections.insert(collections.end(), custom_cols.begin(), custom_cols.end());
    
    qmd_config.collections = collections;
    
    return {
        .backend = "qmd",
        .citations = citations,
        .qmd = qmd_config
    };
}

} // namespace memory
} // namespace hunterclaw
