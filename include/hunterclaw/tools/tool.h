#pragma once

#include "hunterclaw/core/types.h"
#include <atomic>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <functional>
#include <variant>
#include <nlohmann/json.hpp>

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

// ── ToolInterrupt ───────────────────────────────────────────────────────────
// 工具中断：工具执行到一半，需要【用户】给出输入才能继续。
//
// ★★ 语义与 error 截然不同，绝不能混:
//   error   = 这次调用失败了，模型应该换个做法或报告失败
//   interrupt = 这次调用【没结束】，在等人；等到了就继续，结果照样返回
//
// ⚠⚠ 为什么必须是 ToolResult 上的独立通道，而不是在工具内部自己问:
//   工具内部提问意味着"问谁、怎么投递、怎么收答复"这套逻辑要在每个
//   需要它的工具里各写一遍；而且工具拿不到 run 的投递面（on_block_reply）
//   与入站消息路由 —— 那两样都在 ReactEngine 手里。
//   把 interrupt 原样上抛，由 ReactEngine 统一处理，是唯一不重复实现的路。
//
// ⚠⚠ interrupt 有值时 ReactEngine【不会】把本次结果喂回 LLM 再转一圈。
//   否则模型会立刻代用户答"好的，确认" —— 那正是整条链路要防的事。
struct ToolInterrupt {
    /// 中断类型，决定恢复时路由到哪个 handler。
    /// 例: "remote_a2a.input_required"
    std::string kind;

    /// 恢复凭据。对 remote_a2a 就是 handle。
    /// ★ 由产出 interrupt 的工具定义含义，ReactEngine 只做透传。
    std::string resume_token;

    /// 投递给用户的追问原文。
    /// ⚠ 必须自包含（要问什么说清楚）—— 用户看到的就只有这一段。
    std::string prompt;

    /// true = 必须由【用户】答复，模型不得代答。
    /// false = 普通补充输入，模型可以代答（此时 ReactEngine 不中断，
    ///         直接把结果喂回 LLM 让它自己接着做）。
    ///
    /// ★ 这个区分是刻意保留的: 对每个普通追问都去打扰用户，会让用户
    ///   很快学会无脑回"好的"，那时真正需要人判断的那次也就失效了。
    bool requires_user_action = false;

    /// 枚举型选项（可空）。非空时提示词会列出编号供用户选。
    std::vector<std::string> options;

    /// 待确认凭据（如企微 token_id_list）。仅透传，不解析含义。
    std::vector<std::string> token_ids;
};

// Tool执行结果
struct ToolResult {
    std::vector<ToolResultContent> content;
    core::JsonValue details;  // 结构化元数据
    bool success = true;
    std::string error;

    /// 工具中断（等用户输入）。见 ToolInterrupt 说明。
    /// ★ 绝大多数工具永不设置它 —— 检查成本是一次 has_value()。
    std::optional<ToolInterrupt> interrupt;
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

    // 执行Tool（带运行取消标志）
    //
    // 🎯 严格对齐 OpenClaw 的工具执行签名:
    //      execute: async (toolCallId, args, signal) => { ... }
    //    (ask-user-tool.ts:555, common.ts:40/50 —— signal 是每个工具
    //     execute 的第三个参数，可选，工具自行决定是否使用)
    //
    // ★★ 为什么必须是"重载 + 默认转发"而不是改 1 参版签名:
    //   1 参版是纯虚函数，改签名会让【仓库里每一个工具】都编译不过。
    //   OpenClaw 的 signal 本身也是 optional —— 不关心取消的工具直接忽略。
    //   这里的默认实现转发到 1 参版，语义完全一致。
    //
    // ⚠⚠ 只有【会长时间阻塞】的工具需要覆写。不覆写的后果不是编译错误，
    //   而是 chat.abort 之后该工具仍然挂满自己的超时 ——
    //   对 remote_a2a 的用户提问来说是 900s，且这段时间占着 main lane
    //   的唯一槽位（command_queue.cc:60-64 固定 lane 单 WorkerThread，
    //   max_concurrent 对固定 lane 无效）。用户点了停止却要等 15 分钟。
    //
    // @param aborted 运行取消标志。非空且被置位时工具应尽快返回。
    //        ★ 用 `const std::atomic<bool>*` 而非 bool&:
    //          调用方可能没有取消源（单测、后台线程），nullptr 表达
    //          "本次调用不可取消"比传一个永远为 false 的引用更明确。
    virtual ToolResult execute(const core::JsonValue& params,
                               const std::atomic<bool>* aborted) {
        (void)aborted;
        return execute(params);
    }
    
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

    /// 配置原文（未被上面窄化字段覆盖的部分仍可读取）
    ///
    /// Aligned with openclaw: 真源工具直接持有完整 `OpenClawConfig`。
    /// HunterClaw 把配置拆成了具名字段，导致 `streaming.progressCommentary`、
    /// `acp.projection.tagVisibility` 这类字段无处可取。此字段用于透传原文，
    /// 目前由 sessions_tool → AcpSpawnConfig::raw_config → ACP 父流 relay 消费。
    /// 为空时下游走各自默认值。
    nlohmann::json raw_config;

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
