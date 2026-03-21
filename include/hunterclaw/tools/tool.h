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
    /// Stable run identifier for this agent invocation
    /// Aligned with OpenClaw PluginHookToolContext.runId
    std::string run_id;
    /// Ephemeral session UUID — regenerated on /new and /reset
    std::string session_id;
    std::map<std::string, std::string> metadata;

    // ===== Sandbox 集成 (100% 对齐 OpenClaw) =====
    std::shared_ptr<hunterclaw::sandbox::SandboxManager> sandbox_manager;
    std::string sandbox_id;  // 当前 Sandbox ID (如果启用)

    // ===== Exec 配置 (对齐 OpenClaw ExecToolDefaults) =====
    // tools.exec.host: "sandbox" | "gateway" | "node"
    std::string exec_host = "sandbox";
    // tools.exec.security: "deny" | "allowlist" | "full"
    std::string exec_security = "allowlist";
    // tools.exec.ask: "off" | "on-miss" | "always"
    std::string exec_ask = "on-miss";
    // tools.elevated.defaultLevel: "off" | "ask" | "full"
    std::string exec_elevated_level = "off";
    // tools.exec.notifyOnExit
    bool exec_notify_on_exit = true;
    // tools.exec.notifyOnExitEmptySuccess
    bool exec_notify_on_exit_empty_success = false;
    // tools.exec.backgroundMs (default 10000)
    int64_t exec_background_ms = 10000;
    // tools.exec.timeoutSec (default 1800)
    int64_t exec_timeout_sec = 1800;
    // Channel/provider context for exec approval turn-source
    std::string message_provider;
    std::string current_channel_id;
    std::string account_id;
    std::string current_thread_ts;
    std::string scope_key;
};

// Forward declarations
class ToolManager;
class ToolHookManager;
class ToolLoopDetector;
class PolicyPipeline;
class ResultProcessingPipeline;
struct ToolUpdate;
using ToolUpdateCallback = std::function<void(const ToolUpdate&)>;

// Note: ToolManager 的完整定义在 tool_manager.h 中

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
