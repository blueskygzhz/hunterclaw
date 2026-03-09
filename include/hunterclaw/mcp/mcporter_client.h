#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <map>

namespace hunterclaw {
namespace mcp {

// mcporter 配置
struct McporterConfig {
    bool enabled = false;
    std::string server_name = "default";  // MCP server 名称
    bool start_daemon = true;             // 自动启动 daemon
    int timeout_ms = 30000;               // 超时时间 (30秒)
    std::vector<std::string> allowed_servers;  // 白名单 (空=全部)
};

// mcporter 工具参数
struct McporterCallArgs {
    std::string server_and_tool;          // e.g. "linear.list_issues"
    core::JsonValue args;                 // 工具参数
    std::string output_format = "json";   // json/text/pretty
    int timeout_ms = 0;                   // 0=使用默认
};

// mcporter 调用结果
struct McporterResult {
    bool success = false;
    std::string stdout_output;
    std::string stderr_output;
    core::JsonValue structured_content;   // 解析后的 JSON
    std::string error;
    int exit_code = -1;
};

// mcporter 工具定义
struct McpToolDefinition {
    std::string name;           // 完整名称 "server.tool"
    std::string description;
    std::string server_name;
    std::string tool_name;
    core::JsonValue input_schema;  // JSON Schema
};

// mcporter Server 信息
struct McpServerInfo {
    std::string name;
    std::string command;
    std::string lifecycle;  // "keep-alive" / "per-request"
    core::JsonValue metadata;
};

/// mcporter Client - 桥接 MCP servers
class McporterClient {
public:
    virtual ~McporterClient() = default;
    
    // ========== Daemon 管理 ==========
    
    /// 启动 mcporter daemon
    virtual core::Result<void> start_daemon() = 0;
    
    /// 停止 mcporter daemon
    virtual core::Result<void> stop_daemon() = 0;
    
    /// 检查 daemon 状态
    virtual core::Result<bool> is_daemon_running() = 0;
    
    /// 重启 daemon
    virtual core::Result<void> restart_daemon() = 0;
    
    /// 确保 daemon 已启动 (全局单例,避免重复启动)
    /// 对齐 OpenClaw: 使用 static 全局变量跟踪启动状态
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
    /// 如果设置了 allowed_servers,只返回白名单中的
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
    
    // ========== 配置管理 ==========
    
    /// 获取 mcporter 配置
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
    
    /// 检查 mcporter 是否可用
    virtual core::Result<bool> health_check() = 0;
    
    /// 获取 mcporter 版本
    virtual core::Result<std::string> get_version() = 0;
    
    // ========== 实用功能 ==========
    
    /// 验证 mcporter 是否已安装
    static bool is_mcporter_installed();
    
    /// 获取 mcporter 可执行文件路径 (处理 Windows .cmd)
    static std::string resolve_mcporter_command();
    
    /// 创建 McporterClient 实例
    static std::shared_ptr<McporterClient> create(const McporterConfig& config);
    
protected:
    McporterConfig config_;
};

// 工厂函数
std::shared_ptr<McporterClient> create_mcporter_client(const McporterConfig& config);

} // namespace mcp
} // namespace hunterclaw
