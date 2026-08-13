// browser_tool.cc
// Strictly aligned with OpenClaw: extensions/browser/src/browser-tool.ts
//   + browser-tool.schema.ts + browser-tool-description.ts
//
// Tool: browser
// Actions: doctor | status | start | stop | profiles | importprofile | tabs |
//          open | focus | close | snapshot | screenshot | navigate | console |
//          pdf | download | waitfordownload | upload | dialog | act
// Targets: sandbox | host | node

#include "hunterclaw/tools/browser_tool.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/core/types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <set>
#include <map>
#include <vector>
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
// 🎯 对齐 OpenClaw: DEFAULT_EXISTING_SESSION_MANAGE_TIMEOUT_MS = 45_000
static constexpr int DEFAULT_EXISTING_SESSION_MANAGE_TIMEOUT_MS = 45000;

// 🎯 对齐 OpenClaw browser-tool.schema.ts BROWSER_TOOL_ACTIONS (保持顺序)
static const std::vector<std::string> BROWSER_ACTIONS = {
    "doctor", "status", "start", "stop", "profiles", "importprofile",
    "tabs", "open", "focus", "close", "snapshot", "screenshot", "navigate",
    "console", "pdf", "download", "waitfordownload", "upload", "dialog", "act"
};

// 🎯 对齐 OpenClaw browser-tool.schema.ts BROWSER_ACT_KINDS (保持顺序)
static const std::vector<std::string> BROWSER_ACT_KINDS = {
    "batch", "click", "clickCoords", "type", "press", "hover", "scrollIntoView",
    "drag", "select", "fill", "resize", "wait", "evaluate", "close"
};

// 🎯 对齐 OpenClaw browser-tool.schema.ts BROWSER_TARGETS
static const std::vector<std::string> BROWSER_TARGETS = {"sandbox", "host", "node"};
// 🎯 对齐 OpenClaw: BROWSER_SNAPSHOT_FORMATS / MODES / REFS / IMAGE_TYPES
static const std::vector<std::string> BROWSER_SNAPSHOT_FORMATS = {"aria", "ai"};
static const std::vector<std::string> BROWSER_SNAPSHOT_MODES = {"efficient"};
static const std::vector<std::string> BROWSER_SNAPSHOT_REFS = {"role", "aria"};
static const std::vector<std::string> BROWSER_IMAGE_TYPES = {"png", "jpeg"};

// 🎯 对齐 OpenClaw: EXISTING_SESSION_MANAGE_ACTIONS
static const std::set<std::string> EXISTING_SESSION_MANAGE_ACTIONS = {
    "status", "start", "stop", "profiles", "tabs", "open", "focus", "close"
};

static bool is_browser_action(const std::string& a) {
    for (const auto& x : BROWSER_ACTIONS) if (x == a) return true;
    return false;
}

// 🎯 对齐 OpenClaw: LEGACY_BROWSER_ACT_REQUEST_KEYS (保持顺序)
static const std::vector<std::string> LEGACY_BROWSER_ACT_REQUEST_KEYS = {
    "kind", "actions", "stopOnError", "targetId", "ref", "doubleClick", "button",
    "modifiers", "x", "y", "text", "submit", "slowly", "key", "delayMs",
    "startRef", "endRef", "values", "fields", "width", "height", "timeMs",
    "textGone", "selector", "url", "loadState", "fn", "timeoutMs"
};
// 🎯 对齐 OpenClaw: LEGACY_BROWSER_ACT_SHARED_REQUEST_KEYS
static const std::set<std::string> LEGACY_BROWSER_ACT_SHARED_REQUEST_KEYS = {"targetId"};

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
    // 🎯 严格对齐 OpenClaw: describeBrowserTool (browser-tool-description.ts)
    //   targetDefault 默认 host (无 sandboxBridgeUrl)，hostHint 默认允许。
    return
        "Control the browser via OpenClaw's browser control server "
        "(status/start/stop/profiles/tabs/open/snapshot/screenshot/pdf print-to-PDF/download/"
        "console logs/dialog accept-dismiss/actions incl. act:evaluate to run JS in the page). "
        "Browser choice: omit profile to use the configured default (normally the isolated "
        "OpenClaw-managed `openclaw` browser). "
        "When existing logins/cookies matter, use action=profiles to inspect available profiles, "
        "then select the appropriate profile by name. Do not assume a profile name. Use only when "
        "the task requires an existing session and the user has authorized it. "
        "Use action=importprofile on macOS to copy cookies from an authorized Chrome-family system "
        "profile into a fresh managed profile; this may show a Keychain consent prompt. "
        "For Chrome MCP existing-session profiles, omit timeoutMs on act:type, hover, scrollIntoView, "
        "drag, select, and fill; that driver rejects per-call timeout overrides for those actions. "
        "act:evaluate supports timeoutMs. "
        "When a node-hosted browser proxy is available, the tool may auto-route to it. Pin a node "
        "with node=<id|name> or target=\"node\". "
        "When using refs from snapshot (e.g. e12), keep the same tab: prefer passing targetId from "
        "the snapshot response into subsequent actions (act/click/type/etc). For tab operations, "
        "targetId also accepts tabId handles (t1) and labels from action=tabs. "
        "For multi-step browser work, login checks, stale refs, duplicate tabs, or Google Meet flows, "
        "use the bundled browser-automation skill when it is available. "
        "For stable, self-resolving refs across calls, use snapshot with refs=\"aria\" (Playwright "
        "aria-ref ids). Default refs=\"role\" are role+name-based. "
        "Repeated compatible snapshots with stable document identity mark newly appeared ref-bearing "
        "elements with [new]. "
        "navigate returns the loaded page's compact snapshot inline (efficient interactive tier; use "
        "action=snapshot for a full snapshot); do not call snapshot after navigate. Batch act results "
        "that report a cross-document navigation also include fresh page state; after a single act "
        "that triggers navigation, snapshot before using refs. "
        "Use snapshot+act for UI automation. Avoid act:wait by default; use only in exceptional cases "
        "when no reliable UI state exists. "
        "For page text, use a selector-scoped snapshot or act:evaluate that returns only relevant text "
        "or structured data, then reason over that bounded result with the active model. Use efficient "
        "snapshots for controls and action discovery; they omit most non-interactive prose. "
        "For file chooser uploads, pass the trigger ref with paths in the same upload call when "
        "available; use paths-only arming only when a later trigger is intentional. Use inputRef or "
        "element to set a file input directly. "
        "target selects browser location (sandbox|host|node). Default: host. "
        "Host target allowed.";
}

bool BrowserTool::requires_approval() const { return false; }

Tool::Schema BrowserTool::parameters() const {
    Tool::Schema schema;
    schema.type = "object";

    auto make_str = [](const std::string& desc = "") {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        if (!desc.empty()) p["description"] = desc;
        return p;
    };
    auto make_num = [](const std::string& desc = "") {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "number";
        if (!desc.empty()) p["description"] = desc;
        return p;
    };
    auto make_bool = [](const std::string& desc = "") {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "boolean";
        if (!desc.empty()) p["description"] = desc;
        return p;
    };
    auto make_enum = [](const std::vector<std::string>& values, const std::string& desc = "") {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        if (!desc.empty()) p["description"] = desc;
        core::JsonValue e = core::JsonValue::array();
        for (const auto& v : values) e.push_back(v);
        p["enum"] = e;
        return p;
    };
    auto make_str_array = [](const std::string& desc = "") {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "array";
        if (!desc.empty()) p["description"] = desc;
        core::JsonValue items = core::JsonValue::object();
        items["type"] = "string";
        p["items"] = items;
        return p;
    };
    auto make_obj_array = [](const std::string& desc = "") {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "array";
        if (!desc.empty()) p["description"] = desc;
        core::JsonValue items = core::JsonValue::object();
        items["type"] = "object";
        p["items"] = items;
        return p;
    };

    static const std::string kTabRefDesc =
        "Tab reference. Prefer suggestedTargetId, tabId, or label from tabs output; raw CDP "
        "targetId and unique raw prefixes remain supported for compatibility.";

    // 🎯 严格对齐 OpenClaw BrowserToolSchema (flattened)
    // action (required)
    schema.properties["action"]       = make_enum(BROWSER_ACTIONS, "Browser action to perform");
    schema.properties["target"]       = make_enum(BROWSER_TARGETS, "Browser location: sandbox, host, node");
    schema.properties["node"]         = make_str("Node ID or name for node-hosted browser proxy");
    schema.properties["profile"]      = make_str("Browser profile name");
    // importprofile
    schema.properties["browser"]      = make_str("System browser to import from (default: chrome)");
    schema.properties["systemProfile"] = make_str("System profile name to import (default: Default)");
    schema.properties["into"]         = make_str("Target managed profile name (default: imported)");
    schema.properties["domains"]      = make_str_array("Domains to import cookies for");
    // navigation / tabs
    schema.properties["targetUrl"]    = make_str("URL to open or navigate to");
    schema.properties["url"]          = make_str("Alias for targetUrl");
    schema.properties["targetId"]     = make_str(kTabRefDesc);
    schema.properties["label"]        = make_str("Optional label for opened tab");
    // snapshot
    schema.properties["limit"]        = make_num("Max items");
    schema.properties["maxChars"]     = make_num("Max characters");
    schema.properties["mode"]         = make_enum(BROWSER_SNAPSHOT_MODES, "Snapshot mode: efficient");
    schema.properties["snapshotFormat"] = make_enum(BROWSER_SNAPSHOT_FORMATS, "Snapshot format: aria, ai");
    schema.properties["refs"]         = make_enum(BROWSER_SNAPSHOT_REFS, "Snapshot refs mode: role (default), aria");
    schema.properties["interactive"]  = make_bool("Only interactive elements");
    schema.properties["compact"]      = make_bool("Compact snapshot");
    schema.properties["depth"]        = make_num("Snapshot depth");
    schema.properties["selector"]     = make_str("CSS selector scope");
    schema.properties["frame"]        = make_str("Frame selector");
    schema.properties["labels"]       = make_bool("Include labels overlay");
    schema.properties["urls"]         = make_bool("Include urls");
    // screenshot
    schema.properties["fullPage"]     = make_bool("Capture full page screenshot");
    schema.properties["ref"]          = make_str("Element ref for screenshot/act");
    schema.properties["path"]         = make_str("Output file path");
    schema.properties["element"]      = make_str("Element selector for screenshot/act");
    schema.properties["type"]         = make_enum(BROWSER_IMAGE_TYPES, "Screenshot type: png (default), jpeg");
    // console
    schema.properties["level"]        = make_str("Console log level filter");
    // upload
    schema.properties["paths"]        = make_str_array("File paths to upload");
    schema.properties["inputRef"]     = make_str("Input element ref for file chooser");
    schema.properties["timeoutMs"]    = make_num("Request timeout in milliseconds");
    // dialog
    schema.properties["dialogId"]     = make_str("Dialog ID");
    schema.properties["accept"]       = make_bool("Accept dialog (true) or dismiss (false)");
    schema.properties["promptText"]   = make_str("Text for prompt dialog");
    // act (legacy flattened; preferred: request={...})
    schema.properties["kind"]         = make_enum(BROWSER_ACT_KINDS,
        "Act kind: batch, click, clickCoords, type, press, hover, scrollIntoView, drag, "
        "select, fill, resize, wait, evaluate, close");
    schema.properties["actions"]      = make_obj_array("Batch sub-actions");
    schema.properties["stopOnError"]  = make_bool("Stop batch on first error");
    schema.properties["doubleClick"]  = make_bool("Double click");
    schema.properties["button"]       = make_str("Mouse button");
    schema.properties["modifiers"]    = make_str_array("Keyboard modifiers");
    schema.properties["x"]            = make_num("X coordinate (clickCoords)");
    schema.properties["y"]            = make_num("Y coordinate (clickCoords)");
    schema.properties["text"]         = make_str("Text to type");
    schema.properties["submit"]       = make_bool("Submit after typing");
    schema.properties["slowly"]       = make_bool("Type slowly");
    schema.properties["key"]          = make_str("Key to press");
    schema.properties["delayMs"]      = make_num("Delay in milliseconds");
    schema.properties["startRef"]     = make_str("Drag start element ref");
    schema.properties["endRef"]       = make_str("Drag end element ref");
    schema.properties["values"]       = make_str_array("Select option values");
    schema.properties["fields"]       = make_obj_array("Fill fields");
    schema.properties["width"]        = make_num("Width for resize");
    schema.properties["height"]       = make_num("Height for resize");
    schema.properties["timeMs"]       = make_num("Wait time in milliseconds");
    schema.properties["textGone"]     = make_str("Wait for text to disappear");
    schema.properties["loadState"]    = make_str("Load state to wait for");
    schema.properties["fn"]           = make_str("JavaScript function to evaluate");
    // request (nested act request)
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "object";
        p["description"] = "Nested act request (preferred over flattened act fields)";
        schema.properties["request"] = p;
    }

    schema.required = {"action"};
    return schema;
}

ToolResult BrowserTool::execute(const core::JsonValue& params_val) {
    try {
        json params = params_val.to_json();
        std::string action = read_str(params, "action");
        if (action.empty())
            return make_error("action is required");
        if (!is_browser_action(action))
            return make_error("Unknown action: " + action);

        std::string profile = read_str(params, "profile");
        std::string requested_node = read_str(params, "node");
        std::string target = read_str(params, "target");

        // 🎯 对齐 OpenClaw: node is only supported with target="node"
        if (!requested_node.empty() && !target.empty() && target != "node")
            return make_error("node is only supported with target=\"node\".");

        // 🎯 对齐 OpenClaw: importprofile 读取本地 Keychain/Chrome profile，只能在 host 运行。
        //   在 target/node 解析前钉死，避免落到 sandbox 或自动选中的 browser node。
        if (action == "importprofile") {
            if (target == "sandbox" || target == "node" || !requested_node.empty())
                return make_error(
                    "system profile import must run on the host; omit target or use target=\"host\".");
            target = "host";
        }

        // 🎯 对齐 OpenClaw: resolveBrowserNodeTarget
        auto node_target = resolve_browser_node_target(requested_node, target, "");

        // 🎯 对齐 OpenClaw: resolveBrowserBaseUrl (target=="node" 时 base_url 为空，走 node 代理)
        std::optional<std::string> base_url;
        if (!node_target.has_value()) {
            std::string resolved_target = (target == "node") ? "" : target;
            base_url = resolve_browser_base_url(resolved_target, "");
        }

        // Dispatch action (顺序对齐 OpenClaw switch)
        if (action == "doctor")
            return action_doctor(base_url, node_target, profile);
        if (action == "status")
            return action_status(base_url, node_target, profile);
        if (action == "start")
            return action_start(base_url, node_target, profile);
        if (action == "stop")
            return action_stop(base_url, node_target, profile);
        if (action == "profiles")
            return action_profiles(base_url, node_target);
        if (action == "importprofile")
            return action_importprofile(params_val, base_url, node_target);
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
        if (action == "download" || action == "waitfordownload")
            return action_download(action, params_val, base_url, node_target, profile);
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

    // 🎯 对齐 OpenClaw: query gateway for browser-capable nodes
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

// 🎯 对齐 OpenClaw: doctor action → GET /doctor
ToolResult BrowserTool::action_doctor(
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/doctor", {}, {}, DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_get(base_url.value_or("http://localhost:3000") + "/doctor");
    return make_json_result(result);
}

// 🎯 对齐 OpenClaw: importprofile action (host-only, POST /profiles/import)
//   readHostSystemProfiles + browserImportProfile
//   参数: browser(默认chrome) / systemProfile(默认Default) / into(默认imported) / domains
ToolResult BrowserTool::action_importprofile(
    const core::JsonValue& params_val,
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target
) {
    // 🎯 对齐 OpenClaw: system profile import 必须在 host 运行，不能走 node 代理
    if (node_target.has_value()) {
        return make_error("system profile import must run on the browser host");
    }

    json params = params_val.to_json();
    json body = json::object();
    std::string browser = read_str(params, "browser");
    body["browser"] = browser.empty() ? "chrome" : browser;
    std::string system_profile = read_str(params, "systemProfile");
    body["systemProfile"] = system_profile.empty() ? "Default" : system_profile;
    std::string into = read_str(params, "into");
    body["into"] = into.empty() ? "imported" : into;
    // domains: 数组
    if (params.contains("domains") && params["domains"].is_array()) {
        body["domains"] = params["domains"];
    }

    auto result = http_post(
        base_url.value_or("http://localhost:3000") + "/profiles/import", body);
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
    // 🎯 对齐 OpenClaw: label 透传
    // (label 通过 params 读取，这里 action_open 签名未带 params，故由调用方注入到 url 后追加)
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
    // 🎯 对齐 OpenClaw executeSnapshotAction: 透传 snapshot 相关参数
    std::string refs = read_str(params, "refs");
    if (!refs.empty()) body["refs"] = refs;  // role | aria
    std::string snapshot_format = read_str(params, "snapshotFormat");
    if (!snapshot_format.empty()) body["snapshotFormat"] = snapshot_format;  // aria | ai
    std::string mode = read_str(params, "mode");
    if (!mode.empty()) body["mode"] = mode;  // efficient
    if (params.contains("labels") && params["labels"].is_boolean())
        body["labels"] = params["labels"];
    if (params.contains("urls") && params["urls"].is_boolean())
        body["urls"] = params["urls"];
    if (params.contains("interactive") && params["interactive"].is_boolean())
        body["interactive"] = params["interactive"];
    if (params.contains("compact") && params["compact"].is_boolean())
        body["compact"] = params["compact"];
    if (params.contains("depth")) body["depth"] = params["depth"];
    if (params.contains("limit")) body["limit"] = params["limit"];
    if (params.contains("maxChars")) body["maxChars"] = params["maxChars"];
    std::string selector = read_str(params, "selector");
    if (!selector.empty()) body["selector"] = selector;
    std::string frame = read_str(params, "frame");
    if (!frame.empty()) body["frame"] = frame;

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
    // 🎯 对齐 OpenClaw: labels 字段透传
    if (params.contains("labels") && params["labels"].is_boolean())
        body["labels"] = params["labels"];
    if (params.contains("timeoutMs")) body["timeoutMs"] = params["timeoutMs"];

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
    // 🎯 对齐 OpenClaw executeConsoleAction: GET /console 带 level / targetId query
    json query = json::object();
    std::string level = read_str(params, "level");
    if (!level.empty()) query["level"] = level;
    std::string target_id = read_str(params, "targetId");
    if (!target_id.empty()) query["targetId"] = target_id;

    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "GET", "/console", query, {},
            DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_text_result(wrap_external_content(result.to_json().dump(), "browser:console", "console"));
    }
    // 拼接 query 到 URL
    std::string url = base_url.value_or("http://localhost:3000") + "/console";
    bool first = true;
    if (!level.empty()) { url += (first ? "?" : "&"); url += "level=" + level; first = false; }
    if (!target_id.empty()) { url += (first ? "?" : "&"); url += "targetId=" + target_id; first = false; }
    auto result = http_get(url);
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

// 🎯 对齐 OpenClaw: executeDownloadAction
//   download        → POST /download      (需 ref + path)
//   waitfordownload → POST /wait/download (path 可选)
ToolResult BrowserTool::action_download(
    const std::string& action,
    const core::JsonValue& params_val,
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    json params = params_val.to_json();
    std::string target_id = read_str(params, "targetId");
    std::string path = read_str(params, "path");

    std::string route;
    json body = json::object();
    if (action == "download") {
        std::string ref = read_str(params, "ref");
        if (ref.empty()) return make_error("ref required");
        if (path.empty()) return make_error("path required");
        route = "/download";
        body["ref"] = ref;
        body["path"] = path;
    } else {
        // waitfordownload
        route = "/wait/download";
        if (!path.empty()) body["path"] = path;
    }
    if (!target_id.empty()) body["targetId"] = target_id;
    if (params.contains("timeoutMs")) body["timeoutMs"] = params["timeoutMs"];

    if (node_target.has_value()) {
        auto result = call_node_browser_proxy(
            node_target->node_id, "POST", route, {}, body,
            DEFAULT_BROWSER_PROXY_TIMEOUT_MS);
        return make_json_result(result);
    }
    auto result = http_post(base_url.value_or("http://localhost:3000") + route, body);
    return make_json_result(result);
}

ToolResult BrowserTool::action_act(
    const core::JsonValue& params_val,
    const std::optional<std::string>& base_url,
    const std::optional<BrowserNodeTarget>& node_target,
    const std::string& /*profile*/
) {
    json params = params_val.to_json();

    // 🎯 严格对齐 OpenClaw: readActRequestParam
    json request = json::object();
    bool have_request = false;
    if (params.contains("request") && params["request"].is_object()) {
        request = params["request"];
        have_request = true;
        // 🎯 对齐 OpenClaw: kind 不匹配时，只有 targetId 可跨 kind 共享
        const bool has_mismatched_kind =
            request.contains("kind") && request["kind"].is_string() &&
            params.contains("kind") && params["kind"].is_string() &&
            request["kind"] != params["kind"];
        for (const auto& key : LEGACY_BROWSER_ACT_REQUEST_KEYS) {
            if (request.contains(key) || !params.contains(key)) continue;
            if (has_mismatched_kind && !LEGACY_BROWSER_ACT_SHARED_REQUEST_KEYS.count(key)) {
                continue;
            }
            request[key] = params[key];
        }
    } else {
        std::string kind = read_str(params, "kind");
        if (kind.empty()) return make_error("request required");
        for (const auto& key : LEGACY_BROWSER_ACT_REQUEST_KEYS) {
            if (params.contains(key)) request[key] = params[key];
        }
    }
    (void)have_request;

    // 🎯 对齐 OpenClaw: browserAct 提交 request 体
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
    // 🎯 对齐 OpenClaw: dialogId 透传
    std::string dialog_id = read_str(params, "dialogId");
    if (!dialog_id.empty()) body["dialogId"] = dialog_id;
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
// HTTP helpers (使用 infra::HttpClient)
// ============================================================================

core::JsonValue BrowserTool::http_get(const std::string& url) const {
    infra::HttpClient client;
    auto result = client.get(url, 10000);
    if (!result.is_ok()) { json r; r["ok"] = false; r["error"] = "fetch failed"; return core::JsonValue(r); }
    auto& resp = result.get();
    if (resp.body.empty()) { json r; r["ok"] = false; r["error"] = "empty response"; return core::JsonValue(r); }
    try { return core::JsonValue(json::parse(resp.body)); }
    catch (...) { json r; r["ok"] = true; r["body"] = resp.body; return core::JsonValue(r); }
}

core::JsonValue BrowserTool::http_post(const std::string& url, const core::JsonValue& body) const {
    infra::HttpClient client;
    infra::HttpRequestOptions opts;
    opts.body = body.to_json().dump();
    opts.headers = {{"Content-Type", "application/json"}};
    opts.timeout_ms = 10000;
    auto result = client.post(url, opts);
    if (!result.is_ok()) { json r; r["ok"] = false; return core::JsonValue(r); }
    auto& resp = result.get();
    if (resp.body.empty()) { json r; r["ok"] = false; return core::JsonValue(r); }
    try { return core::JsonValue(json::parse(resp.body)); }
    catch (...) { json r; r["ok"] = true; r["body"] = resp.body; return core::JsonValue(r); }
}

core::JsonValue BrowserTool::http_delete(const std::string& url) const {
    infra::HttpClient client;
    auto result = client.del(url, 10000);
    json r; r["ok"] = result.is_ok(); return core::JsonValue(r);
}

core::JsonValue BrowserTool::call_node_browser_proxy(
    const std::string& node_id,
    const std::string& method,
    const std::string& path,
    const core::JsonValue& query,
    const core::JsonValue& body,
    int timeout_ms
) const {
    // 🎯 对齐 OpenClaw: node.invoke with browser.proxy command
    json rpc;
    rpc["method"] = "node.invoke";
    json params;
    params["nodeId"] = node_id;
    params["command"] = "browser.proxy";
    json proxy_params;
    proxy_params["method"] = method;
    proxy_params["path"] = path;
    proxy_params["query"] = query.to_json();
    proxy_params["body"] = body.to_json();
    params["params"] = proxy_params;
    rpc["params"] = params;

    infra::HttpClient client;
    infra::HttpRequestOptions opts;
    opts.body = rpc.dump();
    opts.headers = {{"Content-Type", "application/json"}};
    opts.timeout_ms = timeout_ms;
    auto result = client.post("http://127.0.0.1:3000/v1/rpc", opts);
    if (result.is_ok()) {
        try {
            auto j = json::parse(result.get().body);
            return core::JsonValue(j.contains("result") ? j["result"] : j);
        } catch (...) {}
    }
    json err; err["ok"] = false; err["error"] = "node.invoke failed";
    return core::JsonValue(err);
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
