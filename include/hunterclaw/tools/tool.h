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
namespace infra {
    class HttpClient;
}
namespace gateway {
    class GatewayClient;
}
namespace nodes {
    class NodeRegistry;
}
namespace llm {
    class LLMClient;
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
    // 🎯 对齐 OpenClaw needsApproval。
    // ⚠ 此虚函数曾长期【零消费】—— 声明 true 的工具从不审批。
    //   现由 before_tool_call 链的 Stage 1 消费
    //   (before_tool_call_policy.cc 的 run_approval_gate)。
    virtual bool requires_approval() const { return false; }

    // 审批模式覆盖（"request" / "report" / "deny" / "defer"）。
    //
    // ★ 返回空串表示"用宿主默认"（当前默认 Request，对齐 OpenClaw
    //   resolveBeforeToolCallApprovalOutcome 的 fall-through 分支）。
    //
    // ★ 需要 "defer" 的典型场景：审批的收敛方【不是】本次工具调用本身，
    //   例如"用户要在外部页面上点确认"。Request 会独占会话直到超时，
    //   Defer 则放行并把凭据交给登记表稍后收敛。
    //
    // ⚠ 用字符串而非 enum：tool.h 被极广泛包含，引入
    //   before_tool_call_policy.h 的 enum 会把审批类型拖进所有工具的 TU。
    //   字符串在 Stage 1 入口解析一次即可。
    virtual std::string approval_mode() const { return {}; }

    // 限定本次审批允许的决策集合（"allow-once" / "allow-always" / "deny"）。
    //
    // ★ 返回空表示用默认全集。
    // ⚠ 用途：某些操作【不该】允许 allow-always ——
    //   "确认转账 5000 元"可以批一次，永久授权绝不行。
    //   若不限定，用户一次误点 allow-always 会让后续同类操作全部免审。
    virtual std::vector<std::string> approval_allowed_decisions() const { return {}; }

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

    // 🎯 对齐 OpenClaw senderIsOwner: 调用者是否具有 owner (operator.admin) 身份。
    // 非 owner 调用 owner_only 工具时, before_tool_call 门禁会 veto。
    // 默认 false (最小权限); 由gateway/agent 上下文按 scope 显式置位。
    bool sender_is_owner = false;

    // ===== 依赖注入 (用于工具注册) =====
    std::shared_ptr<infra::HttpClient>    http_client;
    std::shared_ptr<gateway::GatewayClient> gateway_client;
    std::shared_ptr<nodes::NodeRegistry>  node_registry;
    std::shared_ptr<llm::LLMClient>       llm_client;
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
