#pragma once

// 🎯 严格对齐 OpenClaw src/agents/agent-bundle-mcp-materialize.ts 的MCP utility tools：
//   resources_list / resources_read / prompts_list / prompts_get。
//   每个 utility 操作投影为一个 agent tool，命名 {safeServer}__{operation}，
//   execute 调用 client 的对应 RPC，并用 toJsonAgentToolResult 语义包装结果
//   （content = [text: pretty JSON]，details = {mcpServer, mcpOperation, untrustedMcpOutput}）。

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/mcp/mcporter_client.h"
#include <memory>
#include <string>

namespace hunterclaw {
namespace mcp {

// utility 操作类型（对齐 PluginToolMcpMeta.operation 的非 "tool" 取值）。
enum class McpUtilityOperation {
    ResourcesList,
    ResourcesRead,
    PromptsList,
    PromptsGet,
};

// 对齐 addMcpUtilityTool 生成的单个 utility agent tool。
class McpUtilityToolAdapter : public tools::Tool {
public:
    McpUtilityToolAdapter(
        std::shared_ptr<McporterClient> client,
        std::string server_name,
        std::string safe_server_name,
        McpUtilityOperation operation);

    std::string name() const override;
    std::string description() const override;
    Schema parameters() const override;
    tools::ToolResult execute(const core::JsonValue& params) override;
    bool requires_approval() const override;
    bool owner_only() const override;

    // operation → wire 名（"resources_list" 等），用于命名与 details.mcpOperation。
    static std::string operation_key(McpUtilityOperation op);

private:
    std::shared_ptr<McporterClient> client_;
    std::string server_name_;
    std::string safe_server_name_;
    McpUtilityOperation operation_;
    std::string tool_name_;  // {safeServer}__{operation}
};

} // namespace mcp
} // namespace hunterclaw
