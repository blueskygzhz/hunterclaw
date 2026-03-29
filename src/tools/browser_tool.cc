// browser_tool.cc
// Strictly aligned with OpenClaw: src/agents/tools/browser-tool.ts
//
// Tool: browser
// Actions: status | start | stop | profiles | tabs | open | focus | close |
//          snapshot | screenshot | navigate | console | pdf | upload | dialog | act
// Targets: sandbox | host | node

#include "hunterclaw/tools/browser_tool.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/core/types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <set>
#include <map>
#include <optional>
#include <cmath>
#include <sstream>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

// ============================================================================
// Constants (aligned with OpenClaw browser-tool.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: DEFAULT_BROWSER_PROXY_TIMEOUT_MS = 20_000
static constexpr int DEFAULT_BROWSER_PROXY_TIMEOUT_MS = 20000;
// 🎯 对齐 OpenClaw: BROWSER_PROXY_GATEWAY_TIMEOUT_SLACK_MS = 5_000
static constexpr int BROWSER_PROXY_GATEWAY_TIMEOUT_SLACK_MS = 5000;

// 🎯 对齐 OpenClaw: all supported actions
static const std::set<std::string> BROWSER_ACTIONS = {
    "status", "start", "stop", "profiles", "tabs", "open", "focus", "close",
    "snapshot", "screenshot", "navigate", "console", "pdf", "upload", "dialog", "act"
};

// 🎯 对齐 OpenClaw: host-only profiles
static const std::set<std::string> HOST_ONLY_PROFILES = {"user", "chrome-relay"};

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
// BrowserTool implementation
// ============================================================================

BrowserTool::BrowserTool(const ToolContext& context)
    : context_(context) {}

std::string BrowserTool::name() const { return "browser"; }

std::string BrowserTool::description() const {
    // 🎯 对齐 OpenClaw: createBrowserTool description
    return "Control the browser via OpenClaw's browser control server "
           "(status/start/stop/profiles/tabs/open/snapshot/screenshot/actions). "
           "Browser choice: omit profile by default for the isolated OpenClaw-managed browser. "
           "For the logged-in user browser on the local host, prefer profile=\"user\". "
           "Use profile=\"chrome-relay\" only for the Chrome extension relay flow. "
           "Use snapshot+act for UI automation. "
           "target selects browser location (sandbox|host|node). Default: host.";
}

bool BrowserTool::requires_approval() const { return false; }

Tool::Schema BrowserTool::parameters() const {
    Tool::Schema schema;
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
    auto make_bool = [](const std::string& desc) {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "boolean";
        p["description"] = desc;
        return p;
    };

    // action (required)
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Browser action to perform";
        core::JsonValue e = core::JsonValue::array();
        for (const auto& a : BROWSER_ACTIONS) e.push_back(a);
        p["enum"] = e;
        schema.properties["action"] = p;
    }

    // 🎯 对齐 OpenClaw: BrowserToolSchema (flattened)
    schema.properties["profile"]     = make_str("Browser profile: openclaw (default), user, chrome-relay");
    schema.properties["node"]        = make_str("Node ID or name for node-hosted browser proxy");
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Browser location: sandbox, host (default), node";
        core::JsonValue e = core::JsonValue::array();
        e.push_back("sandbox"); e.push_back("host"); e.push_back("node");
        p["enum"] = e;
        schema.properties["target"] = p;
    }
    schema.properties["targetUrl"]   = make_str("URL to open or navigate to");
    schema.properties["url"]         = make_str("Alias for targetUrl");
    schema.properties["targetId"]    = make_str("Browser tab target ID");
    schema.properties["timeoutMs"]   = make_num("Request timeout in milliseconds");
    schema.properties["fullPage"]    = make_bool("Capture full page screenshot");
    schema.properties["ref"]         = make_str("Element ref for screenshot/act");
    schema.properties["element"]     = make_str("Element selector for screenshot/act");
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Screenshot type: png (default), jpeg";
        core::JsonValue e = core::JsonValue::array();
        e.push_back("png"); e.push_back("jpeg");
        p["enum"] = e;
        schema.properties["type"] = p;
    }
    // snapshot
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Snapshot refs mode: role (default), aria";
        core::JsonValue e = core::JsonValue::array();
        e.push_back("role"); e.push_back("aria");
        p["enum"] = e;
        schema.properties["refs"] = p;
    }
    // act
    schema.properties["kind"]        = make_str("Act kind: click, type, press, hover, drag, select, fill, resize, wait, evaluate, close");
    schema.properties["text"]        = make_str("Text to type");
    schema.properties["key"]         = make_str("Key to press");
    schema.properties["submit"]      = make_bool("Submit after typing");
    schema.properties["slowly"]      = make_bool("Type slowly");
    schema.properties["fn"]          = make_str("JavaScript function to evaluate");
    schema.properties["selector"]    = make_str("CSS selector");
    schema.properties["loadState"]   = make_str("Load state to wait for");
    schema.properties["delayMs"]     = make_num("Delay in milliseconds");
    schema.properties["width"]       = make_num("Width for resize");
    schema.properties["height"]      = make_num("Height for resize");
    // upload
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "array";
        p["description"] = "File paths to upload";
        core::JsonValue items = core::JsonValue::object();
        items["type"] = "string";
        p["items"] = items;
        schema.properties["paths"] = p;
    }
    schema.properties["inputRef"]    = make_str("Input element ref for file chooser");
    // dialog
    schema.properties["accept"]      = make_bool("Accept dialog (true) or dismiss (false)");
    schema.properties["promptText"]  = make_str("Text for prompt dialog");
    // console
    schema.properties["includeErrors"] = make_bool("Include console errors");
    schema.properties["includeWarnings"] = make_bool("Include console warnings");

    schema.required = {"action"};
    return schema;
}

ToolResult BrowserTool::execute(const core::JsonValue& params_val) {
    try {
        json params = params_val.to_json();
        std::string action = read_str(params, "action");
        if (action.empty())
            return make_error("action is required");
        if (!BROWSER_ACTIONS.count(action))
            return make_error("Unknown action: " + action);

        std::string profile = read_str(params, "profile");
        std::string requested_node = read_str(params, "node");
        std::string target = read_str(params, "target");

        // 🎯 对齐 OpenClaw: node is only supported with target="node"
        if (!requested_node.empty() && !target.empty() && target != "node")
            return make_error("node is only supported with target=\"node\".");

        // 🎯 对齐 OpenClaw: host-only profiles
        bool is_host_only = HOST_ONLY_PROFILES.count(profile) > 0;
        if (is_host_only) {
            if (!requested_node.empty() || target == "node")
                return make_error("profile=\"" + profile + "\" only supports the local host browser.");
            if (target == "sandbox")
                return make_error("profile=\"" + profile + "\" cannot use the sandbox browser.");
        }

        // 🎯 对齐 OpenClaw: resolveBrowserNodeTarget
        auto node_target = resolve_browser_node_target(requested_node, target, "");

        // 🎯 对齐 OpenClaw: resolveBrowserBaseUrl
        std::optional<std::string> base_url;
        if (!node_target.has_value()) {
            base_url = resolve_browser_base_url(target, "");
        }

        // Dispatch action
        if (action == "status")
            return action_status(base_url, node_target, profile);
        if (action == "start")
            return action_start(base_url, node_target, profile);
        if (action == "stop")
            return action_stop(base_url, node_target, profile);
        if (action == "profiles")
            return action_profiles(base_url, node_target);
        if (action == "tabs")
            return action_tabs(base_url, node_target, profile);
        if (action == "open") {
            std::string url = read_str(params, "targetUrl");
            if (url.empty()) url = read_str(params, "url");
            if (url.empty()) return make_error("targetUrl required");
            return action_open(base_url, node_target, profile, url);
        }
        if (action == "focus") {
            std::string target_id = read_str(params, "targetId");
            if (target_id.empty()) return make_error("targetId required");
            return action_focus(base_url, node_target, profile, target_id);
        }
        if (action == "close") {
            std::string target_id = read_str(params, "targetId");
            return action_close(base_url, node_target, profile, target_id);
        }
        if (action == "navigate") {
            std::string url = read_str(params, "targetUrl");
            if (url.empty()) url = read_str(params, "url");
            if (url.empty()) return make_error("targetUrl required");
            std::string target_id = read_str(params, "targetId");
            return action_navigate(base_url, node_target, profile, url, target_id);
        }
        if (action == "snapshot")
            return action_snapshot(params_val, base_url, node_target, profile);
        if (action == "screenshot")
            return action_screenshot(params_val, base_url, node_target, profile);
        if (action == "console")
            return action_console(params_val, base_url, node_target, profile);
        if (action == "pdf")
            return action_pdf(params_val, base_url, node_target, profile);
        if (action == "act")
            return action_act(params_val, base_url, node_target, profile);
        if (action == "upload")
            return action_upload(params_val, base_url, node_target, profile);
        if (action == "dialog")
            return action_dialog(params_val, base_url, node_target, profile);

        return make_error("Unknown action: " + action);

    } catch (const std::exception& e) {
        return make_error(std::string(e.what()));
    }
}

// ============================================================================
// Browser connection management
// ============================================================================

std::optional<std::string> BrowserTool::resolve_browser_base_url(
    const std::string& target,
    const std::string& /*sandbox_bridge_url*/
) const {
    // 🎯 对齐 OpenClaw: resolveBrowserBaseUrl
    // For host target, use the local browser control server
    if (target == "host" || target.empty()) {
        // Default browser control server URL
        const char* env = std::getenv("BROWSER_CONTROL_URL");
        if (env) return std::string(env);
        return std::string("http://localhost:3000");
    }
    return std::nullopt;
}

std::optional<BrowserTool::BrowserNodeTarget> BrowserTool::resolve_browser_node_target(
    const std::string& requested_node,
    const std::string& target,
    const std::string& /*sandbox_bridge_url*/
) const {
    // 🎯 对齐 OpenClaw: resolveBrowserNodeTarget
    if (target != "node" && requested_node.empty()) return std::nullopt;
    if (requested_node.empty() && target != "node") return std::nullopt;

    // TODO: query gateway for browser-capable nodes
    // For now, return a stub node target if explicitly requested
    if (!requested_node.empty()) {
        return BrowserNodeTarget{requested_node, requested_node};
    }
    return std::nullopt;
}

// ============================================================================
// Action implementations
// ============================================================================

ToolResult BrowserTool::action_status(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& profile
) {
    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/", {}, {}, DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_get(base_url.value_or("http://localhost:3000") + "/status");
    return make_json_result(result);
}

ToolResult BrowserTool::action_start(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    if (node_target.has_value()) {
        call_node_browser_proxy(
            node_target->node_id, "POST", "/start", {}, {}, DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/", {}, {}, DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(result);
    }
    http_post(base_url.value_or("http://localhost:3000") + "/start", {});
    auto result = http_get(base_url.value_or("http://localhost:3000") + "/status");
    return make_json_result(result);
}

ToolResult BrowserTool::action_stop(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    if (node_target.has_value()) {
        call_node_browser_proxy(
            node_target->node_id, "POST", "/stop", {}, {}, DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/", {}, {}, DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(result);
    }
    http_post(base_url.value_or("http://localhost:3000") + "/stop", {});
    auto result = http_get(base_url.value_or("http://localhost:3000") + "/status");
    return make_json_result(result);
}

ToolResult BrowserTool::action_profiles(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target
) {
    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/profiles", {}, {}, DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_get(base_url.value_or("http://localhost:3000") + "/profiles");
    return make_json_result(result);
}

ToolResult BrowserTool::action_tabs(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/tabs", {}, {}, DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_get(base_url.value_or("http://localhost:3000") + "/tabs");
    return make_json_result(result);
}

ToolResult BrowserTool::action_open(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/,
    const std::string& target_url
) {
    json body = {{"url", target_url}};
    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "POST", "/tabs/open", {}, body,
            DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_post(base_url.value_or("http://localhost:3000") + "/tabs/open", body);
    return make_json_result(result);
}

ToolResult BrowserTool::action_focus(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/,
    const std::string& target_id
) {
    json body = {{"targetId", target_id}};
    if (node_target.has_value()) {
        call_node_browser_proxy(
            node_target->node_id, "POST", "/tabs/focus", {}, body,
            DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(json{{"ok", true}});
    }
    http_post(base_url.value_or("http://localhost:3000") + "/tabs/focus", body);
    return make_json_result(json{{"ok", true}});
}

ToolResult BrowserTool::action_close(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/,
    const std::string& target_id
) {
    if (node_target.has_value()) {
        if (!target_id.empty()) {
            call_node_browser_proxy(
                node_target->node_id, "DELETE",
                "/tabs/" + target_id, {}, {}, DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        } else {
            call_node_browser_proxy(
                node_target->node_id, "POST", "/act", {},
                json{{"kind", "close"}}, DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        }
        return make_json_result(json{{"ok", true}});
    }
    if (!target_id.empty()) {
        http_delete(base_url.value_or("http://localhost:3000") + "/tabs/" + target_id);
    } else {
        http_post(base_url.value_or("http://localhost:3000") + "/act", json{{"kind", "close"}});
    }
    return make_json_result(json{{"ok", true}});
}

ToolResult BrowserTool::action_navigate(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/,
    const std::string& target_url,
    const std::string& target_id
) {
    json body = {{"url", target_url}};
    if (!target_id.empty()) body["targetId"] = target_id;
    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "POST", "/navigate", {}, body,
            DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_post(base_url.value_or("http://localhost:3000") + "/navigate", body);
    return make_json_result(result);
}

ToolResult BrowserTool::action_snapshot(
    const core::JsonValue& params_val,
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    json params = params_val.to_json();
    json body = json::object();
    std::string target_id = read_str(params, "targetId");
    if (!target_id.empty()) body["targetId"] = target_id;
    std::string refs = read_str(params, "refs");
    if (!refs.empty()) body["refs"] = refs;

    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "POST", "/snapshot", {}, body,
            DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        // 🎯 对齐 OpenClaw: wrap as untrusted
        return make_text_result(wrap_external_content(result.to_json().dump(), "browser:snapshot", "snapshot"));
    }
    auto result = http_post(base_url.value_or("http://localhost:3000") + "/snapshot", body);
    return make_text_result(wrap_external_content(result.to_json().dump(), "browser:snapshot", "snapshot"));
}

ToolResult BrowserTool::action_screenshot(
    const core::JsonValue& params_val,
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    json params = params_val.to_json();
    json body = json::object();
    std::string target_id = read_str(params, "targetId");
    if (!target_id.empty()) body["targetId"] = target_id;
    bool full_page = params.value("fullPage", false);
    body["fullPage"] = full_page;
    std::string ref = read_str(params, "ref");
    if (!ref.empty()) body["ref"] = ref;
    std::string element = read_str(params, "element");
    if (!element.empty()) body["element"] = element;
    std::string type = read_str(params, "type");
    body["type"] = (type == "jpeg") ? "jpeg" : "png";

    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "POST", "/screenshot", {}, body,
            DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        // Return image if available
        if (result.contains("path") && result["path"].is_string()) {
            ToolResult tool_result;
            tool_result.success = true;
            ToolResultContent content;
            content.type = ToolResultContent::Type::Text;
            content.text = "FILE:" + result["path"].get<std::string>();
            tool_result.content.push_back(content);
            return tool_result;
        }
        return make_json_result(result);
    }
    auto result = http_post(base_url.value_or("http://localhost:3000") + "/screenshot", body);
    if (result.contains("path") && result["path"].is_string()) {
        ToolResult tool_result;
        tool_result.success = true;
        ToolResultContent content;
        content.type = ToolResultContent::Type::Text;
        content.text = "FILE:" + result["path"].get<std::string>();
        tool_result.content.push_back(content);
        return tool_result;
    }
    return make_json_result(result);
}

ToolResult BrowserTool::action_console(
    const core::JsonValue& params_val,
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    json params = params_val.to_json();
    json body = json::object();
    std::string target_id = read_str(params, "targetId");
    if (!target_id.empty()) body["targetId"] = target_id;
    if (params.contains("includeErrors")) body["includeErrors"] = params["includeErrors"];
    if (params.contains("includeWarnings")) body["includeWarnings"] = params["includeWarnings"];

    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/console", {}, body,
            DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_text_result(wrap_external_content(result.to_json().dump(), "browser:console", "console"));
    }
    auto result = http_get(base_url.value_or("http://localhost:3000") + "/console");
    return make_text_result(wrap_external_content(result.to_json().dump(), "browser:console", "console"));
}

ToolResult BrowserTool::action_pdf(
    const core::JsonValue& params_val,
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    json params = params_val.to_json();
    json body = json::object();
    std::string target_id = read_str(params, "targetId");
    if (!target_id.empty()) body["targetId"] = target_id;

    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "POST", "/pdf", {}, body,
            DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        if (result.contains("path") && result["path"].is_string()) {
            ToolResult tool_result;
            tool_result.success = true;
            ToolResultContent content;
            content.type = ToolResultContent::Type::Text;
            content.text = "FILE:" + result["path"].get<std::string>();
            tool_result.content.push_back(content);
            return tool_result;
        }
        return make_json_result(result);
    }
    auto result = http_post(base_url.value_or("http://localhost:3000") + "/pdf", body);
    if (result.contains("path") && result["path"].is_string()) {
        ToolResult tool_result;
        tool_result.success = true;
        ToolResultContent content;
        content.type = ToolResultContent::Type::Text;
        content.text = "FILE:" + result["path"].get<std::string>();
        tool_result.content.push_back(content);
        return tool_result;
    }
    return make_json_result(result);
}

ToolResult BrowserTool::action_act(
    const core::JsonValue& params_val,
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    json params = params_val.to_json();

    // 🎯 对齐 OpenClaw: readActRequestParam
    json request = json::object();
    if (params.contains("request") && params["request"].is_object()) {
        request = params["request"];
    } else {
        std::string kind = read_str(params, "kind");
        if (kind.empty()) return make_error("request or kind required for act");
        request["kind"] = kind;
        // Copy legacy keys
        static const std::vector<std::string> legacy_keys = {
            "targetId", "ref", "doubleClick", "button", "modifiers", "text",
            "submit", "slowly", "key", "delayMs", "startRef", "endRef",
            "values", "fields", "width", "height", "timeMs", "textGone",
            "selector", "url", "loadState", "fn", "timeoutMs"
        };
        for (const auto& k : legacy_keys) {
            if (params.contains(k)) request[k] = params[k];
        }
    }

    json body = {{"request", request}};
    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "POST", "/act", {}, body,
            DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_post(base_url.value_or("http://localhost:3000") + "/act", body);
    return make_json_result(result);
}

ToolResult BrowserTool::action_upload(
    const core::JsonValue& params_val,
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    json params = params_val.to_json();
    if (!params.contains("paths") || !params["paths"].is_array())
        return make_error("paths required");
    json paths = params["paths"];
    if (paths.empty()) return make_error("paths must not be empty");

    json body = {{"paths", paths}};
    std::string ref = read_str(params, "ref");
    if (!ref.empty()) body["ref"] = ref;
    std::string input_ref = read_str(params, "inputRef");
    if (!input_ref.empty()) body["inputRef"] = input_ref;
    std::string element = read_str(params, "element");
    if (!element.empty()) body["element"] = element;
    std::string target_id = read_str(params, "targetId");
    if (!target_id.empty()) body["targetId"] = target_id;
    if (params.contains("timeoutMs")) body["timeoutMs"] = params["timeoutMs"];

    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "POST", "/hooks/file-chooser", {}, body,
            DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_post(
        base_url.value_or("http://localhost:3000") + "/hooks/file-chooser", body);
    return make_json_result(result);
}

ToolResult BrowserTool::action_dialog(
    const core::JsonValue& params_val,
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    json params = params_val.to_json();
    bool accept = params.value("accept", false);
    json body = {{"accept", accept}};
    if (params.contains("promptText") && params["promptText"].is_string())
        body["promptText"] = params["promptText"];
    std::string target_id = read_str(params, "targetId");
    if (!target_id.empty()) body["targetId"] = target_id;
    if (params.contains("timeoutMs")) body["timeoutMs"] = params["timeoutMs"];

    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "POST", "/hooks/dialog", {}, body,
            DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_post(
        base_url.value_or("http://localhost:3000") + "/hooks/dialog", body);
    return make_json_result(result);
}

// ============================================================================
// HTTP helpers (stub - real implementation via HttpClient)
// ============================================================================

core::JsonValue BrowserTool::http_get(const std::string& url) const {
    // TODO: use real HTTP client
    (void)url;
    return core::JsonValue(json{{"ok", true}, {"stub", true}, {"url", url}});
}

core::JsonValue BrowserTool::http_post(const std::string& url, const core::JsonValue& body) const {
    (void)url; (void)body;
    return core::JsonValue(json{{"ok", true}, {"stub", true}});
}

core::JsonValue BrowserTool::http_delete(const std::string& url) const {
    (void)url;
    return core::JsonValue(json{{"ok", true}, {"stub", true}});
}

core::JsonValue BrowserTool::call_node_browser_proxy(
    const std::string& node_id,
    const std::string& method,
    const std::string& path,
    const core::JsonValue& query,
    const core::JsonValue& body,
    int timeout_ms
) const {
    // TODO: call gateway "node.invoke" with browser.proxy command
    (void)node_id; (void)method; (void)path; (void)query; (void)body; (void)timeout_ms;
    return core::JsonValue(json{{"ok", true}, {"stub", true}});
}

// ============================================================================
// Security helpers
// ============================================================================

std::string BrowserTool::wrap_external_content(
    const std::string& content,
    const std::string& source,
    const std::string& kind
) const {
    // 🎯 对齐 OpenClaw: wrap external content as untrusted
    return "[untrusted:" + source + ":" + kind + "]\n" + content;
}

ToolResult BrowserTool::format_tabs_result(
    const std::vector<core::JsonValue>& tabs
) const {
    json tabs_arr = json::array();
    for (const auto& tab : tabs) {
        tabs_arr.push_back(tab.to_json());
    }
    return make_json_result(json{{"tabs", tabs_arr}});
}

// ============================================================================
// Result helpers
// ============================================================================

ToolResult BrowserTool::make_error(const std::string& msg) const {
    ToolResult result;
    result.success = false;
    result.error = msg;
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = "Error: " + msg;
    result.content.push_back(content);
    return result;
}

ToolResult BrowserTool::make_json_result(const core::JsonValue& data) const {
    ToolResult result;
    result.success = true;
    ToolResultContent content;
    content.type = ToolResultContent::Type::Json;
    content.text = data.to_json().dump();
    result.content.push_back(content);
    return result;
}

ToolResult BrowserTool::make_text_result(const std::string& text) const {
    ToolResult result;
    result.success = true;
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = text;
    result.content.push_back(content);
    return result;
}

// ============================================================================
// Factory
// ============================================================================

std::shared_ptr<Tool> BrowserToolFactory::create(const ToolContext& context) {
    return std::make_shared<BrowserTool>(context);
}

} // namespace tools
} // namespace hunterclaw
