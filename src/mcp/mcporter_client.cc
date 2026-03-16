#include "hunterclaw/mcp/mcporter_client.h"
#include "hunterclaw/mcp/json_converter.h"
#include "hunterclaw/core/subprocess.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <cstdlib>
#include <regex>

namespace fs = std::filesystem;
using json = nlohmann::json;
using hunterclaw::mcp::converters::JsonConverter;

namespace hunterclaw {
namespace mcp {

// ============================================================================
// 全局状态管理 (对齐 OpenClaw)
// ============================================================================

namespace {
    // 全局 daemon 启动状态
    struct DaemonState {
        std::mutex mutex;
        bool started = false;
        std::chrono::steady_clock::time_point last_start;
    };
    
    DaemonState& get_daemon_state() {
        static DaemonState state;
        return state;
    }
    
    // 日志
    auto log() {
        static auto logger = spdlog::get("mcp");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("mcp");
        }
        return logger;
    }
}

// ============================================================================
// McporterClient 实现
// ============================================================================

class McporterClientImpl : public McporterClient {
public:
    explicit McporterClientImpl(const McporterConfig& config)
        : config_(config) {}
    
    ~McporterClientImpl() override = default;
    
    // ========== Daemon 管理 ==========
    
    core::Result<void> start_daemon() override {
        auto cmd = resolve_mcporter_command();
        
        log()->info("Starting mcporter daemon...");
        
        auto result = core::subprocess::run({
            cmd, "daemon", "start"
        }, 10000);  // 10秒超时
        
        if (result.exit_code != 0) {
            return core::Result<void>::Err(
                "Failed to start daemon: " + result.stderr_output
            );
        }
        
        log()->info("mcporter daemon started successfully");
        return core::Result<void>::Ok();
    }
    
    core::Result<void> stop_daemon() override {
        auto cmd = resolve_mcporter_command();
        
        log()->info("Stopping mcporter daemon...");
        
        auto result = core::subprocess::run({
            cmd, "daemon", "stop"
        }, 5000);
        
        if (result.exit_code != 0) {
            log()->warn("Failed to stop daemon: {}", result.stderr_output);
        }
        
        return core::Result<void>::Ok();
    }
    
    core::Result<bool> is_daemon_running() override {
        auto cmd = resolve_mcporter_command();
        
        auto result = core::subprocess::run({
            cmd, "daemon", "status"
        }, 5000);
        
        // exit code 0 = running, 1 = not running
        return core::Result<bool>::Ok(result.exit_code == 0);
    }
    
    core::Result<void> restart_daemon() override {
        auto stop_result = stop_daemon();
        if (stop_result.is_err()) {
            log()->warn("Stop daemon failed (may not be running): {}", stop_result.error);
        }
        
        // 等待 500ms 确保进程退出
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        return start_daemon();
    }
    
    core::Result<void> ensure_daemon_started() override {
        if (!config_.start_daemon) {
            // 警告: 冷启动模式 (对齐 OpenClaw)
            static bool warned = false;
            if (!warned) {
                warned = true;
                log()->warn(
                    "mcporter enabled but start_daemon=false; "
                    "each query may cold-start MCP server. "
                    "Consider setting start_daemon=true to keep it warm."
                );
            }
            return core::Result<void>::Ok();
        }
        
        // 全局单例启动 (对齐 OpenClaw globalThis pattern)
        auto& state = get_daemon_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        
        if (state.started) {
            // 已启动,检查是否还在运行
            auto running = is_daemon_running();
            if (running.is_ok() && running.value) {
                return core::Result<void>::Ok();
            }
            
            // Daemon 已停止,重新标记未启动
            state.started = false;
        }
        
        // 启动 daemon
        auto start_result = start_daemon();
        if (start_result.is_err()) {
            return start_result;
        }
        
        // 标记已启动
        state.started = true;
        state.last_start = std::chrono::steady_clock::now();
        
        return core::Result<void>::Ok();
    }
    
    // ========== 工具发现 ==========
    
    core::Result<std::vector<McpServerInfo>> list_servers() override {
        auto cmd = resolve_mcporter_command();
        
        auto result = core::subprocess::run({
            cmd, "config", "list", "--output", "json"
        }, config_.timeout_ms);
        
        if (result.exit_code != 0) {
            return core::Result<std::vector<McpServerInfo>>::Err(
                "Failed to list servers: " + result.stderr_output
            );
        }
        
        try {
            auto j = json::parse(result.stdout_output);
            std::vector<McpServerInfo> servers;
            
            if (j.is_object() && j.contains("servers")) {
                for (auto& [name, config] : j["servers"].items()) {
                    McpServerInfo info;
                    info.name = name;
                    
                    if (config.contains("command")) {
                        info.command = config["command"].get<std::string>();
                    }
                    
                    if (config.contains("lifecycle")) {
                        info.lifecycle = config["lifecycle"].get<std::string>();
                    }
                    
                    // 转换 nlohmann::json 到 JsonValue (简化:序列化为字符串)
                    info.metadata = core::JsonValue(config.dump());
                    servers.push_back(info);
                }
            }
            
            return core::Result<std::vector<McpServerInfo>>::Ok(servers);
            
        } catch (const std::exception& e) {
            return core::Result<std::vector<McpServerInfo>>::Err(
                "Failed to parse server list: " + std::string(e.what())
            );
        }
    }
    
    core::Result<std::vector<McpToolDefinition>> list_tools(
        const std::string& server_name,
        bool with_schema
    ) override {
        auto cmd = resolve_mcporter_command();
        
        std::vector<std::string> args = {
            cmd, "list", server_name, "--output", "json"
        };
        
        if (with_schema) {
            args.push_back("--schema");
        }
        
        auto result = core::subprocess::run(args, config_.timeout_ms);
        
        if (result.exit_code != 0) {
            return core::Result<std::vector<McpToolDefinition>>::Err(
                "Failed to list tools: " + result.stderr_output
            );
        }
        
        try {
            auto j = json::parse(result.stdout_output);
            std::vector<McpToolDefinition> tools;
            
            if (j.is_object() && j.contains("tools")) {
                for (auto& [tool_name, tool_data] : j["tools"].items()) {
                    McpToolDefinition def;
                    def.server_name = server_name;
                    def.tool_name = tool_name;
                    def.name = server_name + "." + tool_name;
                    
                    if (tool_data.contains("description")) {
                        def.description = tool_data["description"].get<std::string>();
                    }
                    
                    if (with_schema && tool_data.contains("inputSchema")) {
                        def.input_schema = core::JsonValue(tool_data["inputSchema"].dump());
                    }
                    
                    tools.push_back(def);
                }
            }
            
            return core::Result<std::vector<McpToolDefinition>>::Ok(tools);
            
        } catch (const std::exception& e) {
            return core::Result<std::vector<McpToolDefinition>>::Err(
                "Failed to parse tools: " + std::string(e.what())
            );
        }
    }
    
    core::Result<std::vector<McpToolDefinition>> discover_all_tools() override {
        // 确保 daemon 启动
        auto ensure_result = ensure_daemon_started();
        if (ensure_result.is_err()) {
            return core::Result<std::vector<McpToolDefinition>>::Err(
                "Failed to start daemon: " + ensure_result.error
            );
        }
        
        // 获取所有 servers
        auto servers_result = list_servers();
        if (servers_result.is_err()) {
            return core::Result<std::vector<McpToolDefinition>>::Err(servers_result.error);
        }
        
        std::vector<McpToolDefinition> all_tools;
        
        for (const auto& server : servers_result.value) {
            // 检查白名单
            if (!config_.allowed_servers.empty()) {
                auto it = std::find(
                    config_.allowed_servers.begin(),
                    config_.allowed_servers.end(),
                    server.name
                );
                if (it == config_.allowed_servers.end()) {
                    log()->debug("Skipping server {} (not in whitelist)", server.name);
                    continue;
                }
            }
            
            // 获取该 server 的所有 tools
            auto tools_result = list_tools(server.name, true);
            if (tools_result.is_ok()) {
                all_tools.insert(
                    all_tools.end(),
                    tools_result.value.begin(),
                    tools_result.value.end()
                );
            } else {
                log()->warn("Failed to list tools for {}: {}", 
                           server.name, tools_result.error);
            }
        }
        
        log()->info("Discovered {} MCP tools from {} servers",
                   all_tools.size(), servers_result.value.size());
        
        return core::Result<std::vector<McpToolDefinition>>::Ok(all_tools);
    }
    
    // ========== 工具调用 ==========
    
    core::Result<McporterResult> call_tool(
        const std::string& server_and_tool,
        const core::JsonValue& args
    ) override {
        // 确保 daemon 启动
        auto ensure_result = ensure_daemon_started();
        if (ensure_result.is_err()) {
            return core::Result<McporterResult>::Err(
                "Failed to start daemon: " + ensure_result.error
            );
        }
        
        auto cmd = resolve_mcporter_command();
        
        // 使用 JsonConverter 转换参数 (对齐 OpenClaw)
        json args_json = JsonConverter::to_nlohmann(args);
        std::string args_str = JsonConverter::stringify(args_json, -1);  // 紧凑格式
        
        int timeout = config_.timeout_ms;
        
        std::vector<std::string> command = {
            cmd,
            "call",
            server_and_tool,
            "--args", args_str,
            "--output", "json",
            "--timeout", std::to_string(timeout)
        };
        
        log()->debug("Calling MCP tool: {} with args: {}", 
                    server_and_tool, args_str);
        
        auto result = core::subprocess::run(command, timeout + 2000);
        
        McporterResult mcp_result;
        mcp_result.stdout_output = result.stdout_output;
        mcp_result.stderr_output = result.stderr_output;
        mcp_result.exit_code = result.exit_code;
        
        if (result.exit_code != 0) {
            mcp_result.success = false;
            mcp_result.error = "mcporter call failed: " + result.stderr_output;
            return core::Result<McporterResult>::Ok(mcp_result);
        }
        
        // 解析结果 (对齐 OpenClaw)
        try {
            auto parsed = JsonConverter::parse_safe(result.stdout_output);
            
            if (parsed.is_null()) {
                mcp_result.success = false;
                mcp_result.error = "Failed to parse mcporter output as JSON";
                return core::Result<McporterResult>::Ok(mcp_result);
            }
            
            // mcporter 返回格式: { structuredContent: {...}, ... }
            if (parsed.is_object() && parsed.contains("structuredContent")) {
                mcp_result.structured_content = JsonConverter::from_nlohmann(parsed["structuredContent"]);
            } else {
                mcp_result.structured_content = JsonConverter::from_nlohmann(parsed);
            }
            
            mcp_result.success = true;
            
        } catch (const std::exception& e) {
            mcp_result.success = false;
            mcp_result.error = "Failed to parse result: " + std::string(e.what());
        }
        
        return core::Result<McporterResult>::Ok(mcp_result);
    }
    
    core::Result<std::vector<McporterResult>> call_tools(
        const std::vector<McporterCallArgs>& calls
    ) override {
        std::vector<McporterResult> results;
        results.reserve(calls.size());
        
        // TODO: 实现并发调用 (使用 thread pool)
        // 当前实现: 串行调用
        for (const auto& call : calls) {
            int timeout = call.timeout_ms > 0 ? call.timeout_ms : config_.timeout_ms;
            
            // 临时修改 timeout
            auto old_timeout = config_.timeout_ms;
            config_.timeout_ms = timeout;
            
            auto result = call_tool(call.server_and_tool, call.args);
            
            config_.timeout_ms = old_timeout;
            
            if (result.is_ok()) {
                results.push_back(result.value);
            } else {
                McporterResult error_result;
                error_result.success = false;
                error_result.error = result.error;
                results.push_back(error_result);
            }
        }
        
        return core::Result<std::vector<McporterResult>>::Ok(results);
    }
    
    // ========== 配置管理 ==========
    
    core::Result<core::JsonValue> get_config() override {
        auto cmd = resolve_mcporter_command();
        
        auto result = core::subprocess::run({
            cmd, "config", "list", "--output", "json"
        }, 5000);
        
        if (result.exit_code != 0) {
            return core::Result<core::JsonValue>::Err(
                "Failed to get config: " + result.stderr_output
            );
        }
        
        try {
            auto j = JsonConverter::parse_safe(result.stdout_output);
            
            if (j.is_null()) {
                return core::Result<core::JsonValue>::Err(
                    "Failed to parse config JSON"
                );
            }
            
            return core::Result<core::JsonValue>::Ok(JsonConverter::from_nlohmann(j));
        } catch (const std::exception& e) {
            return core::Result<core::JsonValue>::Err(
                "Failed to parse config: " + std::string(e.what())
            );
        }
    }
    
    core::Result<void> add_server(
        const std::string& name,
        const std::string& command,
        const core::JsonValue& options
    ) override {
        auto cmd = resolve_mcporter_command();
        
        std::vector<std::string> args = {
            cmd, "config", "add", name, "--command", command
        };
        
        // 添加选项
        if (options.contains("lifecycle")) {
            args.push_back("--lifecycle");
            args.push_back(options["lifecycle"].get<std::string>());
        }
        
        auto result = core::subprocess::run(args, 10000);
        
        if (result.exit_code != 0) {
            return core::Result<void>::Err(
                "Failed to add server: " + result.stderr_output
            );
        }
        
        log()->info("Added MCP server: {}", name);
        return core::Result<void>::Ok();
    }
    
    core::Result<void> remove_server(const std::string& name) override {
        auto cmd = resolve_mcporter_command();
        
        auto result = core::subprocess::run({
            cmd, "config", "remove", name
        }, 5000);
        
        if (result.exit_code != 0) {
            return core::Result<void>::Err(
                "Failed to remove server: " + result.stderr_output
            );
        }
        
        log()->info("Removed MCP server: {}", name);
        return core::Result<void>::Ok();
    }
    
    core::Result<void> import_config(const std::string& config_path) override {
        if (!fs::exists(config_path)) {
            return core::Result<void>::Err("Config file not found: " + config_path);
        }
        
        auto cmd = resolve_mcporter_command();
        
        auto result = core::subprocess::run({
            cmd, "config", "import", config_path
        }, 10000);
        
        if (result.exit_code != 0) {
            return core::Result<void>::Err(
                "Failed to import config: " + result.stderr_output
            );
        }
        
        log()->info("Imported mcporter config from: {}", config_path);
        return core::Result<void>::Ok();
    }
    
    // ========== 健康检查 ==========
    
    core::Result<bool> health_check() override {
        if (!is_mcporter_installed()) {
            return core::Result<bool>::Ok(false);
        }
        
        auto version_result = get_version();
        return core::Result<bool>::Ok(version_result.is_ok());
    }
    
    core::Result<std::string> get_version() override {
        auto cmd = resolve_mcporter_command();
        
        auto result = core::subprocess::run({
            cmd, "--version"
        }, 5000);
        
        if (result.exit_code != 0) {
            return core::Result<std::string>::Err(
                "Failed to get version: " + result.stderr_output
            );
        }
        
        return core::Result<std::string>::Ok(
            std::regex_replace(result.stdout_output, std::regex(R"(^\s+|\s+$)"), "")
        );
    }

private:
    McporterConfig config_;  // 配置
};

// ============================================================================
// 静态方法实现
// ============================================================================

bool McporterClient::is_mcporter_installed() {
    auto cmd = resolve_mcporter_command();
    
    auto result = core::subprocess::run({
        cmd, "--version"
    }, 3000);
    
    return result.exit_code == 0;
}

std::string McporterClient::resolve_mcporter_command() {
    std::string base_cmd = "mcporter";
    
#ifdef _WIN32
    // Windows: 需要 .cmd 后缀 (对齐 OpenClaw)
    // npm 全局安装的命令行工具会生成 .cmd wrapper
    return base_cmd + ".cmd";
#else
    return base_cmd;
#endif
}

std::shared_ptr<McporterClient> McporterClient::create(const McporterConfig& config) {
    return std::make_shared<McporterClientImpl>(config);
}

// ============================================================================
// 工厂函数
// ============================================================================

std::shared_ptr<McporterClient> create_mcporter_client(const McporterConfig& config) {
    return McporterClient::create(config);
}

} // namespace mcp
} // namespace hunterclaw
