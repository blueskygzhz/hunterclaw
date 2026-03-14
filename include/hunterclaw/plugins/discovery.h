#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/json.h"
#include <memory>
#include <string>
#include <vector>
#include <set>

namespace hunterclaw {
namespace plugins {

// ============================================================================
// Plugin Manifest
// ============================================================================

struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::string type;                          // memory/skills/channels/tools/generic
    std::string main;                          // 主文件路径 (相对于插件目录)
    std::vector<std::string> dependencies;     // 依赖的其他插件
    core::JsonValue config_schema;             // 配置 schema (JSON Schema)
};

// ============================================================================
// Plugin Origin
// ============================================================================

enum class PluginOrigin {
    Bundled,     // 内置插件 (系统级)
    User,        // 用户级插件 (~/.hunterclaw/plugins)
    Workspace    // 工作空间插件 (<workspace>/.hunterclaw/plugins)
};

// ============================================================================
// Security Issue
// ============================================================================

enum class SecuritySeverity {
    Warning,
    Error
};

struct SecurityIssue {
    std::string reason;
    std::string path;
    std::string root;
    uint32_t found_uid = 0;
    uint32_t expected_uid = 0;
    uint32_t mode_bits = 0;
    SecuritySeverity severity = SecuritySeverity::Warning;
};

// ============================================================================
// Plugin Candidate
// ============================================================================

struct PluginCandidate {
    PluginManifest manifest;
    std::string plugin_dir;
    std::string manifest_path;
    std::string main_file;
    PluginOrigin origin;
    std::vector<SecurityIssue> security_issues;
};

// ============================================================================
// PluginDiscovery - 插件发现和验证
// ============================================================================

class PluginDiscovery {
public:
    explicit PluginDiscovery(std::shared_ptr<core::Config> config);
    ~PluginDiscovery();
    
    // 发现所有插件
    std::vector<PluginCandidate> discover();
    
    // 按类型过滤
    static std::vector<PluginCandidate> filter_by_type(
        const std::vector<PluginCandidate>& candidates,
        const std::string& type
    );
    
    // 过滤安全的插件 (无 Error 级别问题)
    static std::vector<PluginCandidate> filter_secure(
        const std::vector<PluginCandidate>& candidates
    );
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace plugins
} // namespace hunterclaw
