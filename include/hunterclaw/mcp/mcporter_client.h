#pragma once

// ============================================================================
// MCP Client 接口定义
// ============================================================================
//
// 历史背景:
//   最初此接口通过外部 `mcporter` CLI 桥接(McporterClientImpl),故保留名称。
//   自 2026-04 起,HunterClaw 使用纯 C++ 原生客户端(NativeMcpClient),
//   不再依赖外部 mcporter 工具。接口名保留以避免大范围重命名。
//
// 唯一实现: hunterclaw::mcp::NativeMcpClient
//   - Stdio 传输: fork 子进程 + JSON-RPC over stdin/stdout
//   - Http 传输: 基于 infra::HttpClient (SSE + StreamableHttp)
//   - MCP 协议: initialize → tools/list → tools/call
// ============================================================================

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <map>

namespace hunterclaw {
namespace mcp {

// MCP 客户端配置
struct McporterConfig {
    bool enabled = false;
    std::string server_name = "default";       // 默认 MCP server 名称
    bool start_daemon = true;                  // 保留:启动时预连接所有 server
    int timeout_ms = 30000;                    // 工具调用超时时间 (30秒)
    std::vector<std::string> allowed_servers;  // 白名单 (空=全部)
};

// MCP 工具调用参数
struct McporterCallArgs {
    std::string server_and_tool;          // e.g. "linear.list_issues"
    core::JsonValue args;                 // 工具参数
    std::string output_format = "json";   // json/text/pretty
    int timeout_ms = 0;                   // 0=使用默认
};

// MCP 工具调用结果
struct McporterResult {
    bool success = false;
    std::string stdout_output;
    std::string stderr_output;
    core::JsonValue structured_content;   // 解析后的 JSON
    std::string error;
    int exit_code = -1;
    // 🎯 对齐 OpenClaw CallToolResult：保留原始 MCP content 数组与 isError，
    //   供 toAgentToolResult 规范化（结构化优先、保留非文本块、无内容回退）。
    core::JsonValue content;// MCP content[] 原样（Array<{type,text,...}>）
    bool is_error = false;                // MCP isError
    bool has_structured_content = false;  // 区分"无 structuredContent"与"值为 null"
};

// MCP 工具定义
struct McpToolDefinition {
    std::string name;           // 完整名称 "server.tool"
    std::string description;
    std::string server_name;
    std::string tool_name;
    core::JsonValue input_schema;  // JSON Schema
};

// MCP Server 信息
struct McpServerInfo {
    std::string name;
    std::string command;
    std::string lifecycle;  // "keep-alive" / "per-request"
    core::JsonValue metadata;
};

/// MCP Client 接口 — 唯一实现为 NativeMcpClient
class McporterClient {
public:
    virtual ~McporterClient() = default;

    // ========== Daemon 管理 ==========
    // 在 NativeMcpClient 下,"daemon" 概念变为"连接池预热"

    /// 预热所有已配置 server 的连接
    virtual core::Result<void> start_daemon() = 0;

    /// 关闭所有活动连接
    virtual core::Result<void> stop_daemon() = 0;

    /// 检查是否有任何活动连接
    virtual core::Result<bool> is_daemon_running() = 0;

    /// 重置所有连接
    virtual core::Result<void> restart_daemon() = 0;

    /// 确保客户端已就绪 (线程安全, 幂等)
    virtual core::Result<void> ensure_daemon_started() = 0;

    // ========== 工具发现 ==========

    /// 列出所有已配置的 MCP servers
    virtual core::Result<std::vector<McpServerInfo>> list_servers() = 0;

    /// 列出指定 server 的所有 tools
    virtual core::Result<std::vector<McpToolDefinition>> list_tools(
        const std::string& server_name,
        bool with_schema = true
    ) = 0;

    /// 获取所有可用的 MCP tools (跨所有 servers)
    /// 如果设置了 allowed_servers, 只返回白名单中的
    virtual core::Result<std::vector<McpToolDefinition>> discover_all_tools() = 0;

    // ========== 工具调用 ==========

    /// 调用 MCP 工具
    /// @param server_and_tool 格式: "server.tool" 或 "server:tool"
    /// @param args 工具参数 (JSON)
    virtual core::Result<McporterResult> call_tool(
        const std::string& server_and_tool,
        const core::JsonValue& args
    ) = 0;

    /// 批量调用 (并发)
    virtual core::Result<std::vector<McporterResult>> call_tools(
        const std::vector<McporterCallArgs>& calls
    ) = 0;

    // ========== Resources / Prompts (🎯 对齐 OpenClaw utility tools) ==========
    // 默认返回 unsupported，仅 NativeMcpClient 实现（走 MCP resources/*、prompts/*）。

    /// resources/list（有界分页聚合）→ {resources:[...]}
    virtual core::Result<core::JsonValue> list_resources(const std::string& /*server_name*/) {
        return core::Result<core::JsonValue>::Err("resources not supported by this client");
    }
    /// resources/read →原始结果
    virtual core::Result<core::JsonValue> read_resource(
        const std::string& /*server_name*/, const std::string& /*uri*/) {
        return core::Result<core::JsonValue>::Err("resources not supported by this client");
    }
    /// prompts/list（有界分页聚合）→ {prompts:[...]}
    virtual core::Result<core::JsonValue> list_prompts(const std::string& /*server_name*/) {
        return core::Result<core::JsonValue>::Err("prompts not supported by this client");
    }
    /// prompts/get → 原始结果
    virtual core::Result<core::JsonValue> get_prompt(
        const std::string& /*server_name*/, const std::string& /*name*/,
        const std::map<std::string, std::string>& /*arguments*/ = {}) {
        return core::Result<core::JsonValue>::Err("prompts not supported by this client");
    }

    // ========== 配置管理 ==========

    /// 获取当前 MCP 配置
    virtual core::Result<core::JsonValue> get_config() = 0;

    /// 添加 MCP server 配置
    virtual core::Result<void> add_server(
        const std::string& name,
        const std::string& command,
        const core::JsonValue& options = {}
    ) = 0;

    /// 删除 MCP server 配置
    virtual core::Result<void> remove_server(const std::string& name) = 0;

    /// 导入配置文件
    virtual core::Result<void> import_config(const std::string& config_path) = 0;

    // ========== 健康检查 ==========

    /// 健康检查 (任意 server 可用即 healthy)
    virtual core::Result<bool> health_check() = 0;

    /// 获取客户端版本
    virtual core::Result<std::string> get_version() = 0;

    // ========== 工厂方法 ==========

    /// 创建 McporterClient 实例 (返回 NativeMcpClient)
    static std::shared_ptr<McporterClient> create(const McporterConfig& config);

protected:
    McporterConfig config_;
};

/// 工厂函数 (等价于 McporterClient::create)
std::shared_ptr<McporterClient> create_mcporter_client(const McporterConfig& config);

} // namespace mcp
} // namespace hunterclaw
