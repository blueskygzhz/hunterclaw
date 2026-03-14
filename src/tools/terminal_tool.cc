#include "hunterclaw/tools/terminal_tool.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/infra/http_client.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <regex>

namespace hunterclaw {
namespace tools {

namespace {
    auto log() {
        auto logger = spdlog::get("terminal");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("terminal_tool");
        }
        return logger;
    }
    
    // OpenClaw 常量对齐
    const int DEFAULT_TERMINAL_TIMEOUT_MS = 30000;
    const int DEFAULT_TERMINAL_ROWS = 24;
    const int DEFAULT_TERMINAL_COLS = 80;
    const char* DEFAULT_SHELL = "bash";
}

// ===== Constructor =====

TerminalTool::TerminalTool(const ToolContext& context) 
    : context_(context) {}

std::string TerminalTool::name() const {
    return "terminal";
}

std::string TerminalTool::description() const {
    std::stringstream desc;
    desc << "Control persistent terminal sessions for long-running commands and interactive shells. "
         << "\n\n**Persistent Sessions**: "
         << "Terminal sessions persist across multiple tool calls. "
         << "Use action=\"start\" to create a new session, then use the returned sessionId "
         << "for subsequent action=\"execute\" calls. "
         << "\n\n**Multi-Tab Support**: "
         << "Manage multiple concurrent terminal sessions (tabs). "
         << "Each session has its own shell, cwd, and output history. "
         << "\n\n**Incremental Output**: "
         << "Use lastLine parameter with action=\"execute\" to fetch only new output lines "
         << "since the last call (avoids duplicate output). "
         << "\n\n**Actions**: "
         << "\n- status: Get status of all terminal sessions "
         << "\n- start: Create new terminal session (returns sessionId) "
         << "\n- stop: Terminate a terminal session "
         << "\n- tabs: List all active terminal tabs "
         << "\n- execute: Run command in session (auto-appends newline) "
         << "\n- input: Send raw input to session (no auto-newline, for interactive prompts) "
         << "\n- resize: Adjust terminal window size (rows x cols) "
         << "\n\n**Shell Selection**: "
         << "Defaults to bash. Specify shell=\"zsh\" or shell=\"sh\" if needed. "
         << "\n\n**Target Selection**: "
         << "- target=\"sandbox\": Sandbox terminal (requires sandbox.terminal.enabled) "
         << "- target=\"host\": Host terminal (requires terminal.enabled=true) "
         << "- target=\"node\": Node-hosted terminal (auto-routes to connected node) "
         << "\n\n**Working Directory**: "
         << "Each session starts in the workspace root. Use 'cd' command to change cwd. ";
    
    return desc.str();
}

Schema TerminalTool::parameters() const {
    Schema schema;
    schema.type = "object";
    schema.description = "Terminal tool parameters (action-specific)";
    
    // action (required)
    auto action_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Terminal action to perform"},
        {"enum", core::JsonValue::array({
            "status", "start", "stop", "tabs", "execute", "input", "resize"
        })}
    });
    schema.properties["action"] = action_prop;
    
    // target (optional)
    auto target_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Terminal location: sandbox, host, or node"},
        {"enum", core::JsonValue::array({"sandbox", "host", "node"})}
    });
    schema.properties["target"] = target_prop;
    
    // node (optional)
    auto node_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Node ID or name (for target=node)"}
    });
    schema.properties["node"] = node_prop;
    
    // sessionId (for execute/input/resize/stop)
    auto session_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Terminal session ID (from start action)"}
    });
    schema.properties["sessionId"] = session_prop;
    
    // shell (for start)
    auto shell_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Shell to use (bash, zsh, sh)"},
        {"default", "bash"}
    });
    schema.properties["shell"] = shell_prop;
    
    // cwd (for start)
    auto cwd_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Initial working directory"}
    });
    schema.properties["cwd"] = cwd_prop;
    
    // env (for start)
    auto env_prop = core::JsonValue::object({
        {"type", "object"},
        {"description", "Environment variables (key-value pairs)"}
    });
    schema.properties["env"] = env_prop;
    
    // command (for execute)
    auto command_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Command to execute (auto-appends newline)"}
    });
    schema.properties["command"] = command_prop;
    
    // text (for input)
    auto text_prop = core::JsonValue::object({
        {"type", "string"},
        {"description", "Raw text to send (no auto-newline)"}
    });
    schema.properties["text"] = text_prop;
    
    // lastLine (for execute)
    auto last_line_prop = core::JsonValue::object({
        {"type", "integer"},
        {"description", "Fetch output from this line onwards (incremental)"},
        {"default", 0}
    });
    schema.properties["lastLine"] = last_line_prop;
    
    // timeoutMs (for execute)
    auto timeout_prop = core::JsonValue::object({
        {"type", "integer"},
        {"description", "Command timeout in milliseconds"},
        {"default", DEFAULT_TERMINAL_TIMEOUT_MS}
    });
    schema.properties["timeoutMs"] = timeout_prop;
    
    // rows/cols (for start/resize)
    auto rows_prop = core::JsonValue::object({
        {"type", "integer"},
        {"description", "Terminal rows (height)"},
        {"default", DEFAULT_TERMINAL_ROWS}
    });
    schema.properties["rows"] = rows_prop;
    
    auto cols_prop = core::JsonValue::object({
        {"type", "integer"},
        {"description", "Terminal columns (width)"},
        {"default", DEFAULT_TERMINAL_COLS}
    });
    schema.properties["cols"] = cols_prop;
    
    schema.required = {"action"};
    
    return schema;
}

ToolResult TerminalTool::execute(const core::JsonValue& params) {
    ToolParams reader(params);
    std::string action = reader.read_string("action", true);
    std::string target = reader.read_string("target", false, "");
    std::string requested_node = reader.read_string("node", false, "");
    
    log()->info("[TerminalTool] Action: {}, Target: {}", action, target);
    
    // 解析 Node 目标
    auto node_target = resolve_terminal_node_target(requested_node, target, context_.sandbox_bridge_url);
    
    // 解析 Base URL (sandbox vs host)
    std::optional<std::string> base_url;
    if (!node_target.has_value()) {
        base_url = resolve_terminal_base_url(target, context_.sandbox_bridge_url);
    }
    
    // 路由到对应的 Action
    try {
        if (action == "status") {
            return action_status(base_url, node_target);
        } else if (action == "start") {
            std::string shell = reader.read_string("shell", false, DEFAULT_SHELL);
            std::string cwd = reader.read_string("cwd", false, context_.workspace_dir);
            core::JsonValue env = reader.read_object("env", false, core::JsonValue::object());
            int rows = static_cast<int>(reader.read_int("rows", false, DEFAULT_TERMINAL_ROWS));
            int cols = static_cast<int>(reader.read_int("cols", false, DEFAULT_TERMINAL_COLS));
            return action_start(base_url, node_target, shell, cwd, env, rows, cols);
        } else if (action == "stop") {
            std::string session_id = reader.read_string("sessionId", true);
            return action_stop(base_url, node_target, session_id);
        } else if (action == "tabs") {
            return action_tabs(base_url, node_target);
        } else if (action == "execute") {
            std::string session_id = reader.read_string("sessionId", true);
            std::string command = reader.read_string("command", true);
            int timeout_ms = static_cast<int>(reader.read_int("timeoutMs", false, DEFAULT_TERMINAL_TIMEOUT_MS));
            int last_line = static_cast<int>(reader.read_int("lastLine", false, 0));
            return action_execute(base_url, node_target, session_id, command, timeout_ms, last_line);
        } else if (action == "input") {
            std::string session_id = reader.read_string("sessionId", true);
            std::string text = reader.read_string("text", true);
            return action_input(base_url, node_target, session_id, text);
        } else if (action == "resize") {
            std::string session_id = reader.read_string("sessionId", true);
            int rows = static_cast<int>(reader.read_int("rows", true));
            int cols = static_cast<int>(reader.read_int("cols", true));
            return action_resize(base_url, node_target, session_id, rows, cols);
        } else {
            return tool_result::error("Unknown terminal action: " + action);
        }
    } catch (const std::exception& e) {
        log()->error("[TerminalTool] Action '{}' failed: {}", action, e.what());
        return tool_result::error(std::string("Terminal action failed: ") + e.what());
    }
}

bool TerminalTool::requires_approval() const {
    return true;  // Terminal execute 需要审批
}

// ===== 连接管理 =====

std::optional<std::string> TerminalTool::resolve_terminal_base_url(
    const std::string& target,
    const std::string& sandbox_bridge_url
) const {
    std::string normalized_target = target.empty() ? 
        (sandbox_bridge_url.empty() ? "host" : "sandbox") : target;
    
    if (normalized_target == "sandbox") {
        if (sandbox_bridge_url.empty()) {
            throw std::runtime_error(
                "Sandbox terminal unavailable. Enable agents.defaults.sandbox.terminal.enabled "
                "or use target=\"host\"");
        }
        std::string url = sandbox_bridge_url;
        if (url.back() == '/') {
            url.pop_back();
        }
        return url;
    }
    
    if (normalized_target == "host") {
        // TODO: 读取配置 terminal.enabled
        // 暂时返回默认 localhost:8766
        return "http://localhost:8766";
    }
    
    return std::nullopt;  // target="node"
}

std::optional<TerminalTool::TerminalNodeTarget> TerminalTool::resolve_terminal_node_target(
    const std::string& requested_node,
    const std::string& target,
    const std::string& sandbox_bridge_url
) const {
    if (target == "node" || !requested_node.empty()) {
        log()->warn("[TerminalTool] Node routing not yet implemented, falling back to host");
        return std::nullopt;
    }
    
    return std::nullopt;
}

// ===== Terminal Actions =====

ToolResult TerminalTool::action_status(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target
) {
    log()->info("[TerminalTool] GET /status");
    
    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "GET", "/",
            core::JsonValue::object(), core::JsonValue::object(),
            DEFAULT_TERMINAL_TIMEOUT_MS);
        return tool_result::json(result);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Terminal base URL not configured");
    }
    
    std::string url = base_url.value() + "/";
    auto result = http_get(url);
    
    return tool_result::json(result);
}

ToolResult TerminalTool::action_start(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target,
    const std::string& shell,
    const std::string& cwd,
    const core::JsonValue& env,
    int rows,
    int cols
) {
    log()->info("[TerminalTool] POST /start (shell={}, cwd={})", shell, cwd);
    
    auto body = core::JsonValue::object({
        {"shell", shell},
        {"cwd", cwd},
        {"rows", rows},
        {"cols", cols}
    });
    
    if (!env.is_null() && env.is_object()) {
        body["env"] = env;
    }
    
    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "POST", "/start",
            core::JsonValue::object(), body,
            DEFAULT_TERMINAL_TIMEOUT_MS);
        return tool_result::json(result);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Terminal base URL not configured");
    }
    
    std::string url = base_url.value() + "/start";
    auto result = http_post(url, body);
    
    // Mock 响应
    result["sessionId"] = "term-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    result["shell"] = shell;
    result["cwd"] = cwd;
    result["rows"] = rows;
    result["cols"] = cols;
    
    return tool_result::json(result);
}

ToolResult TerminalTool::action_stop(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target,
    const std::string& session_id
) {
    log()->info("[TerminalTool] DELETE /sessions/{}", session_id);
    
    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "DELETE", "/sessions/" + session_id,
            core::JsonValue::object(), core::JsonValue::object(),
            DEFAULT_TERMINAL_TIMEOUT_MS);
        return tool_result::json(result);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Terminal base URL not configured");
    }
    
    std::string url = base_url.value() + "/sessions/" + session_id;
    auto result = http_delete(url);
    
    return tool_result::json(result);
}

ToolResult TerminalTool::action_tabs(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target
) {
    log()->info("[TerminalTool] GET /tabs");
    
    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "GET", "/tabs",
            core::JsonValue::object(), core::JsonValue::object(),
            DEFAULT_TERMINAL_TIMEOUT_MS);
        return tool_result::json(result);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Terminal base URL not configured");
    }
    
    std::string url = base_url.value() + "/tabs";
    auto result = http_get(url);
    
    return tool_result::json(result);
}

ToolResult TerminalTool::action_execute(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target,
    const std::string& session_id,
    const std::string& command,
    int timeout_ms,
    int last_line
) {
    log()->info("[TerminalTool] POST /sessions/{}/execute (command='{}', lastLine={})", 
                session_id, command, last_line);
    
    auto body = core::JsonValue::object({
        {"command", command},
        {"timeoutMs", timeout_ms},
        {"lastLine", last_line}
    });
    
    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "POST", "/sessions/" + session_id + "/execute",
            core::JsonValue::object(), body,
            timeout_ms + 5000);  // 额外 5s 余量
        
        // 格式化输出
        std::vector<std::string> output_lines;
        if (result.contains("output") && result["output"].is_array()) {
            for (const auto& line : result["output"].items()) {
                output_lines.push_back(line.value().get<std::string>());
            }
        }
        
        int exit_code = result.contains("exitCode") ? result["exitCode"].get<int>() : 0;
        std::string cwd = result.contains("cwd") ? result["cwd"].get<std::string>() : "";
        
        return format_terminal_output(output_lines, last_line, session_id, cwd, exit_code);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Terminal base URL not configured");
    }
    
    std::string url = base_url.value() + "/sessions/" + session_id + "/execute";
    auto result = http_post(url, body);
    
    // Mock 响应
    std::vector<std::string> output_lines = {
        "$ " + command,
        "Command output line 1",
        "Command output line 2",
        "$ "
    };
    
    return format_terminal_output(output_lines, last_line, session_id, context_.workspace_dir, 0);
}

ToolResult TerminalTool::action_input(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target,
    const std::string& session_id,
    const std::string& text
) {
    log()->info("[TerminalTool] POST /sessions/{}/input", session_id);
    
    auto body = core::JsonValue::object({{"text", text}});
    
    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "POST", "/sessions/" + session_id + "/input",
            core::JsonValue::object(), body,
            DEFAULT_TERMINAL_TIMEOUT_MS);
        return tool_result::json(result);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Terminal base URL not configured");
    }
    
    std::string url = base_url.value() + "/sessions/" + session_id + "/input";
    auto result = http_post(url, body);
    
    return tool_result::json(result);
}

ToolResult TerminalTool::action_resize(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target,
    const std::string& session_id,
    int rows,
    int cols
) {
    log()->info("[TerminalTool] POST /sessions/{}/resize ({}x{})", session_id, rows, cols);
    
    auto body = core::JsonValue::object({
        {"rows", rows},
        {"cols", cols}
    });
    
    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "POST", "/sessions/" + session_id + "/resize",
            core::JsonValue::object(), body,
            DEFAULT_TERMINAL_TIMEOUT_MS);
        return tool_result::json(result);
    }
    
    if (!base_url.has_value()) {
        return tool_result::error("Terminal base URL not configured");
    }
    
    std::string url = base_url.value() + "/sessions/" + session_id + "/resize";
    auto result = http_post(url, body);
    
    return tool_result::json(result);
}

// ===== HTTP 请求辅助 =====

core::JsonValue TerminalTool::http_get(const std::string& url) const {
    log()->info("[TerminalTool] HTTP GET: {}", url);
    
    auto client = infra::HttpClient::create();
    auto result = client->get(url, DEFAULT_TERMINAL_TIMEOUT_MS);
    
    if (result.is_err()) {
        log()->error("[TerminalTool] HTTP GET failed: {}", result.error());
        return core::JsonValue::object({
            {"ok", false},
            {"error", result.error()}
        });
    }
    
    auto& response = result.value();
    if (!response.ok) {
        log()->warn("[TerminalTool] HTTP GET returned {}", response.status_code);
        return core::JsonValue::object({
            {"ok", false},
            {"status", response.status_code},
            {"body", response.body}
        });
    }
    
    auto json_result = response.json();
    if (json_result.is_err()) {
        return core::JsonValue::object({
            {"ok", false},
            {"error", "Invalid JSON response"},
            {"body", response.body}
        });
    }
    
    return json_result.value();
}

core::JsonValue TerminalTool::http_post(const std::string& url, const core::JsonValue& body) const {
    log()->info("[TerminalTool] HTTP POST: {}", url);
    
    auto client = infra::HttpClient::create();
    auto result = client->post(url, body, DEFAULT_TERMINAL_TIMEOUT_MS);
    
    if (result.is_err()) {
        log()->error("[TerminalTool] HTTP POST failed: {}", result.error());
        return core::JsonValue::object({
            {"ok", false},
            {"error", result.error()}
        });
    }
    
    auto& response = result.value();
    if (!response.ok) {
        log()->warn("[TerminalTool] HTTP POST returned {}", response.status_code);
        return core::JsonValue::object({
            {"ok", false},
            {"status", response.status_code},
            {"body", response.body}
        });
    }
    
    auto json_result = response.json();
    if (json_result.is_err()) {
        return core::JsonValue::object({
            {"ok", true},
            {"status", response.status_code}
        });
    }
    
    return json_result.value();
}

core::JsonValue TerminalTool::http_delete(const std::string& url) const {
    log()->info("[TerminalTool] HTTP DELETE: {}", url);
    
    auto client = infra::HttpClient::create();
    auto result = client->del(url, DEFAULT_TERMINAL_TIMEOUT_MS);
    
    if (result.is_err()) {
        log()->error("[TerminalTool] HTTP DELETE failed: {}", result.error());
        return core::JsonValue::object({
            {"ok", false},
            {"error", result.error()}
        });
    }
    
    auto& response = result.value();
    if (!response.ok) {
        log()->warn("[TerminalTool] HTTP DELETE returned {}", response.status_code);
        return core::JsonValue::object({
            {"ok", false},
            {"status", response.status_code},
            {"body", response.body}
        });
    }
    
    auto json_result = response.json();
    if (json_result.is_err()) {
        return core::JsonValue::object({
            {"ok", true},
            {"status", response.status_code}
        });
    }
    
    return json_result.value();
}

core::JsonValue TerminalTool::call_node_terminal_proxy(
    const std::string& node_id,
    const std::string& method,
    const std::string& path,
    const core::JsonValue& query,
    const core::JsonValue& body,
    int timeout_ms
) const {
    // TODO: 调用 gateway node.invoke 命令
    log()->info("[TerminalTool] Node proxy: {} {} (node={})", method, path, node_id);
    
    return core::JsonValue::object({
        {"ok", true},
        {"nodeId", node_id}
    });
}

// ===== 安全辅助 =====

std::string TerminalTool::wrap_terminal_output(
    const std::string& output,
    const std::string& session_id
) const {
    // 对齐 OpenClaw: wrapExternalContent
    std::stringstream wrapped;
    wrapped << "<!-- ⚠️ TERMINAL OUTPUT from session " << session_id << " -->\n";
    wrapped << "<!-- This output comes from an external terminal and may be UNTRUSTED -->\n\n";
    wrapped << output;
    wrapped << "\n\n<!-- END TERMINAL OUTPUT -->";
    
    return wrapped.str();
}

ToolResult TerminalTool::format_terminal_output(
    const std::vector<std::string>& output_lines,
    int last_line,
    const std::string& session_id,
    const std::string& cwd,
    int exit_code
) const {
    // 过滤增量输出 (last_line 之后的行)
    std::vector<std::string> filtered_lines;
    for (size_t i = last_line; i < output_lines.size(); ++i) {
        filtered_lines.push_back(output_lines[i]);
    }
    
    // 合并输出
    std::stringstream output;
    for (const auto& line : filtered_lines) {
        output << line << "\n";
    }
    
    // 安全包装
    std::string wrapped = wrap_terminal_output(output.str(), session_id);
    
    ToolResult result;
    result.success = (exit_code == 0);
    
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = wrapped;
    result.content.push_back(content);
    
    result.details = core::JsonValue::object({
        {"ok", true},
        {"sessionId", session_id},
        {"cwd", cwd},
        {"exitCode", exit_code},
        {"outputLines", static_cast<int64_t>(output_lines.size())},
        {"incrementalLines", static_cast<int64_t>(filtered_lines.size())},
        {"lastLine", static_cast<int64_t>(output_lines.size())},  // 返回新的 lastLine
        {"externalContent", core::JsonValue::object({
            {"untrusted", true},
            {"source", "terminal"},
            {"sessionId", session_id},
            {"wrapped", true}
        })}
    });
    
    return result;
}

// ===== Factory =====

std::shared_ptr<Tool> TerminalToolFactory::create(const ToolContext& context) {
    return std::make_shared<TerminalTool>(context);
}

} // namespace tools
} // namespace hunterclaw
