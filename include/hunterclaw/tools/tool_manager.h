#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/types.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <memory>
#include <map>
#include <vector>
#include <functional>

namespace hunterclaw {

// Forward declare MCP types (avoid pulling headers into public interface)
namespace mcp {
    struct McporterConfig;
    class McporterClient;
    class McpToolRegistry;
}

namespace tools {

// Forward declarations
class ToolHookManager;
class ToolLoopDetector;
class PolicyPipeline;
class ResultProcessingPipeline;
class ApprovalManager;
class ToolApprovalBroker;

struct ToolUpdate;
using ToolUpdateCallback = std::function<void(const ToolUpdate&)>;

// Tool Manager - 这个头文件包含 ToolManager 的完整接口定义
// 注意：Tool类的基本声明在 tool.h 中
class ToolManager : public std::enable_shared_from_this<ToolManager> {
public:
    static std::shared_ptr<ToolManager> create(const ToolContext& context);
    
    // Tool registration and retrieval
    void register_tool(std::shared_ptr<Tool> tool);
    std::shared_ptr<Tool> get_tool(const std::string& name);
    std::vector<std::string> list_tools() const;
    
    // Tool execution
    ToolResult execute_tool(const std::string& name, const core::JsonValue& params);
    ToolResult execute_tool(const std::string& name, const core::JsonValue& params, 
                           ToolUpdateCallback on_update);

    /// 执行选项。
    ///
    /// ★ before_tool_call_applied：调用方【已经】跑过 before_tool_call 策略链。
    ///
    /// ⚠⚠ 这个开关是必需的，不是优化。react_engine 出于 loop-detection 的
    ///   归属（它持有 LoopDetectionState，见 before_tool_call_policy.cc 的
    ///   Stage 2 说明）自己先跑一遍链，随后才调 execute_tool。若 execute_tool
    ///   再跑一遍，后果是：
    ///     · **审批被问两遍** —— 用户点了同意，又被弹一次
    ///     · before_tool_call 插件 hook 触发两次（副作用重复执行）
    ///     · 参数被 hook 改写两轮，第二轮基于已改写的值
    ///   审批加入后第一条变成用户可见的 bug，故必须显式关掉重复执行。
    ///
    /// ★ 默认 false：对于**没有**自己跑链的调用方（外部/未来接入方），
    ///   execute_tool 必须自带完整门禁 —— 安全默认值不能是"跳过检查"。
    struct ExecuteOptions {
        bool before_tool_call_applied = false;
    };

    ToolResult execute_tool(const std::string& name, const core::JsonValue& params,
                           ToolUpdateCallback on_update, const ExecuteOptions& options);
    
    // Policy management
    void set_policy_enabled(bool enabled);
    void set_policy_pipeline(std::shared_ptr<PolicyPipeline> pipeline);
    
    // Approval management
    void set_approval_manager(std::shared_ptr<ApprovalManager> manager);

    /// 工具审批的传输通道（gateway 侧注入）。
    ///
    /// ★ 与 ApprovalManager 是两件事:
    ///   · ApprovalManager  —— exec 命令审批的白名单/配置管理
    ///   · ToolApprovalBroker —— 工具审批的发起+等待 (plugin 形状载荷)
    ///
    /// ⚠ 为空时 before_tool_call Stage 1 按「审批面不可用」拒绝执行，
    ///   【不会】放行 —— 否则没配 broker 的部署会让所有需审批工具静默直通。
    void set_approval_broker(std::shared_ptr<ToolApprovalBroker> broker);
    ToolApprovalBroker* approval_broker() const { return approval_broker_.get(); }
    
    // Hook management
    void set_hook_manager(std::shared_ptr<ToolHookManager> manager);
    
    // Loop detection
    void set_loop_detector(std::shared_ptr<ToolLoopDetector> detector);
    
    // Result processing
    void set_result_processor(std::shared_ptr<ResultProcessingPipeline> processor);
    
    // ========================================================================
    // MCP Integration
    // ========================================================================
    
    /// 启用 MCP 集成
    /// - 根据 config.prefer_native 自动选择 NativeMcpClient (默认) 或 McporterClientImpl
    /// - 创建 McpToolRegistry 并发现所有 MCP 工具
    /// - 工具通过 McpToolAdapter 注册到当前 ToolManager
    /// @return Ok(注册的工具数) / Err(错误信息)
    core::Result<int> enable_mcp(const mcp::McporterConfig& config);
    
    /// 刷新 MCP 工具列表(重新 discover)
    /// @return Ok(注册的工具数)
    core::Result<int> refresh_mcp_tools(
        const std::vector<std::string>& server_names = {}
    );
    
    /// 获取底层 MCP 客户端(用于直接调用 MCP API)
    std::shared_ptr<mcp::McporterClient> get_mcp_client() const;
    
    /// 获取 MCP 工具注册表
    std::shared_ptr<mcp::McpToolRegistry> get_mcp_registry() const;
    
    /// 是否已启用 MCP
    bool is_mcp_enabled() const;
    
    /// Legacy: 通过 void* 设置 registry (保留向后兼容)
    void set_mcp_registry(std::shared_ptr<void> registry);
    
    // Context access
    const ToolContext& context() const { return context_; }
    void set_context(const ToolContext& context) { context_ = context; }

private:
    ToolContext context_;
    std::map<std::string, std::shared_ptr<Tool>> tools_;
    std::shared_ptr<ToolHookManager> hook_manager_;
    std::shared_ptr<ToolLoopDetector> loop_detector_;
    std::shared_ptr<PolicyPipeline> policy_pipeline_;
    std::shared_ptr<ResultProcessingPipeline> result_processor_;
    std::shared_ptr<ApprovalManager> approval_manager_;
    std::shared_ptr<ToolApprovalBroker> approval_broker_;
    
    // MCP integration
    std::shared_ptr<mcp::McporterClient> mcp_client_;
    std::shared_ptr<mcp::McpToolRegistry> mcp_registry_typed_;
    std::shared_ptr<void> mcp_registry_;  // Legacy opaque pointer
    
    bool policy_enabled_ = false;
};

} // namespace tools
} // namespace hunterclaw
