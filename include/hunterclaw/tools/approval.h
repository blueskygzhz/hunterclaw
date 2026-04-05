// HunterClaw - High-Performance AI Agent Platform (C++)
// Copyright (c) 2025 HunterClaw Team
// 
// Tool Approval System (100% 对齐 OpenClaw)
// 实现 human-in-the-loop 功能，支持命令执行前的人工审批

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <map>
#include <set>
#include <mutex>
#include <condition_variable>
#include "hunterclaw/core/types.h"
#include "hunterclaw/core/json.h"

namespace hunterclaw {
namespace tools {

// ============================================================================
// Approval Configuration Types (对齐 OpenClaw)
// ============================================================================

/// 安全模式
enum class ExecSecurity {
    DENY,           // 拒绝所有命令
    ALLOWLIST,      // 白名单模式
    FULL            // 完全信任（不限制）
};

/// 询问策略
enum class ExecAsk {
    OFF,            // 关闭审批
    ON_MISS,        // 白名单未命中时询问
    ALWAYS          // 总是询问
};

/// 审批决策
enum class ApprovalDecision {
    PENDING,        // 等待决策
    ALLOW_ONCE,     // 允许一次
    ALLOW_ALWAYS,   // 总是允许（添加到白名单）
    DENY,           // 拒绝
    TIMEOUT         // 超时
};

/// 白名单条目
struct AllowlistEntry {
    std::string id;             // UUID
    std::string pattern;        // Glob 模式 (e.g., "git *", "npm install")
    int64_t created_at_ms;      // 创建时间
    std::string created_by;     // 创建者
};

/// Agent 审批配置
struct AgentApprovalConfig {
    ExecSecurity security = ExecSecurity::ALLOWLIST;
    ExecAsk ask = ExecAsk::ON_MISS;
    ExecSecurity ask_fallback = ExecSecurity::DENY;  // 对齐 OpenClaw: askFallback 类型为 ExecSecurity, 默认 "deny"
    bool auto_allow_skills = false;          // 自动允许技能 CLI
    std::vector<AllowlistEntry> allowlist;
};

/// 全局审批配置
struct ApprovalConfig {
    int version = 1;
    AgentApprovalConfig defaults;
    std::map<std::string, AgentApprovalConfig> agents;  // 按 agent_id 配置
};

// ============================================================================
// Approval Request & Response (对齐 OpenClaw RPC)
// ============================================================================

/// 审批请求
struct ApprovalRequest {
    std::string id;                         // UUID
    std::string command;                    // 完整命令字符串
    std::vector<std::string> command_argv;  // 命令参数数组
    std::string cwd;                        // 工作目录
    std::string host;                       // 执行主机 (gateway/node)
    ExecSecurity security;                  // 安全模式
    ExecAsk ask;                            // 询问策略
    std::string agent_id;                   // Agent ID
    std::string session_key;                // 会话密钥
    int64_t timeout_ms = 120000;            // 超时时间（默认2分钟）
    bool two_phase = true;                  // 两阶段提交
    int64_t created_at_ms;                  // 创建时间
    int64_t expires_at_ms;                  // 过期时间
    
    // 审批绑定（防篡改）
    std::string cwd_snapshot_hash;          // 工作目录快照哈希
    std::string env_hash;                   // 环境变量哈希
};

/// 审批响应
struct ApprovalResponse {
    std::string id;                         // 请求 ID
    ApprovalDecision decision;              // 决策结果
    std::string resolved_by;                // 决策者
    int64_t resolved_at_ms;                 // 决策时间
    std::string reason;                     // 拒绝原因（如果适用）
};

/// 审批状态
enum class ApprovalStatus {
    ACCEPTED,       // 已接受（等待决策）
    RESOLVED,       // 已决策
    EXPIRED,        // 已过期
    CANCELLED       // 已取消
};

// ============================================================================
// Approval Manager (核心管理器)
// ============================================================================

/// 审批更新回调
using ApprovalUpdateCallback = std::function<void(const ApprovalRequest&, ApprovalStatus)>;

/// 审批管理器（线程安全）
class ApprovalManager {
public:
    static std::shared_ptr<ApprovalManager> create(const std::string& config_path = "");
    
    ApprovalManager(const std::string& config_path);
    ~ApprovalManager();
    
    // ===== 配置管理 =====
    
    /// 加载配置文件
    core::Result<void> load_config();
    
    /// 保存配置文件
    core::Result<void> save_config();
    
    /// 获取 Agent 配置
    AgentApprovalConfig get_agent_config(const std::string& agent_id) const;
    
    /// 设置 Agent 配置
    void set_agent_config(const std::string& agent_id, const AgentApprovalConfig& config);
    
    // ===== 白名单管理 =====
    
    /// 添加白名单条目
    void add_allowlist_entry(const std::string& agent_id, const AllowlistEntry& entry);
    
    /// 移除白名单条目
    void remove_allowlist_entry(const std::string& agent_id, const std::string& entry_id);
    
    /// 检查命令是否在白名单中
    bool is_allowlisted(const std::string& agent_id, const std::string& command) const;
    
    /// 匹配白名单模式
    std::vector<AllowlistEntry> match_allowlist(
        const std::string& agent_id, 
        const std::string& command
    ) const;
    
    // ===== 审批流程 =====
    
    /// 请求审批（两阶段提交第一阶段）
    core::Result<ApprovalRequest> request_approval(const ApprovalRequest& request);
    
    /// 等待审批决策（阻塞）
    core::Result<ApprovalResponse> wait_decision(const std::string& request_id);
    
    /// 等待审批决策（非阻塞，超时）
    core::Result<ApprovalResponse> wait_decision_timeout(
        const std::string& request_id,
        int64_t timeout_ms
    );
    
    /// 提交审批决策（UI/渠道调用）
    core::Result<void> resolve_approval(
        const std::string& request_id,
        ApprovalDecision decision,
        const std::string& resolved_by = ""
    );
    
    /// 取消审批请求
    core::Result<void> cancel_approval(const std::string& request_id);
    
    /// 获取待审批列表
    std::vector<ApprovalRequest> get_pending_approvals() const;
    
    /// 获取审批状态
    core::Result<ApprovalStatus> get_approval_status(const std::string& request_id) const;
    
    // ===== 策略评估 =====
    
    /// 评估是否需要审批
    /// 对齐 OpenClaw requiresExecApproval({ ask, security, analysisOk, allowlistSatisfied })
    bool requires_approval(
        const std::string& agent_id,
        const std::string& command,
        const AgentApprovalConfig& config
    ) const;
    
    /// 严格对齐 OpenClaw requiresExecApproval() 的静态版本
    /// 用于 gateway/exec 工具层直接调用
    static bool requires_exec_approval(
        ExecAsk ask,
        ExecSecurity security,
        bool analysis_ok,
        bool allowlist_satisfied
    );
    
    /// 自动决策（根据配置和白名单）
    core::Result<ApprovalDecision> auto_decide(
        const std::string& agent_id,
        const std::string& command
    ) const;
    
    // ===== 回调管理 =====
    
    /// 注册审批更新回调（用于 UI 通知）
    void register_callback(ApprovalUpdateCallback callback);
    
    /// 清理过期的审批请求
    void cleanup_expired();

private:
    std::string config_path_;
    ApprovalConfig config_;
    
    // 审批请求存储
    mutable std::mutex mutex_;
    std::map<std::string, ApprovalRequest> pending_requests_;
    std::map<std::string, ApprovalResponse> resolved_responses_;
    std::map<std::string, std::condition_variable> cv_map_;
    
    // 回调
    std::vector<ApprovalUpdateCallback> callbacks_;
    
    // 内部方法
    void notify_callbacks(const ApprovalRequest& request, ApprovalStatus status);
    bool glob_match(const std::string& pattern, const std::string& text) const;
    std::string default_config_path() const;
};

// ============================================================================
// Approval Integration Helper
// ============================================================================

/// 构建审批请求的辅助函数
ApprovalRequest build_approval_request(
    const std::string& command,
    const std::vector<std::string>& argv,
    const std::string& cwd,
    const std::string& agent_id,
    const std::string& session_key,
    const AgentApprovalConfig& config
);

/// 从命令生成白名单模式
std::vector<std::string> generate_allowlist_patterns(
    const std::string& command,
    const std::vector<std::string>& argv,
    const std::string& cwd
);

/// 检查是否为敏感命令
bool is_sensitive_command(const std::string& command);

/// 检查是否为 shell 包装器
bool is_shell_wrapper(const std::vector<std::string>& argv);

// ============================================================================
// Policy Merge Functions (对齐 OpenClaw exec-approvals.ts)
// ============================================================================

/// 取更严格的安全策略 (deny < allowlist < full)
/// 对齐 OpenClaw: minSecurity(a, b)
ExecSecurity min_security(ExecSecurity a, ExecSecurity b);

/// 取更严格的询问策略 (off < on-miss < always)
/// 对齐 OpenClaw: maxAsk(a, b)
ExecAsk max_ask(ExecAsk a, ExecAsk b);

// ============================================================================
// Approval Decision Resolution (对齐 OpenClaw bash-tools.exec-host-shared.ts)
// ============================================================================

/// 解析基础审批决策结果
/// 对齐 OpenClaw: resolveBaseExecApprovalDecision()
struct BaseApprovalDecisionResult {
    bool approved_by_ask = false;
    std::string denied_reason;     // 空字符串 = 未拒绝
    bool timed_out = false;
};

BaseApprovalDecisionResult resolve_base_exec_approval_decision(
    const std::string& decision,          // "allow-once" / "allow-always" / "deny" / ""(null)
    ExecSecurity ask_fallback,
    bool obfuscation_detected
);

}  // namespace tools
}  // namespace hunterclaw
