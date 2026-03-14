#include "hunterclaw/mcp/mcporter_client.h"
#include "hunterclaw/mcp/mcp_tool_adapter.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw::mcp {

namespace {

// 获取 MCP logger
auto get_logger() {
    static auto logger = []() {
        auto log = spdlog::get("mcp");
        if (!log) {
            log = spdlog::stdout_logger_mt("mcp");
        }
        return log;
    }();
    return logger;
}

} // namespace

bool McporterClient::is_mcporter_installed() {
    get_logger()->warn("McporterClient::is_mcporter_installed not implemented");
    return false;
}

std::shared_ptr<McporterClient> create_mcporter_client(const McporterConfig&) {
    get_logger()->warn("create_mcporter_client not implemented (requires subprocess.h)");
    return nullptr;
}

std::shared_ptr<McpToolRegistry> McpToolRegistry::create(std::shared_ptr<McporterClient>) {
    get_logger()->warn("McpToolRegistry::create not implemented");
    return nullptr;
}

core::Result<int> McpToolRegistry::discover_and_register(tools::ToolManager&,
                                                          const std::vector<std::string>&) {
    get_logger()->warn("McpToolRegistry::discover_and_register not implemented");
    
    core::Result<int> result;
    result.error = "Not implemented";
    return result;
}

} // namespace hunterclaw::mcp
