// HunterClaw - Tool 策略管道实现

#include "hunterclaw/tools/tool_policy.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace tools {

namespace {
    auto log() {
        static auto logger = spdlog::get("tools");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("tools");
        }
        return logger;
    }
}

// ============================================================================
// WhitelistPolicy
// ============================================================================

WhitelistPolicy::WhitelistPolicy(const std::set<std::string>& allowed_tools)
    : allowed_tools_(allowed_tools) {
}

std::string WhitelistPolicy::description() const {
    return "Only allow whitelisted tools";
}

PolicyResult WhitelistPolicy::evaluate(const PolicyContext& ctx) {
    PolicyResult result;
    
    if (allowed_tools_.empty()) {
        result.decision = PolicyDecision::ALLOW;
        return result;
    }
    
    if (allowed_tools_.find(ctx.tool_name) != allowed_tools_.end()) {
        result.decision = PolicyDecision::ALLOW;
        result.matched_rules.push_back("whitelist");
    } else {
        result.decision = PolicyDecision::DENY;
        result.reason = "Tool '" + ctx.tool_name + "' not in whitelist";
        result.matched_rules.push_back("whitelist");
    }
    
    return result;
}

// ============================================================================
// BlacklistPolicy
// ============================================================================

BlacklistPolicy::BlacklistPolicy(const std::set<std::string>& denied_tools)
    : denied_tools_(denied_tools) {
}

std::string BlacklistPolicy::description() const {
    return "Deny blacklisted tools";
}

PolicyResult BlacklistPolicy::evaluate(const PolicyContext& ctx) {
    PolicyResult result;
    
    if (denied_tools_.find(ctx.tool_name) != denied_tools_.end()) {
        result.decision = PolicyDecision::DENY;
        result.reason = "Tool '" + ctx.tool_name + "' is blacklisted";
        result.matched_rules.push_back("blacklist");
    } else {
        result.decision = PolicyDecision::ALLOW;
    }
    
    return result;
}

// ============================================================================
// DangerousCommandPolicy
// ============================================================================

DangerousCommandPolicy::DangerousCommandPolicy() {
    dangerous_patterns_ = {
        "rm -rf",
        "rm -fr",
        "mkfs",
        "dd if=",
        "> /dev/",
        ":(){ :|:& };:",  // fork bomb
        "chmod -R 777",
        "chown -R",
        "wget", "curl",  // 可能下载恶意脚本
        "eval",
        "sudo rm",
        "sudo dd"
    };
}

std::string DangerousCommandPolicy::description() const {
    return "Detect and block dangerous commands";
}

bool DangerousCommandPolicy::is_dangerous(const std::string& command) {
    for (const auto& pattern : dangerous_patterns_) {
        if (command.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

PolicyResult DangerousCommandPolicy::evaluate(const PolicyContext& ctx) {
    PolicyResult result;
    result.decision = PolicyDecision::ALLOW;
    
    // 只检查 exec 工具
    if (ctx.tool_name != "exec" && ctx.tool_name != "execute") {
        return result;
    }
    
    // 获取 command 参数
    std::string command;
    if (ctx.params.is_object() && ctx.params.has_key("command")) {
        command = ctx.params["command"].as_string();
    }
    
    if (is_dangerous(command)) {
        result.decision = PolicyDecision::REQUIRE_APPROVAL;
        result.reason = "Potentially dangerous command detected: " + command;
        result.matched_rules.push_back("dangerous_command");
    }
    
    return result;
}

// ============================================================================
// FilePathPolicy
// ============================================================================

FilePathPolicy::FilePathPolicy(const std::vector<std::string>& allowed_paths)
    : allowed_paths_(allowed_paths) {
}

std::string FilePathPolicy::description() const {
    return "Restrict file operations to allowed paths";
}

bool FilePathPolicy::is_path_allowed(const std::string& path) {
    if (allowed_paths_.empty()) {
        return true;
    }
    
    for (const auto& allowed : allowed_paths_) {
        if (path.find(allowed) == 0) {  // path 以 allowed 开头
            return true;
        }
    }
    
    return false;
}

PolicyResult FilePathPolicy::evaluate(const PolicyContext& ctx) {
    PolicyResult result;
    result.decision = PolicyDecision::ALLOW;
    
    // 只检查文件操作工具
    if (ctx.tool_name != "read_file" && 
        ctx.tool_name != "write_file" &&
        ctx.tool_name != "list_dir") {
        return result;
    }
    
    // 获取 path 参数
    std::string path;
    if (ctx.params.is_object()) {
        if (ctx.params.has_key("path")) {
            path = ctx.params["path"].as_string();
        } else if (ctx.params.has_key("file_path")) {
            path = ctx.params["file_path"].as_string();
        } else if (ctx.params.has_key("directory")) {
            path = ctx.params["directory"].as_string();
        }
    }
    
    if (!path.empty() && !is_path_allowed(path)) {
        result.decision = PolicyDecision::DENY;
        result.reason = "Access to path '" + path + "' is not allowed";
        result.matched_rules.push_back("file_path");
    }
    
    return result;
}

// ============================================================================
// RateLimitPolicy
// ============================================================================

RateLimitPolicy::RateLimitPolicy(const RateLimitConfig& config)
    : config_(config) {
}

std::string RateLimitPolicy::description() const {
    return "Rate limit tool calls";
}

void RateLimitPolicy::cleanup_old_calls(const std::string& tool_name) {
    auto& history = call_history_[tool_name];
    auto cutoff = std::chrono::system_clock::now() - 
                  std::chrono::seconds(config_.time_window_seconds);
    
    while (!history.empty() && history.front() < cutoff) {
        history.pop_front();
    }
}

PolicyResult RateLimitPolicy::evaluate(const PolicyContext& ctx) {
    cleanup_old_calls(ctx.tool_name);
    
    auto& history = call_history_[ctx.tool_name];
    
    PolicyResult result;
    if (history.size() >= static_cast<size_t>(config_.max_calls)) {
        result.decision = PolicyDecision::DENY;
        result.reason = "Rate limit exceeded for tool '" + ctx.tool_name + 
                       "' (" + std::to_string(config_.max_calls) + 
                       " calls per " + std::to_string(config_.time_window_seconds) + "s)";
        result.matched_rules.push_back("rate_limit");
    } else {
        result.decision = PolicyDecision::ALLOW;
        history.push_back(std::chrono::system_clock::now());
    }
    
    return result;
}

// ============================================================================
// RoleBasedPolicy
// ============================================================================

RoleBasedPolicy::RoleBasedPolicy(const std::vector<RolePermission>& permissions)
    : permissions_(permissions) {
}

std::string RoleBasedPolicy::description() const {
    return "Role-based access control";
}

PolicyResult RoleBasedPolicy::evaluate(const PolicyContext& ctx) {
    PolicyResult result;
    result.decision = PolicyDecision::DENY;
    result.reason = "No role permissions defined";
    
    if (ctx.user_roles.empty()) {
        result.reason = "User has no roles";
        return result;
    }
    
    // 检查每个角色的权限
    for (const auto& role_name : ctx.user_roles) {
        for (const auto& perm : permissions_) {
            if (perm.role != role_name) {
                continue;
            }
            
            // 检查黑名单
            if (perm.denied_tools.find(ctx.tool_name) != perm.denied_tools.end()) {
                result.decision = PolicyDecision::DENY;
                result.reason = "Tool '" + ctx.tool_name + "' denied for role '" + role_name + "'";
                result.matched_rules.push_back("role_based");
                return result;
            }
            
            // 检查白名单
            if (perm.allowed_tools.empty() || 
                perm.allowed_tools.find(ctx.tool_name) != perm.allowed_tools.end()) {
                result.decision = PolicyDecision::ALLOW;
                result.matched_rules.push_back("role_based");
                return result;
            }
        }
    }
    
    result.reason = "Tool '" + ctx.tool_name + "' not allowed for user roles";
    result.matched_rules.push_back("role_based");
    return result;
}

// ============================================================================
// PolicyPipeline
// ============================================================================

PolicyPipeline::PolicyPipeline(const PolicyPipelineConfig& config)
    : config_(config) {
}

void PolicyPipeline::register_policy(std::shared_ptr<ToolPolicy> policy) {
    if (!policy) {
        return;
    }
    
    policies_.push_back(policy);
    sort_policies_by_priority();
    
    log()->info("Registered policy: {} (priority: {})", 
                policy->name(), policy->priority());
}

void PolicyPipeline::remove_policy(const std::string& policy_name) {
    policies_.erase(
        std::remove_if(policies_.begin(), policies_.end(),
            [&](const auto& p) { return p->name() == policy_name; }),
        policies_.end()
    );
}

PolicyResult PolicyPipeline::evaluate(const PolicyContext& ctx) {
    PolicyResult final_result;
    final_result.decision = config_.default_decision;
    
    if (policies_.empty()) {
        return final_result;
    }
    
    // 依次评估所有策略
    for (const auto& policy : policies_) {
        auto result = policy->evaluate(ctx);
        
        if (config_.log_decisions) {
            log()->debug("Policy '{}' returned {} for tool '{}'",
                        policy->name(),
                        static_cast<int>(result.decision),
                        ctx.tool_name);
        }
        
        // 合并匹配的规则
        final_result.matched_rules.insert(
            final_result.matched_rules.end(),
            result.matched_rules.begin(),
            result.matched_rules.end()
        );
        
        // DENY 优先级最高
        if (result.decision == PolicyDecision::DENY) {
            final_result.decision = PolicyDecision::DENY;
            final_result.reason = result.reason;
            
            if (config_.short_circuit) {
                return final_result;
            }
        }
        
        // REQUIRE_APPROVAL 次优先
        if (result.decision == PolicyDecision::REQUIRE_APPROVAL) {
            if (final_result.decision != PolicyDecision::DENY) {
                final_result.decision = PolicyDecision::REQUIRE_APPROVAL;
                final_result.reason = result.reason;
            }
        }
    }
    
    return final_result;
}

std::vector<std::string> PolicyPipeline::list_policies() const {
    std::vector<std::string> names;
    for (const auto& policy : policies_) {
        names.push_back(policy->name());
    }
    return names;
}

const PolicyPipelineConfig& PolicyPipeline::get_config() const {
    return config_;
}

void PolicyPipeline::set_config(const PolicyPipelineConfig& config) {
    config_ = config;
}

void PolicyPipeline::sort_policies_by_priority() {
    std::sort(policies_.begin(), policies_.end(),
        [](const auto& a, const auto& b) {
            return a->priority() > b->priority();
        });
}

// ============================================================================
// Profiles
// ============================================================================

namespace profiles {

ToolProfile create_restricted_profile() {
    ToolProfile profile;
    profile.name = "restricted";
    profile.description = "Only allow safe read-only tools";
    
    std::set<std::string> allowed = {
        "read_file", "list_dir", "search_content"
    };
    
    profile.policies.push_back(
        std::make_shared<WhitelistPolicy>(allowed)
    );
    
    return profile;
}

ToolProfile create_standard_profile() {
    ToolProfile profile;
    profile.name = "standard";
    profile.description = "Allow most tools, restrict dangerous operations";
    
    std::set<std::string> denied = {
        "exec", "execute"  // 需要特殊审批
    };
    
    profile.policies.push_back(
        std::make_shared<BlacklistPolicy>(denied)
    );
    profile.policies.push_back(
        std::make_shared<DangerousCommandPolicy>()
    );
    
    RateLimitPolicy::RateLimitConfig rate_config;
    rate_config.max_calls = 100;
    rate_config.time_window_seconds = 60;
    profile.policies.push_back(
        std::make_shared<RateLimitPolicy>(rate_config)
    );
    
    return profile;
}

ToolProfile create_developer_profile() {
    ToolProfile profile;
    profile.name = "developer";
    profile.description = "Allow all tools, only check dangerous commands";
    
    profile.policies.push_back(
        std::make_shared<DangerousCommandPolicy>()
    );
    
    return profile;
}

ToolProfile create_production_profile() {
    ToolProfile profile;
    profile.name = "production";
    profile.description = "Strict restrictions, require approval";
    
    std::set<std::string> allowed = {
        "read_file", "list_dir", "search_content", "write_file"
    };
    
    profile.policies.push_back(
        std::make_shared<WhitelistPolicy>(allowed)
    );
    
    std::vector<std::string> allowed_paths = {
        "/workspace",
        "/tmp"
    };
    profile.policies.push_back(
        std::make_shared<FilePathPolicy>(allowed_paths)
    );
    
    RateLimitPolicy::RateLimitConfig rate_config;
    rate_config.max_calls = 50;
    rate_config.time_window_seconds = 60;
    profile.policies.push_back(
        std::make_shared<RateLimitPolicy>(rate_config)
    );
    
    return profile;
}

ToolProfile create_custom_profile(
    const std::string& name,
    const std::set<std::string>& allowed_tools,
    const std::set<std::string>& denied_tools
) {
    ToolProfile profile;
    profile.name = name;
    profile.description = "Custom profile";
    
    if (!allowed_tools.empty()) {
        profile.policies.push_back(
            std::make_shared<WhitelistPolicy>(allowed_tools)
        );
    }
    
    if (!denied_tools.empty()) {
        profile.policies.push_back(
            std::make_shared<BlacklistPolicy>(denied_tools)
        );
    }
    
    return profile;
}

}  // namespace profiles

}  // namespace tools
}  // namespace hunterclaw
