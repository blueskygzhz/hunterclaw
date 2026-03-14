#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <memory>
#include <map>
#include <vector>
#include <functional>

namespace hunterclaw {
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
    
    // MCP integration
    void set_mcp_registry(std::shared_ptr<void> registry);  // void* to avoid circular dependency
    
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
    std::shared_ptr<void> mcp_registry_;  // Opaque pointer to MCP registry
    bool policy_enabled_ = false;
};

} // namespace tools
} // namespace hunterclaw
