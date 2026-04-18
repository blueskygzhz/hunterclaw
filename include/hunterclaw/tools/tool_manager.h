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
    
    // Policy management
    void set_policy_enabled(bool enabled);
    void set_policy_pipeline(std::shared_ptr<PolicyPipeline> pipeline);
    
    // Approval management
    void set_approval_manager(std::shared_ptr<ApprovalManager> manager);
    
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
    
    // MCP integration
    std::shared_ptr<mcp::McporterClient> mcp_client_;
    std::shared_ptr<mcp::McpToolRegistry> mcp_registry_typed_;
    std::shared_ptr<void> mcp_registry_;  // Legacy opaque pointer
    
    bool policy_enabled_ = false;
};

} // namespace tools
} // namespace hunterclaw
