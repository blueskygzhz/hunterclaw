// terminal_tool.cc
// Strictly aligned with OpenClaw: src/agents/tools/terminal-tool.ts
//
// Tool: terminal
// Actions: status | start | stop | tabs | execute | input | resize
// Targets: sandbox | host | node

#include "hunterclaw/tools/terminal_tool.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/core/types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <set>
#include <optional>
#include <cstdlib>
#include <cmath>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

// ============================================================================
// Constants (aligned with OpenClaw terminal-tool.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: DEFAULT_TERMINAL_TIMEOUT_MS = 30_000
static constexpr int DEFAULT_TERMINAL_TIMEOUT_MS = 30000;
// 🎯 对齐 OpenClaw: DEFAULT_ROWS = 24, DEFAULT_COLS = 80
static constexpr int DEFAULT_ROWS = 24;
static constexpr int DEFAULT_COLS = 80;

static const std::set<std::string> TERMINAL_ACTIONS = {
    "status", "start", "stop", "tabs", "execute", "input", "resize"
};

// ============================================================================
// Helpers
// ============================================================================

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string read_str(const json& params, const std::string& key) {
    auto it = params.find(key);
    if (it == params.end() || !it->is_string()) return "";
    return trim(it->get<std::string>());
}

// ============================================================================
// TerminalTool implementation
// ============================================================================

TerminalTool::TerminalTool(const ToolContext& context)
    : context_(context) {}

std::string TerminalTool::name() const { return "terminal"; }

std::string TerminalTool::description() const {
    return "Control persistent terminal sessions "
           "(status/start/stop/tabs/execute/input/resize). "
           "Use execute to run commands and get output. "
           "Sessions persist across tool calls. "
           "target selects terminal location (sandbox|host|node). Default: host.";
}

bool TerminalTool::requires_approval() const { return false; }

Schema TerminalTool::parameters() const {
    Schema schema;
    schema.type = "object";

    auto make_str = [](const std::string& desc) {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = desc;
        return p;
    };
    auto make_num = [](const std::string& desc) {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "number";
        p["description"] = desc;
        return p;
    };

    // action (required)
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Terminal action to perform";
        core::JsonValue e = core::JsonValue::array();
        for (const auto& a : TERMINAL_ACTIONS) e.push_back(a);
        p["enum"] = e;
        schema.properties["action"] = p;
    }

    // 🎯 对齐 OpenClaw: TerminalToolSchema (flattened)
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Terminal location: sandbox, host (default), node";
        core::JsonValue e = core::JsonValue::array();
        e.push_back("sandbox"); e.push_back("host"); e.push_back("node");
        p["enum"] = e;
        schema.properties["target"] = p;
    }
    schema.properties["node"]       = make_str("Node ID or name for node-hosted terminal");
    schema.properties["sessionId"]  = make_str("Terminal session ID");
    schema.properties["command"]    = make_str("Command to execute");
    schema.properties["text"]       = make_str("Raw text to send as input");
    schema.properties["shell"]      = make_str("Shell to use: bash, zsh, sh (default: auto-detect)");
    schema.properties["cwd"]        = make_str("Working directory for new session");
    schema.properties["timeoutMs"]  = make_num("Command timeout in milliseconds (default 30000)");
    schema.properties["lastLine"]   = make_num("Last output line number seen (for incremental output)");
    schema.properties["rows"]       = make_num("Terminal rows (default 24)");
    schema.properties["cols"]       = make_num("Terminal columns (default 80)");
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "object";
        p["description"] = "Environment variables (key-value pairs)";
        p["additionalProperties"] = core::JsonValue(json{{"type", "string"}});
        schema.properties["env"] = p;
    }

    schema.required = {"action"};
    return schema;
}

ToolResult TerminalTool::execute(const core::JsonValue& params_val) {
    try {
        json params = params_val.to_json();
        std::string action = read_str(params, "action");
        if (action.empty())
            return make_error("action is required");
        if (!TERMINAL_ACTIONS.count(action))
            return make_error("Unknown action: " + action);

        std::string target = read_str(params, "target");
        std::string requested_node = read_str(params, "node");

        auto node_target = resolve_terminal_node_target(requested_node, target, "");
        std::optional<std::string> base_url;
        if (!node_target.has_value()) {
            base_url = resolve_terminal_base_url(target, "");
        }

        if (action == "status")
            return action_status(base_url, node_target);

        if (action == "tabs")
            return action_tabs(base_url, node_target);

        if (action == "start") {
            std::string shell = read_str(params, "shell");
            std::string cwd   = read_str(params, "cwd");
            int rows = DEFAULT_ROWS, cols = DEFAULT_COLS;
            if (params.contains("rows") && params["rows"].is_number())
                rows = (int)params["rows"].get<double>();
            if (params.contains("cols") && params["cols"].is_number())
                cols = (int)params["cols"].get<double>();
            core::JsonValue env = params.contains("env")
                ? core::JsonValue::from_json(params["env"]) : core::JsonValue::object();
            return action_start(base_url, node_target, shell, cwd, env, rows, cols);
        }

        if (action == "stop") {
            std::string session_id = read_str(params, "sessionId");
            return action_stop(base_url, node_target, session_id);
        }

        if (action == "execute") {
            std::string session_id = read_str(params, "sessionId");
            std::string command    = read_str(params, "command");
            if (command.empty()) return make_error("command required");
            int timeout_ms = DEFAULT_TERMINAL_TIMEOUT_MS;
            if (params.contains("timeoutMs") && params["timeoutMs"].is_number())
                timeout_ms = (int)params["timeoutMs"].get<double>();
            int last_line = 0;
            if (params.contains("lastLine") && params["lastLine"].is_number())
                last_line = (int)params["lastLine"].get<double>();
            return action_execute(base_url, node_target, session_id, command, timeout_ms, last_line);
        }

        if (action == "input") {
            std::string session_id = read_str(params, "sessionId");
            std::string text       = read_str(params, "text");
            if (text.empty()) return make_error("text required");
            return action_input(base_url, node_target, session_id, text);
        }

        if (action == "resize") {
            std::string session_id = read_str(params, "sessionId");
            int rows = DEFAULT_ROWS, cols = DEFAULT_COLS;
            if (params.contains("rows") && params["rows"].is_number())
                rows = (int)params["rows"].get<double>();
            if (params.contains("cols") && params["cols"].is_number())
                cols = (int)params["cols"].get<double>();
            return action_resize(base_url, node_target, session_id, rows, cols);
        }

        return make_error("Unknown action: " + action);

    } catch (const std::exception& e) {
        return make_error(std::string(e.what()));
    }
}

// ============================================================================
// Connection resolution
// ============================================================================

std::optional<std::string> TerminalTool::resolve_terminal_base_url(
    const std::string& target,
    const std::string& /*sandbox_bridge_url*/
) const {
    if (target == "host" || target.empty()) {
        const char* env = std::getenv("TERMINAL_CONTROL_URL");
        if (env) return std::string(env);
        return std::string("http://localhost:3001");
    }
    return std::nullopt;
}

std::optional<TerminalTool::TerminalNodeTarget> TerminalTool::resolve_terminal_node_target(
    const std::string& requested_node,
    const std::string& target,
    const std::string& /*sandbox_bridge_url*/
) const {
    if (target != "node" && requested_node.empty()) return std::nullopt;
    if (!requested_node.empty()) {
        return TerminalNodeTarget{requested_node, requested_node};
    }
    return std::nullopt;
}

// ============================================================================
// Actions
// ============================================================================

ToolResult TerminalTool::action_status(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target
) {
    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "GET", "/status", {}, {}, DEFAULT_TERMINAL_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_get(base_url.value_or("http://localhost:3001") + "/status");
    return make_json_result(result);
}

ToolResult TerminalTool::action_tabs(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target
) {
    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "GET", "/tabs", {}, {}, DEFAULT_TERMINAL_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_get(base_url.value_or("http://localhost:3001") + "/tabs");
    return make_json_result(result);
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
    json body = {{"rows", rows}, {"cols", cols}};
    if (!shell.empty()) body["shell"] = shell;
    if (!cwd.empty())   body["cwd"]   = cwd;
    if (!env.is_null()) body["env"]   = env.to_json();

    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "POST", "/start", {}, core::JsonValue::from_json(body),
            DEFAULT_TERMINAL_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_post(base_url.value_or("http://localhost:3001") + "/start",
                            core::JsonValue::from_json(body));
    return make_json_result(result);
}

ToolResult TerminalTool::action_stop(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target,
    const std::string& session_id
) {
    json body = json::object();
    if (!session_id.empty()) body["sessionId"] = session_id;

    if (node_target.has_value()) {
        call_node_terminal_proxy(
            node_target->node_id, "POST", "/stop", {}, core::JsonValue::from_json(body),
            DEFAULT_TERMINAL_TIMEOUT_MS);
        return make_json_result(json{{"ok", true}});
    }
    http_post(base_url.value_or("http://localhost:3001") + "/stop",
              core::JsonValue::from_json(body));
    return make_json_result(json{{"ok", true}});
}

ToolResult TerminalTool::action_execute(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target,
    const std::string& session_id,
    const std::string& command,
    int timeout_ms,
    int last_line
) {
    json body = {{"command", command}, {"timeoutMs", timeout_ms}};
    if (!session_id.empty()) body["sessionId"] = session_id;
    if (last_line > 0) body["lastLine"] = last_line;

    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "POST", "/execute", {}, core::JsonValue::from_json(body),
            timeout_ms + 5000);
        return make_json_result(result);
    }
    auto result = http_post(base_url.value_or("http://localhost:3001") + "/execute",
                            core::JsonValue::from_json(body));
    return make_json_result(result);
}

ToolResult TerminalTool::action_input(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target,
    const std::string& session_id,
    const std::string& text
) {
    json body = {{"text", text}};
    if (!session_id.empty()) body["sessionId"] = session_id;

    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "POST", "/input", {}, core::JsonValue::from_json(body),
            DEFAULT_TERMINAL_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_post(base_url.value_or("http://localhost:3001") + "/input",
                            core::JsonValue::from_json(body));
    return make_json_result(result);
}

ToolResult TerminalTool::action_resize(
    const std::optional<std::string>& base_url,
    const std::optional<TerminalNodeTarget>& node_target,
    const std::string& session_id,
    int rows,
    int cols
) {
    json body = {{"rows", rows}, {"cols", cols}};
    if (!session_id.empty()) body["sessionId"] = session_id;

    if (node_target.has_value()) {
        auto result = call_node_terminal_proxy(
            node_target->node_id, "POST", "/resize", {}, core::JsonValue::from_json(body),
            DEFAULT_TERMINAL_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_post(base_url.value_or("http://localhost:3001") + "/resize",
                            core::JsonValue::from_json(body));
    return make_json_result(result);
}

// ============================================================================
// HTTP helpers (stub)
// ============================================================================

core::JsonValue TerminalTool::http_get(const std::string& url) const {
    (void)url;
    return core::JsonValue::from_json(json{{"ok", true}, {"stub", true}});
}

core::JsonValue TerminalTool::http_post(const std::string& url, const core::JsonValue& body) const {
    (void)url; (void)body;
    return core::JsonValue::from_json(json{{"ok", true}, {"stub", true}});
}

core::JsonValue TerminalTool::http_delete(const std::string& url) const {
    (void)url;
    return core::JsonValue::from_json(json{{"ok", true}, {"stub", true}});
}

core::JsonValue TerminalTool::call_node_terminal_proxy(
    const std::string& node_id,
    const std::string& method,
    const std::string& path,
    const core::JsonValue& query,
    const core::JsonValue& body,
    int timeout_ms
) const {
    (void)node_id; (void)method; (void)path; (void)query; (void)body; (void)timeout_ms;
    return core::JsonValue::from_json(json{{"ok", true}, {"stub", true}});
}

// ============================================================================
// Security helpers
// ============================================================================

std::string TerminalTool::wrap_terminal_output(
    const std::string& output,
    const std::string& session_id
) const {
    return "[untrusted:terminal:" + session_id + "]\n" + output;
}

ToolResult TerminalTool::format_terminal_output(
    const std::vector<std::string>& output_lines,
    int last_line,
    const std::string& session_id,
    const std::string& cwd,
    int exit_code
) const {
    std::string output;
    for (int i = last_line; i < (int)output_lines.size(); ++i) {
        output += output_lines[i] + "\n";
    }
    json result = {
        {"output",    wrap_terminal_output(output, session_id)},
        {"lastLine",  (int)output_lines.size()},
        {"exitCode",  exit_code},
        {"cwd",       cwd},
        {"sessionId", session_id},
    };
    return make_json_result(result);
}

ToolResult TerminalTool::make_error(const std::string& msg) const {
    ToolResult result;
    result.success = false;
    result.error = msg;
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = "Error: " + msg;
    result.content.push_back(content);
    return result;
}

ToolResult TerminalTool::make_json_result(const json& data) const {
    ToolResult result;
    result.success = true;
    ToolResultContent content;
    content.type = ToolResultContent::Type::Json;
    content.text = data.dump();
    result.content.push_back(content);
    return result;
}

// ============================================================================
// Factory
// ============================================================================

std::shared_ptr<Tool> TerminalToolFactory::create(const ToolContext& context) {
    return std::make_shared<TerminalTool>(context);
}

} // namespace tools
} // namespace hunterclaw
