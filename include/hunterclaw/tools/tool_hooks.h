#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/tools/tool.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>

namespace hunterclaw {
namespace tools {

// ============================================================================
// Tool Hook System (100% 对齐 OpenClaw)
// ============================================================================

/// Tool Update (用于 Stream 输出)
struct ToolUpdate {
    enum class Type {
        Progress,    // 进度更新
        Partial,     // 部分结果
        Status       // 状态更新
    };
    
    Type type;
    std::string message;
    double progress = 0.0;  // 0.0 ~ 1.0
    std::optional<ToolResultContent> partial_content;
};

/// Tool 更新回调 (Stream 支持)
using ToolUpdateCallback = std::function<void(const ToolUpdate&)>;

// ============================================================================
// Tool Hook 定义
// ============================================================================

/// Before Tool Call Hook 结果
struct BeforeHookOutcome {
    bool blocked = false;
    std::string reason;
    core::JsonValue params;  // 可能被修改的参数
};

/// Before Tool Call Hook
/// @param tool_name 工具名称
/// @param params 工具参数
/// @param ctx 工具上下文
/// @return BeforeHookOutcome (blocked=true 阻止执行)
using BeforeToolCallHook = std::function<
    BeforeHookOutcome(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolContext& ctx
    )
>;

/// After Tool Call Hook
/// @param tool_name 工具名称
/// @param params 工具参数
/// @param result 工具结果
/// @param duration_ms 执行时长 (毫秒)
/// @param error 错误信息 (如果有)
using AfterToolCallHook = std::function<
    void(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolResult& result,
        double duration_ms,
        const std::string& error
    )
>;

// ============================================================================
// Tool Hook Manager (100% 对齐 OpenClaw)
// ============================================================================

/// Tool Hook 注册和执行管理器
class ToolHookManager {
public:
    ToolHookManager() = default;
    ~ToolHookManager() = default;
    
    // ========== Hook 注册 ==========
    
    /// 注册 Before Hook
    /// @param name Hook 名称 (用于调试)
    /// @param hook Hook 函数
    void register_before_hook(const std::string& name, BeforeToolCallHook hook);
    
    /// 注册 After Hook
    /// @param name Hook 名称
    /// @param hook Hook 函数
    void register_after_hook(const std::string& name, AfterToolCallHook hook);
    
    /// 取消注册 Before Hook
    void unregister_before_hook(const std::string& name);
    
    /// 取消注册 After Hook
    void unregister_after_hook(const std::string& name);
    
    /// 清除所有 Hooks
    void clear_all_hooks();
    
    // ========== Hook 执行 ==========
    
    /// 运行所有 Before Hooks
    /// @return BeforeHookOutcome (如果任何 Hook 阻止，返回 blocked=true)
    BeforeHookOutcome run_before_hooks(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolContext& ctx
    );
    
    /// 运行所有 After Hooks
    void run_after_hooks(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolResult& result,
        double duration_ms,
        const std::string& error = ""
    );
    
    // ========== 查询 ==========
    
    /// 获取已注册的 Before Hook 数量
    size_t before_hooks_count() const;
    
    /// 获取已注册的 After Hook 数量
    size_t after_hooks_count() const;
    
    /// 列出所有 Before Hook 名称
    std::vector<std::string> list_before_hooks() const;
    
    /// 列出所有 After Hook 名称
    std::vector<std::string> list_after_hooks() const;

private:
    struct HookEntry {
        std::string name;
        BeforeToolCallHook before_hook;
        AfterToolCallHook after_hook;
    };
    
    mutable std::mutex mutex_;
    std::vector<HookEntry> before_hooks_;
    std::vector<HookEntry> after_hooks_;
};

// ============================================================================
// 内置 Hook 实现
// ============================================================================

namespace builtin_hooks {

/// 参数验证 Hook (检查必需参数)
BeforeToolCallHook create_param_validation_hook();

/// 日志记录 Hook (记录所有工具调用)
AfterToolCallHook create_logging_hook();

/// 性能监控 Hook (统计工具执行时间)
AfterToolCallHook create_performance_monitoring_hook();

/// 错误收集 Hook (收集工具错误统计)
AfterToolCallHook create_error_collection_hook();

/// 审计 Hook (审计敏感工具调用)
struct AuditHookConfig {
    std::vector<std::string> sensitive_tools;  // 敏感工具列表
    std::string audit_log_path;                // 审计日志路径
};

AfterToolCallHook create_audit_hook(const AuditHookConfig& config);

/// 速率限制 Hook (限制工具调用频率)
struct RateLimitConfig {
    int max_calls_per_minute = 60;
    int max_calls_per_tool_per_minute = 10;
};

BeforeToolCallHook create_rate_limit_hook(const RateLimitConfig& config);

} // namespace builtin_hooks

// ============================================================================
// 参数规范化 (对齐 OpenClaw)
// ============================================================================

/// 参数规范化配置
struct ParamNormalizationConfig {
    bool snake_to_camel = true;   // snake_case => camelCase
    bool camel_to_snake = false;  // camelCase => snake_case
    bool trim_strings = true;     // 去除字符串前后空白
    bool remove_nulls = false;    // 移除 null 值
};

/// 规范化工具参数
/// @param params 原始参数
/// @param config 规范化配置
/// @return 规范化后的参数
core::JsonValue normalize_tool_params(
    const core::JsonValue& params,
    const ParamNormalizationConfig& config = {}
);

/// snake_case => camelCase
std::string to_camel_case(const std::string& snake_case);

/// camelCase => snake_case
std::string to_snake_case(const std::string& camel_case);

} // namespace tools
} // namespace hunterclaw
