#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/mcp/mcporter_client.h"
#include <memory>
#include <chrono>
#include <map>

namespace hunterclaw {
namespace mcp {

/// 将 MCP Tool 适配为 HunterClaw Tool
class McpToolAdapter : public tools::Tool {
public:
    McpToolAdapter(
        std::shared_ptr<McporterClient> client,
        const McpToolDefinition& definition
    );
    
    virtual ~McpToolAdapter() = default;
    
    // Tool 接口实现
    std::string name() const override;
    std::string description() const override;
    Schema parameters() const override;
    tools::ToolResult execute(const core::JsonValue& params) override;
    bool requires_approval() const override;
    bool owner_only() const override;
    
private:
    std::shared_ptr<McporterClient> client_;
    McpToolDefinition definition_;
    
    // 将 JSON Schema 转换为 Tool::Schema
    Schema convert_json_schema(const core::JsonValue& json_schema) const;
    
    // 判断是否需要审批 (基于工具名称启发式)
    bool should_require_approval() const;
};

/// MCP Tools 注册器
class McpToolRegistry {
public:
    explicit McpToolRegistry(std::shared_ptr<McporterClient> client);
    
    /// 从 MCP server 发现并注册所有 tools
    /// @param tool_manager 要注册到的 ToolManager
    /// @param server_names 要注册的 server 名称列表 (空=注册所有)
    /// @return 注册的工具数量
    core::Result<int> discover_and_register(
        tools::ToolManager& tool_manager,
        const std::vector<std::string>& server_names = {}
    );
    
    /// 从指定 server 注册 tools
    core::Result<int> register_server_tools(
        tools::ToolManager& tool_manager,
        const std::string& server_name
    );
    
    /// 创建 McpToolRegistry
    static std::shared_ptr<McpToolRegistry> create(
        std::shared_ptr<McporterClient> client
    );
    
private:
    std::shared_ptr<McporterClient> client_;
    
    // 工具缓存 (避免重复发现)
    struct ToolCache {
        std::chrono::steady_clock::time_point last_refresh;
        std::map<std::string, std::vector<McpToolDefinition>> tools_by_server;
    };
    ToolCache cache_;
    std::chrono::seconds cache_ttl_{300};  // 5分钟 TTL
    
    // 检查缓存是否过期
    bool is_cache_expired() const;
    
    // 刷新缓存
    core::Result<void> refresh_cache();
};

} // namespace mcp
} // namespace hunterclaw
