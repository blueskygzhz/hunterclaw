#include "hunterclaw/mcp/mcp_tool_adapter.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace hunterclaw {
namespace mcp {

namespace {
    auto log() {
        static auto logger = spdlog::get("mcp");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("mcp");
        }
        return logger;
    }
}

// ============================================================================
// McpToolAdapter 实现
// ============================================================================

McpToolAdapter::McpToolAdapter(
    std::shared_ptr<McporterClient> client,
    const McpToolDefinition& definition
) : client_(client), definition_(definition) {}

std::string McpToolAdapter::name() const {
    return definition_.name;  // "server.tool"
}

std::string McpToolAdapter::description() const {
    if (!definition_.description.empty()) {
        return definition_.description;
    }
    return "MCP tool: " + definition_.name;
}

tools::Tool::Schema McpToolAdapter::parameters() const {
    return convert_json_schema(definition_.input_schema);
}

tools::ToolResult McpToolAdapter::execute(const core::JsonValue& params) {
    log()->debug("Executing MCP tool: {} with params: {}", 
                definition_.name, params.dump());
    
    // 调用 mcporter
    auto result = client_->call_tool(definition_.name, params);
    
    if (result.is_err()) {
        return tools::tool_result::error(
            "Failed to call MCP tool: " + result.error
        );
    }
    
    auto mcp_result = result.value;
    
    if (!mcp_result.success) {
        return tools::tool_result::error(
            "MCP tool execution failed: " + mcp_result.error
        );
    }
    
    // 转换结果
    tools::ToolResult tool_result;
    tool_result.success = true;
    
    // 处理结构化内容
    if (!mcp_result.structured_content.empty()) {
        // 如果是对象/数组,返回 JSON
        if (mcp_result.structured_content.is_object() || 
            mcp_result.structured_content.is_array()) {
            
            tools::ToolResultContent content;
            content.type = tools::ToolResultContent::Type::Json;
            content.text = mcp_result.structured_content.dump(2);
            
            tool_result.content.push_back(content);
            tool_result.details = mcp_result.structured_content;
            
        } else if (mcp_result.structured_content.is_string()) {
            // 字符串内容
            tools::ToolResultContent content;
            content.type = tools::ToolResultContent::Type::Text;
            content.text = mcp_result.structured_content.get<std::string>();
            
            tool_result.content.push_back(content);
        }
    } else if (!mcp_result.stdout_output.empty()) {
        // 回退到 stdout
        tools::ToolResultContent content;
        content.type = tools::ToolResultContent::Type::Text;
        content.text = mcp_result.stdout_output;
        
        tool_result.content.push_back(content);
    }
    
    return tool_result;
}

bool McpToolAdapter::requires_approval() const {
    return should_require_approval();
}

bool McpToolAdapter::owner_only() const {
    // MCP tools 默认不限制为 owner only
    // 可以通过配置或元数据扩展
    return false;
}

tools::Tool::Schema McpToolAdapter::convert_json_schema(
    const core::JsonValue& json_schema
) const {
    Schema schema;
    schema.type = "object";
    
    if (json_schema.is_null() || !json_schema.is_object()) {
        return schema;
    }
    
    // 提取 description
    if (json_schema.contains("description")) {
        schema.description = json_schema["description"].get<std::string>();
    }
    
    // 提取 properties
    if (json_schema.contains("properties") && 
        json_schema["properties"].is_object()) {
        
        for (auto& [key, prop] : json_schema["properties"].items()) {
            schema.properties[key] = prop;
        }
    }
    
    // 提取 required
    if (json_schema.contains("required") && 
        json_schema["required"].is_array()) {
        
        for (auto& item : json_schema["required"]) {
            if (item.is_string()) {
                schema.required.push_back(item.get<std::string>());
            }
        }
    }
    
    return schema;
}

bool McpToolAdapter::should_require_approval() const {
    // 启发式判断: 包含敏感操作的工具需要审批
    std::string lower_name = definition_.tool_name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                  [](unsigned char c){ return std::tolower(c); });
    
    // 敏感操作关键词
    const std::vector<std::string> sensitive_keywords = {
        "delete", "remove", "destroy", "drop",
        "create", "insert", "update", "modify",
        "execute", "run", "exec", "eval",
        "write", "commit", "push", "deploy",
        "sudo", "admin", "root"
    };
    
    for (const auto& keyword : sensitive_keywords) {
        if (lower_name.find(keyword) != std::string::npos) {
            return true;
        }
    }
    
    return false;
}

// ============================================================================
// McpToolRegistry 实现
// ============================================================================

McpToolRegistry::McpToolRegistry(std::shared_ptr<McporterClient> client)
    : client_(client) {}

std::shared_ptr<McpToolRegistry> McpToolRegistry::create(
    std::shared_ptr<McporterClient> client
) {
    return std::make_shared<McpToolRegistry>(client);
}

core::Result<int> McpToolRegistry::discover_and_register(
    tools::ToolManager& tool_manager,
    const std::vector<std::string>& server_names
) {
    // 刷新缓存 (如果过期)
    if (is_cache_expired()) {
        auto refresh_result = refresh_cache();
        if (refresh_result.is_err()) {
            return core::Result<int>::Err(
                "Failed to refresh tool cache: " + refresh_result.error
            );
        }
    }
    
    int registered_count = 0;
    
    if (server_names.empty()) {
        // 注册所有 servers
        for (const auto& [server_name, tools] : cache_.tools_by_server) {
            auto result = register_server_tools(tool_manager, server_name);
            if (result.is_ok()) {
                registered_count += result.value;
            } else {
                log()->warn("Failed to register tools from {}: {}", 
                           server_name, result.error);
            }
        }
    } else {
        // 注册指定 servers
        for (const auto& server_name : server_names) {
            auto result = register_server_tools(tool_manager, server_name);
            if (result.is_ok()) {
                registered_count += result.value;
            } else {
                log()->warn("Failed to register tools from {}: {}", 
                           server_name, result.error);
            }
        }
    }
    
    log()->info("Registered {} MCP tools", registered_count);
    return core::Result<int>::Ok(registered_count);
}

core::Result<int> McpToolRegistry::register_server_tools(
    tools::ToolManager& tool_manager,
    const std::string& server_name
) {
    // 从缓存获取 tools
    auto it = cache_.tools_by_server.find(server_name);
    if (it == cache_.tools_by_server.end()) {
        // 缓存中没有,直接查询
        auto tools_result = client_->list_tools(server_name, true);
        if (tools_result.is_err()) {
            return core::Result<int>::Err(tools_result.error);
        }
        
        // 更新缓存
        cache_.tools_by_server[server_name] = tools_result.value;
        it = cache_.tools_by_server.find(server_name);
    }
    
    int count = 0;
    
    for (const auto& tool_def : it->second) {
        // 创建适配器
        auto adapter = std::make_shared<McpToolAdapter>(client_, tool_def);
        
        // 注册到 ToolManager
        tool_manager.register_tool(adapter);
        
        count++;
    }
    
    log()->debug("Registered {} tools from server: {}", count, server_name);
    return core::Result<int>::Ok(count);
}

bool McpToolRegistry::is_cache_expired() const {
    if (cache_.tools_by_server.empty()) {
        return true;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - cache_.last_refresh
    );
    
    return elapsed >= cache_ttl_;
}

core::Result<void> McpToolRegistry::refresh_cache() {
    log()->info("Refreshing MCP tool cache...");
    
    // 发现所有 tools
    auto tools_result = client_->discover_all_tools();
    if (tools_result.is_err()) {
        return core::Result<void>::Err(tools_result.error);
    }
    
    // 按 server 分组
    cache_.tools_by_server.clear();
    
    for (const auto& tool : tools_result.value) {
        cache_.tools_by_server[tool.server_name].push_back(tool);
    }
    
    cache_.last_refresh = std::chrono::steady_clock::now();
    
    log()->info("Cached {} tools from {} servers",
               tools_result.value.size(),
               cache_.tools_by_server.size());
    
    return core::Result<void>::Ok();
}

} // namespace mcp
} // namespace hunterclaw
