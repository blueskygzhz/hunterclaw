#include "hunterclaw/mcp/mcp_tool_adapter.h"
#include "hunterclaw/mcp/json_converter.h"
#include "hunterclaw/tools/tool_manager.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;
using hunterclaw::mcp::converters::JsonConverter;

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
    // 转换 JsonValue 到字符串以便日志记录
    std::string params_str = params.is_string() ? params.as_string() : "{}";
    log()->debug("Executing MCP tool: {} with params: {}", 
                definition_.name, params_str);
    
    // 调用 mcporter
    auto result = client_->call_tool(definition_.name, params);
    
    if (result.is_err()) {
        return tools::tool_result::error(
            "Failed to call MCP tool: " + result.error
        );
    }
    
    auto mcp_result = result.get();

    // 🎯 严格对齐 OpenClaw toAgentToolResult (agent-bundle-mcp-materialize.ts:90-145)。
    const std::string& server_name = definition_.server_name;
    const std::string& tool_name = definition_.tool_name;

    json source_content = JsonConverter::to_nlohmann(mcp_result.content);
    if (!source_content.is_array()) {
        source_content = json::array();
    }

    // 单个 MCP content block → HunterClaw ToolResultContent。
    auto block_to_content = [](const json& block) -> tools::ToolResultContent {
        tools::ToolResultContent c;
        if (block.is_object() && block.value("type", "") == "text" &&
            block.contains("text") && block["text"].is_string()) {
            c.type = tools::ToolResultContent::Type::Text;
            c.text = block["text"].get<std::string>();
        } else {
            // 非文本块（image/audio/resource 等）：HunterClaw 无对应富类型，
            // 以 JSON 文本承载，保留原始结构（对齐 mcpContentBlockToAgentContent 的降级）。
            c.type = tools::ToolResultContent::Type::Json;
            c.text = block.dump(2);
        }
        return c;
    };

    tools::ToolResult tool_result;
    tool_result.success = !mcp_result.is_error;

    if (mcp_result.has_structured_content) {
        // structuredContent 存在：首块为 "structuredContent:\n{pretty}"，
        // 其后保留原始的非 text 块（图片/资源/音频，JSON 无法镜像）。
        json structured = JsonConverter::to_nlohmann(mcp_result.structured_content);
        tools::ToolResultContent head;
        head.type = tools::ToolResultContent::Type::Text;
        head.text = "structuredContent:\n" + structured.dump(2);
        tool_result.content.push_back(std::move(head));
        for (const auto& block : source_content) {
            if (!(block.is_object() && block.value("type", "") == "text")) {
                tool_result.content.push_back(block_to_content(block));
            }
        }
    } else if (!source_content.empty()) {
        // 无 structured：映射全部原始 content 块。
        for (const auto& block : source_content) {
            tool_result.content.push_back(block_to_content(block));
        }
    } else {
        // 无内容：回退到 {status, server, tool} JSON（对齐 OC的兜底块）。
        json fallback = {
            {"status", mcp_result.is_error ? "error" : "ok"},
            {"server", server_name},
            {"tool", tool_name},
        };
        tools::ToolResultContent c;
        c.type = tools::ToolResultContent::Type::Text;
        c.text = fallback.dump(2);
        tool_result.content.push_back(std::move(c));
    }

    // details：mcpServer/mcpTool，有 structured 加 structuredContent，error 加 status。
    json details = json::object();
    details["mcpServer"] = server_name;
    details["mcpTool"] = tool_name;
    if (mcp_result.has_structured_content) {
        details["structuredContent"] = JsonConverter::to_nlohmann(mcp_result.structured_content);
    }
    if (mcp_result.is_error) {
        details["status"] = "error";
    }
    tool_result.details = JsonConverter::from_nlohmann(details);

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
    
    // 使用 JsonConverter 进行转换 (对齐 OpenClaw)
    json j = JsonConverter::to_nlohmann(json_schema);
    
    if (j.is_null() || !j.is_object()) {
        return schema;
    }
    
    // 提取 description
    if (j.contains("description") && j["description"].is_string()) {
        schema.description = j["description"].get<std::string>();
    }
    
    // 提取 properties (完整转换)
    if (j.contains("properties") && j["properties"].is_object()) {
        for (auto& [key, prop] : j["properties"].items()) {
            schema.properties[key] = JsonConverter::from_nlohmann(prop);
        }
    }
    
    // 提取 required
    if (j.contains("required") && j["required"].is_array()) {
        for (auto& item : j["required"]) {
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
                registered_count += result.get();
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
                registered_count += result.get();
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
        cache_.tools_by_server[server_name] = tools_result.get();
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
    
    const auto& tools = tools_result.get();
    for (const auto& tool : tools) {
        cache_.tools_by_server[tool.server_name].push_back(tool);
    }
    
    cache_.last_refresh = std::chrono::steady_clock::now();
    
    log()->info("Cached {} tools from {} servers",
               tools.size(),
               cache_.tools_by_server.size());
    
    return core::Result<void>::Ok();
}

} // namespace mcp
} // namespace hunterclaw
