#include "hunterclaw/mcp/mcporter_client.h"
#include "hunterclaw/mcp/mcp_tool_adapter.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw::mcp {

bool McporterClient::is_mcporter_installed() {
    static auto logger = spdlog::get("mcp");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("mcp");
    }
    logger->warn("McporterClient::is_mcporter_installed not implemented");
    return false;
}

std::shared_ptr<McporterClient> create_mcporter_client(const McporterConfig&) {
    static auto logger = spdlog::get("mcp");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("mcp");
    }
    logger->warn("create_mcporter_client not implemented (requires subprocess.h)");
    return nullptr;
}

std::shared_ptr<McpToolRegistry> McpToolRegistry::create(std::shared_ptr<McporterClient>) {
    static auto logger = spdlog::get("mcp");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("mcp");
    }
    logger->warn("McpToolRegistry::create not implemented");
    return nullptr;
}

core::Result<int> McpToolRegistry::discover_and_register(tools::ToolManager&,
                                                          const std::vector<std::string>&) {
    static auto logger = spdlog::get("mcp");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("mcp");
    }
    logger->warn("McpToolRegistry::discover_and_register not implemented");
    
    core::Result<int> result;
    result.error = "Not implemented";
    return result;
}

} // namespace hunterclaw::mcp
