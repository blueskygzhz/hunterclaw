// HunterClaw - High-Performance AI Agent Platform (C++)
// Copyright (c) 2025 HunterClaw Team
// 
// Tool 策略管道 (100% 对齐 OpenClaw)
// 提供细粒度的工具权限控制和策略管理

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <set>
#include <deque>
#include <map>
#include <chrono>
#include "hunterclaw/core/types.h"
#include "hunterclaw/tools/tool.h"

namespace hunterclaw {
namespace tools {

/// 策略决策结果
enum class PolicyDecision {
    ALLOW,      // 允许执行
    DENY,       // 拒绝执行
    REQUIRE_APPROVAL  // 需要用户批准
};

/// 策略评估上下文
struct PolicyContext {
    std::string tool_name;
    core::JsonValue params;
    ToolContext tool_context;
    
    // 额外上下文
    std::string user_id;
    std::string session_id;
    std::vector<std::string> user_roles;
};

/// 策略评估结果
struct PolicyResult {
    PolicyDecision decision;
    std::string reason;
    std::vector<std::string> matched_rules;
};

/// Tool 策略接口
class ToolPolicy {
public:
    virtual ~ToolPolicy() = default;
    
    /// 策略名称
    virtual std::string name() const = 0;
    
    /// 策略描述
    virtual std::string description() const = 0;
    
    /// 评估策略
    virtual PolicyResult evaluate(const PolicyContext& ctx) = 0;
    
    /// 策略优先级 (数字越大优先级越高)
    virtual int priority() const { return 0; }
};

// ============================================================================
// 内置策略
// ============================================================================

/// 工具白名单策略
class WhitelistPolicy : public ToolPolicy {
public:
    explicit WhitelistPolicy(const std::set<std::string>& allowed_tools);
    
    std::string name() const override { return "whitelist"; }
    std::string description() const override;
    PolicyResult evaluate(const PolicyContext& ctx) override;
    int priority() const override { return 100; }

private:
    std::set<std::string> allowed_tools_;
};

/// 工具黑名单策略
class BlacklistPolicy : public ToolPolicy {
public:
    explicit BlacklistPolicy(const std::set<std::string>& denied_tools);
    
    std::string name() const override { return "blacklist"; }
    std::string description() const override;
    PolicyResult evaluate(const PolicyContext& ctx) override;
    int priority() const override { return 90; }

private:
    std::set<std::string> denied_tools_;
};

/// 危险命令检测策略
class DangerousCommandPolicy : public ToolPolicy {
public:
    DangerousCommandPolicy();
    
    std::string name() const override { return "dangerous_command"; }
    std::string description() const override;
    PolicyResult evaluate(const PolicyContext& ctx) override;
    int priority() const override { return 80; }

private:
    std::vector<std::string> dangerous_patterns_;
    bool is_dangerous(const std::string& command);
};

/// 文件路径限制策略
class FilePathPolicy : public ToolPolicy {
public:
    explicit FilePathPolicy(const std::vector<std::string>& allowed_paths);
    
    std::string name() const override { return "file_path"; }
    std::string description() const override;
    PolicyResult evaluate(const PolicyContext& ctx) override;
    int priority() const override { return 70; }

private:
    std::vector<std::string> allowed_paths_;
    bool is_path_allowed(const std::string& path);
};

/// 速率限制策略
class RateLimitPolicy : public ToolPolicy {
public:
    struct RateLimitConfig {
        int max_calls = 100;
        int time_window_seconds = 60;
    };
    
    explicit RateLimitPolicy(const RateLimitConfig& config);
    
    std::string name() const override { return "rate_limit"; }
    std::string description() const override;
    PolicyResult evaluate(const PolicyContext& ctx) override;
    int priority() const override { return 60; }

private:
    RateLimitConfig config_;
    std::map<std::string, std::deque<std::chrono::system_clock::time_point>> call_history_;
    void cleanup_old_calls(const std::string& tool_name);
};

/// 角色权限策略
class RoleBasedPolicy : public ToolPolicy {
public:
    struct RolePermission {
        std::string role;
        std::set<std::string> allowed_tools;
        std::set<std::string> denied_tools;
    };
    
    explicit RoleBasedPolicy(const std::vector<RolePermission>& permissions);
    
    std::string name() const override { return "role_based"; }
    std::string description() const override;
    PolicyResult evaluate(const PolicyContext& ctx) override;
    int priority() const override { return 50; }

private:
    std::vector<RolePermission> permissions_;
};

// ============================================================================
// 策略管道
// ============================================================================

/// Tool 策略管道配置
struct PolicyPipelineConfig {
    // 默认决策 (当所有策略都返回 ALLOW 时)
    PolicyDecision default_decision = PolicyDecision::ALLOW;
    
    // 是否短路评估 (遇到 DENY 立即停止)
    bool short_circuit = true;
    
    // 是否记录策略决策
    bool log_decisions = true;
};

/// Tool 策略管道 (100% 对齐 OpenClaw)
class PolicyPipeline {
public:
    explicit PolicyPipeline(const PolicyPipelineConfig& config = PolicyPipelineConfig{});
    
    /// 注册策略
    void register_policy(std::shared_ptr<ToolPolicy> policy);
    
    /// 移除策略
    void remove_policy(const std::string& policy_name);
    
    /// 评估所有策略
    PolicyResult evaluate(const PolicyContext& ctx);
    
    /// 列出所有策略
    std::vector<std::string> list_policies() const;
    
    /// 获取配置
    const PolicyPipelineConfig& get_config() const;
    
    /// 设置配置
    void set_config(const PolicyPipelineConfig& config);

private:
    PolicyPipelineConfig config_;
    std::vector<std::shared_ptr<ToolPolicy>> policies_;
    
    void sort_policies_by_priority();
};

// ============================================================================
// Profile 系统 (预定义策略组合)
// ============================================================================

/// Tool Profile (策略组合)
struct ToolProfile {
    std::string name;
    std::string description;
    std::vector<std::shared_ptr<ToolPolicy>> policies;
};

/// Profile 工厂
namespace profiles {

/// 受限模式 (只允许安全的只读工具)
ToolProfile create_restricted_profile();

/// 标准模式 (允许大部分工具，限制危险操作)
ToolProfile create_standard_profile();

/// 开发模式 (允许所有工具，只检测危险命令)
ToolProfile create_developer_profile();

/// 生产模式 (严格限制，需要审批)
ToolProfile create_production_profile();

/// 自定义 Profile
ToolProfile create_custom_profile(
    const std::string& name,
    const std::set<std::string>& allowed_tools,
    const std::set<std::string>& denied_tools
);

}  // namespace profiles

}  // namespace tools
}  // namespace hunterclaw
