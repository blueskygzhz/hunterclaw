#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <variant>

// Forward declarations for MCP types
namespace hunterclaw {
namespace mcp {
    struct McporterConfig;
    class McporterClient;
    class McpToolRegistry;
}
namespace sandbox {
    class SandboxManager;
    class Sandbox;
}
}

namespace hunterclaw {
namespace tools {

// Tool结果内容类型
struct ToolResultContent {
    enum class Type {
        Text,
        Image,
        Json
    };
    
    Type type;
    std::string text;  // type=Text/Json时使用
    std::string data;  // type=Image时使用(base64)
    std::string mime_type;  // type=Image时使用
};

// Tool执行结果
struct ToolResult {
    std::vector<ToolResultContent> content;
    core::JsonValue details;  // 结构化元数据
    bool success = true;
    std::string error;
};

// Tool参数辅助函数
class ToolParams {
public:
    explicit ToolParams(const core::JsonValue& params) : params_(params) {}
    
    // 读取字符串参数
    std::string read_string(const std::string& key, bool required = false, 
                           const std::string& default_val = "") const;
    
    // 读取数字参数
    double read_number(const std::string& key, bool required = false, 
                      double default_val = 0.0) const;
    
    // 读取整数参数
    int64_t read_int(const std::string& key, bool required = false, 
                    int64_t default_val = 0) const;
    
    // 读取布尔参数
    bool read_bool(const std::string& key, bool required = false, 
                  bool default_val = false) const;
    
    // 读取字符串数组
    std::vector<std::string> read_string_array(const std::string& key, 
                                               bool required = false) const;
    
    // 检查参数是否存在
    bool has(const std::string& key) const;
    
    // 获取原始JSON
    const core::JsonValue& raw() const { return params_; }

private:
    const core::JsonValue& params_;
};

// Tool定义
class Tool {
public:
    struct Schema {
        std::string type;  // "object"
        std::string description;
        std::map<std::string, core::JsonValue> properties;
        std::vector<std::string> required;
    };
    
    virtual ~Tool() = default;
    
    // Tool名称
    virtual std::string name() const = 0;
    
    // Tool描述
    virtual std::string description() const = 0;
    
    // 参数Schema (JSON Schema)
    virtual Schema parameters() const = 0;
    
    // 执行Tool
    virtual ToolResult execute(const core::JsonValue& params) = 0;
    
    // 是否需要审批
    virtual bool requires_approval() const { return false; }
    
    // 是否仅Owner可用
    virtual bool owner_only() const { return false; }
};

// Tool上下文
struct ToolContext {
    std::string workspace_dir;
    std::string agent_id;
    std::string session_key;
    std::map<std::string, std::string> metadata;
    
    // ===== Sandbox 集成 (100% 对齐 OpenClaw) =====
    std::shared_ptr<hunterclaw::sandbox::SandboxManager> sandbox_manager;
    std::string sandbox_id;  // 当前 Sandbox ID (如果启用)
};

class ToolHookManager;
class ToolLoopDetector;
class PolicyPipeline;
class ResultProcessingPipeline;
struct ToolUpdate;
using ToolUpdateCallback = std::function<void(const ToolUpdate&)>;

// Tool Manager
class ToolManager {
public:
    static std::shared_ptr<ToolManager> create(const ToolContext& context);
    
    // 注册Tool
    void register_tool(std::shared_ptr<Tool> tool);
    
    // 获取Tool
    std::shared_ptr<Tool> get_tool(const std::string& name);
    
    // 列出所有Tools
    std::vector<std::string> list_tools() const;
    
    // 执行Tool
    ToolResult execute_tool(const std::string& name, const core::JsonValue& params);
    
    // 执行Tool (带 Stream 回调)
    ToolResult execute_tool(
        const std::string& name,
        const core::JsonValue& params,
        ToolUpdateCallback on_update
    );
    
    // 转换为LLM格式
    struct LLMToolDefinition {
        std::string name;
        std::string description;
        std::string parameters;  // JSON Schema字符串
    };
    std::vector<LLMToolDefinition> to_llm_format() const;
    
    // ========== MCP 集成 ==========
    
    /// 启用 MCP 工具发现
    void enable_mcp(const mcp::McporterConfig& config);
    
    /// 刷新 MCP 工具列表
    core::Result<int> refresh_mcp_tools();
    
    /// 获取 MCP client (如果已启用)
    std::shared_ptr<mcp::McporterClient> get_mcp_client();
    
    /// 检查 MCP 是否已启用
    bool is_mcp_enabled() const;
    
    // ========== Hook 系统 (100% 对齐 OpenClaw) ==========
    
    /// 获取 Hook Manager
    std::shared_ptr<ToolHookManager> get_hook_manager();
    
    /// 启用内置 Hooks
    void enable_builtin_hooks(bool logging = true, bool performance = true);
    
    // ========== 循环检测 (100% 对齐 OpenClaw) ==========
    
    /// 获取循环检测器
    std::shared_ptr<ToolLoopDetector> get_loop_detector();
    
    /// 启用循环检测
    void enable_loop_detection(bool enable = true);
    
    // ========== 策略管道 (100% 对齐 OpenClaw) ==========
    
    /// 获取策略管道
    std::shared_ptr<PolicyPipeline> get_policy_pipeline();
    
    /// 启用策略管道
    void enable_policy_pipeline(bool enable = true);
    
    /// 应用 Profile (策略预设)
    void apply_profile(const std::string& profile_name);
    
    // ========== 结果处理 (100% 对齐 OpenClaw) ==========
    
    /// 获取结果处理管道
    std::shared_ptr<ResultProcessingPipeline> get_result_pipeline();
    
    /// 启用结果规范化
    void enable_result_normalization(bool enable = true);

private:
    ToolContext context_;
    std::map<std::string, std::shared_ptr<Tool>> tools_;
    
    // MCP 支持
    std::shared_ptr<hunterclaw::mcp::McporterClient> mcp_client_;
    std::shared_ptr<hunterclaw::mcp::McpToolRegistry> mcp_registry_;
    
    // Hook 支持
    std::shared_ptr<ToolHookManager> hook_manager_;
    
    // 循环检测
    std::shared_ptr<ToolLoopDetector> loop_detector_;
    bool loop_detection_enabled_ = false;
    
    // 策略管道
    std::shared_ptr<PolicyPipeline> policy_pipeline_;
    bool policy_enabled_ = false;
    
    // 结果处理
    std::shared_ptr<ResultProcessingPipeline> result_pipeline_;
    bool result_normalization_enabled_ = false;
};

// 内置Tools工厂
class BuiltinTools {
public:
    // Bash执行工具
    static std::shared_ptr<Tool> create_exec_tool(const ToolContext& context);
    
    // 文件读取工具
    static std::shared_ptr<Tool> create_read_file_tool(const ToolContext& context);
    
    // 文件写入工具
    static std::shared_ptr<Tool> create_write_file_tool(const ToolContext& context);
    
    // 文件列表工具
    static std::shared_ptr<Tool> create_list_dir_tool(const ToolContext& context);
    
    // 文件搜索工具
    static std::shared_ptr<Tool> create_search_files_tool(const ToolContext& context);
    
    // 内容搜索工具(grep)
    static std::shared_ptr<Tool> create_search_content_tool(const ToolContext& context);
    
    // Web搜索工具
    static std::shared_ptr<Tool> create_web_search_tool(const ToolContext& context);
    
    // 注册所有内置工具
    static void register_all(ToolManager& manager, const ToolContext& context);
};

// Tool结果辅助函数
namespace tool_result {
    // 文本结果
    ToolResult text(const std::string& content);
    
    // JSON结果
    ToolResult json(const core::JsonValue& data);
    
    // 图片结果
    ToolResult image(const std::string& base64_data, const std::string& mime_type);
    
    // 错误结果
    ToolResult error(const std::string& message);
}

} // namespace tools
} // namespace hunterclaw
