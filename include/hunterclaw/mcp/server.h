#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/json.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace hunterclaw {
namespace mcp {

// ============================================================================
// MCP (Model Context Protocol) - Server Implementation
// ============================================================================

/**
 * MCP 工具定义
 */
struct MCPTool {
    std::string name;
    std::string description;
    core::JsonValue input_schema;  // JSON Schema
};

/**
 * MCP 资源定义
 */
struct MCPResource {
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type;
};

/**
 * MCP Prompt 定义
 */
struct MCPPrompt {
    std::string name;
    std::string description;
    std::vector<core::JsonValue> arguments;
};

/**
 * MCP Server 接口
 */
class MCPServer {
public:
    virtual ~MCPServer() = default;
    
    // Server 信息
    virtual std::string name() const = 0;
    virtual std::string version() const = 0;
    
    // 工具
    virtual std::vector<MCPTool> list_tools() = 0;
    virtual core::Result<core::JsonValue> call_tool(
        const std::string& name,
        const core::JsonValue& arguments
    ) = 0;
    
    // 资源
    virtual std::vector<MCPResource> list_resources() = 0;
    virtual core::Result<std::string> read_resource(const std::string& uri) = 0;
    
    // Prompts
    virtual std::vector<MCPPrompt> list_prompts() = 0;
    virtual core::Result<std::string> get_prompt(
        const std::string& name,
        const core::JsonValue& arguments
    ) = 0;
};

/**
 * HunterClaw MCP Server
 */
class HunterClawMCPServer : public MCPServer {
public:
    HunterClawMCPServer();
    ~HunterClawMCPServer() override;
    
    std::string name() const override { return "hunterclaw"; }
    std::string version() const override { return "1.0.0"; }
    
    std::vector<MCPTool> list_tools() override;
    core::Result<core::JsonValue> call_tool(
        const std::string& name,
        const core::JsonValue& arguments
    ) override;
    
    std::vector<MCPResource> list_resources() override;
    core::Result<std::string> read_resource(const std::string& uri) override;
    
    std::vector<MCPPrompt> list_prompts() override;
    core::Result<std::string> get_prompt(
        const std::string& name,
        const core::JsonValue& arguments
    ) override;
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcp
} // namespace hunterclaw
