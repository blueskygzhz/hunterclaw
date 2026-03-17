#include "hunterclaw/plugins/discovery.h"
#include "hunterclaw/core/json.h"
#include <filesystem>  // ✅ 使用标准库而非自定义 filesystem.h
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace plugins {

// ============================================================================
// PluginDiscovery Implementation
// ============================================================================

struct PluginDiscovery::Impl {
    std::shared_ptr<core::Config> config;
    std::vector<std::string> search_paths;
    uint32_t current_uid;
    
    Impl(std::shared_ptr<core::Config> cfg) : config(cfg) {
        current_uid = getuid();
        init_search_paths();
    }
    
    void init_search_paths() {
        // 默认搜索路径 (优先级从低到高)
        std::vector<std::string> default_paths = {
            "/usr/lib/hunterclaw/plugins",           // System
            "~/.hunterclaw/plugins-bundled",         // Bundled
            "~/.hunterclaw/plugins",                 // User
        };
        
        // 展开 ~ 符号
        const char* home = std::getenv("HOME");
        for (auto& path : default_paths) {
            if (path[0] == '~' && home) {
                path = std::string(home) + path.substr(1);
            }
            search_paths.push_back(path);
        }
        
        // 从配置读取额外路径
        auto custom_paths = config->get_string_list("plugins.loadPaths", {});
        for (const auto& path : custom_paths) {
            std::string expanded = path;
            if (expanded[0] == '~' && home) {
                expanded = std::string(home) + expanded.substr(1);
            }
            search_paths.push_back(expanded);
        }
        
        // 工作空间插件目录
        std::string workspace_dir = config->get_string("workspace_dir", ".");
        search_paths.push_back(workspace_dir + "/.hunterclaw/plugins");
    }
    
    PluginOrigin determine_origin(const std::string& plugin_dir) {
        if (plugin_dir.find("plugins-bundled") != std::string::npos ||
            plugin_dir.find("/usr/lib/hunterclaw") != std::string::npos) {
            return PluginOrigin::Bundled;
        } else if (plugin_dir.find(".hunterclaw/plugins") != std::string::npos) {
            return PluginOrigin::User;
        } else {
            return PluginOrigin::Workspace;
        }
    }
    
    core::Result<PluginManifest> read_manifest(const std::string& manifest_path) {
        std::ifstream ifs(manifest_path);
        if (!ifs.is_open()) {
            return core::Result<PluginManifest>::Err("Failed to open manifest file");
        }
        
        std::stringstream ss;
        ss << ifs.rdbuf();
        ifs.close();
        
        auto parse_result = core::Json::parse(ss.str());
        if (parse_result.is_err()) {
            return core::Result<PluginManifest>::Err("Failed to parse manifest: " + parse_result.unwrap_err());
        }
        
        core::JsonValue root = parse_result.unwrap();
        
        // 验证必填字段
        if (!root.has("id")) {
            return core::Result<PluginManifest>::Err("Missing required field: id");
        }
        
        PluginManifest manifest;
        manifest.id = root["id"].as_string();
        manifest.name = root.get("name", manifest.id);
        manifest.version = root.get("version", "0.0.0");
        manifest.description = root.get("description", "");
        manifest.type = root.get("type", "generic");
        manifest.main = root.get("main", "");
        
        // 解析依赖
        if (root.has("dependencies")) {
            auto deps = root["dependencies"];
            if (deps.is_array()) {
                for (size_t i = 0; i < deps.size(); ++i) {
                    manifest.dependencies.push_back(deps[i].as_string());
                }
            }
        }
        
        // 解析配置 schema
        if (root.has("config")) {
            manifest.config_schema = root["config"];
        }
        
        return core::Result<PluginManifest>::Ok(manifest);
    }
    
    core::Result<SecurityIssue> check_path_security(
        const std::string& target_path,
        const std::string& root_dir,
        PluginOrigin origin
    ) {
        struct stat st;
        if (stat(target_path.c_str(), &st) != 0) {
            SecurityIssue issue;
            issue.reason = "path_not_found";
            issue.path = target_path;
            issue.severity = SecuritySeverity::Error;
            return core::Result<SecurityIssue>::Ok(issue);
        }
        
        // 1. 检查世界可写权限
        mode_t mode = st.st_mode & 0777;
        if ((mode & 0002) != 0) {
            SecurityIssue issue;
            issue.reason = "path_world_writable";
            issue.path = target_path;
            issue.mode_bits = mode;
            issue.severity = SecuritySeverity::Error;
            return core::Result<SecurityIssue>::Ok(issue);
        }
        
        // 2. 检查所有权 (User 和 Workspace 插件)
        if (origin != PluginOrigin::Bundled) {
            if (st.st_uid != current_uid && st.st_uid != 0) {
                SecurityIssue issue;
                issue.reason = "path_suspicious_ownership";
                issue.path = target_path;
                issue.found_uid = st.st_uid;
                issue.expected_uid = current_uid;
                issue.severity = SecuritySeverity::Warning;
                return core::Result<SecurityIssue>::Ok(issue);
            }
        }
        
        // 3. 检查是否逃逸根目录
        char* real_target = realpath(target_path.c_str(), nullptr);
        char* real_root = realpath(root_dir.c_str(), nullptr);
        
        if (!real_target || !real_root) {
            if (real_target) free(real_target);
            if (real_root) free(real_root);
            
            SecurityIssue issue;
            issue.reason = "path_resolution_failed";
            issue.path = target_path;
            issue.severity = SecuritySeverity::Error;
            return core::Result<SecurityIssue>::Ok(issue);
        }
        
        std::string resolved_target(real_target);
        std::string resolved_root(real_root);
        free(real_target);
        free(real_root);
        
        if (resolved_target.find(resolved_root) != 0) {
            SecurityIssue issue;
            issue.reason = "source_escapes_root";
            issue.path = target_path;
            issue.root = root_dir;
            issue.severity = SecuritySeverity::Error;
            return core::Result<SecurityIssue>::Ok(issue);
        }
        
        // 无安全问题
        return core::Result<SecurityIssue>::Err("No security issues");
    }
    
    bool is_plugin_allowed(const std::string& plugin_id, PluginOrigin origin) {
        // Bundled 插件总是允许
        if (origin == PluginOrigin::Bundled) {
            return true;
        }
        
        // 检查白名单
        // Config 没有 get_string_list 方法，暂时允许所有插件
        std::vector<std::string> allow_list;  // Empty = allow all
        if (allow_list.empty()) {
            // 空白名单 = 允许所有
            return true;
        }
        
        return std::find(allow_list.begin(), allow_list.end(), plugin_id) != allow_list.end();
    }
};

// ============================================================================
// PluginDiscovery
// ============================================================================

PluginDiscovery::PluginDiscovery(std::shared_ptr<core::Config> config)
    : impl_(std::make_unique<Impl>(config))
{
}

PluginDiscovery::~PluginDiscovery() = default;

std::vector<PluginCandidate> PluginDiscovery::discover() {
    std::vector<PluginCandidate> candidates;
    std::set<std::string> seen_ids;
    
    // 按优先级从高到低扫描 (后扫描的覆盖先扫描的)
    for (auto it = impl_->search_paths.rbegin(); it != impl_->search_paths.rend(); ++it) {
        const std::string& search_path = *it;
        
        if (!fs::exists(search_path)) {
            continue;
        }
        
        // 扫描目录
        for (const auto& entry : fs::directory_iterator(search_path)) {
            if (!entry.is_directory()) {
                continue;
            }
            
            std::string plugin_dir = entry.path();
            std::string manifest_path = plugin_dir + "/manifest.json";
            
            // 检查清单文件
            if (!fs::exists(manifest_path)) {
                // 尝试 .plugin.json
                manifest_path = plugin_dir + "/" + entry.path().filename().string() + ".plugin.json";
                if (!fs::exists(manifest_path)) {
                    continue;
                }
            }
            
            // 读取清单
            auto manifest_result = impl_->read_manifest(manifest_path);
            if (manifest_result.is_err()) {
                continue;
            }
            
            PluginManifest manifest = manifest_result.unwrap();
            
            // 检查是否已见过
            if (seen_ids.count(manifest.id) > 0) {
                // 高优先级已存在, 跳过
                continue;
            }
            
            seen_ids.insert(manifest.id);
            
            // 确定来源
            PluginOrigin origin = impl_->determine_origin(plugin_dir);
            
            // 检查是否允许
            if (!impl_->is_plugin_allowed(manifest.id, origin)) {
                continue;
            }
            
            // 构建候选插件
            PluginCandidate candidate;
            candidate.manifest = manifest;
            candidate.plugin_dir = plugin_dir;
            candidate.manifest_path = manifest_path;
            candidate.origin = origin;
            
            // 安全检查
            auto security_result = impl_->check_path_security(plugin_dir, search_path, origin);
            if (security_result.is_ok()) {
                candidate.security_issues.push_back(security_result.unwrap());
            }
            
            // 解析主文件路径
            if (!manifest.main.empty()) {
                candidate.main_file = plugin_dir + "/" + manifest.main;
                
                if (!fs::exists(candidate.main_file)) {
                    SecurityIssue issue;
                    issue.reason = "main_file_not_found";
                    issue.path = candidate.main_file;
                    issue.severity = SecuritySeverity::Error;
                    candidate.security_issues.push_back(issue);
                }
            }
            
            candidates.push_back(candidate);
        }
    }
    
    return candidates;
}

std::vector<PluginCandidate> PluginDiscovery::filter_by_type(
    const std::vector<PluginCandidate>& candidates,
    const std::string& type
) {
    std::vector<PluginCandidate> filtered;
    
    for (const auto& candidate : candidates) {
        if (candidate.manifest.type == type) {
            filtered.push_back(candidate);
        }
    }
    
    return filtered;
}

std::vector<PluginCandidate> PluginDiscovery::filter_secure(
    const std::vector<PluginCandidate>& candidates
) {
    std::vector<PluginCandidate> filtered;
    
    for (const auto& candidate : candidates) {
        bool has_error = false;
        
        for (const auto& issue : candidate.security_issues) {
            if (issue.severity == SecuritySeverity::Error) {
                has_error = true;
                break;
            }
        }
        
        if (!has_error) {
            filtered.push_back(candidate);
        }
    }
    
    return filtered;
}

} // namespace plugins
} // namespace hunterclaw
