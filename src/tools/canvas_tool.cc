// canvas_tool.cc
// Strictly aligned with OpenClaw: src/agents/tools/canvas-tool.ts
//
// Tool: canvas
// Actions: present | hide | navigate | eval | snapshot | a2ui_push | a2ui_reset

#include "hunterclaw/tools/canvas_tool.h"
#include "hunterclaw/tools/tool.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <set>
#include <cmath>
#include <fstream>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

namespace {

// ============================================================================
// 🎯 对齐 OpenClaw: CANVAS_ACTIONS
// ============================================================================
const std::set<std::string> CANVAS_ACTIONS = {
    "present", "hide", "navigate", "eval", "snapshot", "a2ui_push", "a2ui_reset"
};

// 🎯 对齐 OpenClaw: CANVAS_SNAPSHOT_FORMATS
const std::set<std::string> CANVAS_SNAPSHOT_FORMATS = {"png", "jpg", "jpeg"};

// ============================================================================
// Helper: read string param
// ============================================================================
static std::string read_str(const json& params, const std::string& key, bool required = false) {
    auto it = params.find(key);
    if (it == params.end() || !it->is_string()) {
        if (required) throw std::runtime_error(key + " is required");
        return "";
    }
    std::string s = it->get<std::string>();
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        if (required) throw std::runtime_error(key + " is required");
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ============================================================================
// CanvasTool
// ============================================================================
class CanvasTool : public Tool {
public:
    explicit CanvasTool(CanvasToolOptions opts)
        : opts_(std::move(opts)) {}

    // 🎯 对齐 OpenClaw: name: "canvas"
    std::string name() const override { return "canvas"; }

    // 🎯 对齐 OpenClaw: description
    std::string description() const override {
        return "Control node canvases (present/hide/navigate/eval/snapshot/A2UI). "
               "Use snapshot to capture the rendered UI.";
    }

    // 🎯 对齐 OpenClaw: CanvasToolSchema (flattened)
    Schema parameters() const override {
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
            p["description"] = "Canvas action to perform";
            core::JsonValue e = core::JsonValue::array();
            for (const auto& a : CANVAS_ACTIONS) e.push_back(a);
            p["enum"] = e;
            schema.properties["action"] = p;
        }

        schema.properties["gatewayUrl"]   = make_str("Gateway URL override");
        schema.properties["gatewayToken"] = make_str("Gateway token override");
        schema.properties["timeoutMs"]    = make_num("Request timeout in milliseconds");
        schema.properties["node"]         = make_str("Node ID (optional, auto-resolved if omitted)");
        // present
        schema.properties["target"]       = make_str("URL or target to present (for present/navigate)");
        schema.properties["x"]            = make_num("X position for present placement");
        schema.properties["y"]            = make_num("Y position for present placement");
        schema.properties["width"]        = make_num("Width for present placement");
        schema.properties["height"]       = make_num("Height for present placement");
        // navigate
        schema.properties["url"]          = make_str("URL to navigate to");
        // eval
        schema.properties["javaScript"]   = make_str("JavaScript code to evaluate");
        // snapshot
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            p["description"] = "Output format: png, jpg, jpeg (default: png)";
            core::JsonValue e = core::JsonValue::array();
            for (const auto& f : CANVAS_SNAPSHOT_FORMATS) e.push_back(f);
            p["enum"] = e;
            schema.properties["outputFormat"] = p;
        }
        schema.properties["maxWidth"]     = make_num("Max width for snapshot (pixels)");
        schema.properties["quality"]      = make_num("JPEG quality (0-100)");
        schema.properties["delayMs"]      = make_num("Delay before snapshot (ms)");
        // a2ui_push
        schema.properties["jsonl"]        = make_str("JSONL string for a2ui_push");
        schema.properties["jsonlPath"]    = make_str("Path to JSONL file for a2ui_push");

        schema.required = {"action"};
        return schema;
    }

    // 🎯 对齐 OpenClaw: execute
    ToolResult execute(const core::JsonValue& params_val) override {
        try {
            json params = params_val.to_json();
            if (auto log = spdlog::get("default")) log->debug("[CanvasTool] execute: {}", params.dump());

            std::string action = read_str(params, "action");
            if (action.empty())
                return make_error("action is required");
            if (!CANVAS_ACTIONS.count(action))
                return make_error("Unknown action: " + action);

            // Gateway opts
            std::string gw_url   = read_str(params, "gatewayUrl");
            std::string gw_token = read_str(params, "gatewayToken");
            int64_t timeout_ms = 60000;
            if (params.contains("timeoutMs") && params["timeoutMs"].is_number()) {
                double v = params["timeoutMs"].get<double>();
                if (std::isfinite(v)) timeout_ms = (int64_t)v;
            }

            // Node ID
            std::string node_id = read_str(params, "node");
            // 🎯 对齐 OpenClaw: resolveNodeId(gatewayOpts, node, true)
            // If node_id is empty, auto-resolve from gateway
            if (node_id.empty()) {
                node_id = resolve_node_id(gw_url, gw_token, timeout_ms);
            }

            // ----------------------------------------------------------------
            // 🎯 case "present"
            // ----------------------------------------------------------------
            if (action == "present") {
                json invoke_params = json::object();
                // 🎯 对齐 OpenClaw: Accept both `target` and `url` for present
                std::string present_target = read_str(params, "target");
                if (present_target.empty()) present_target = read_str(params, "url");
                if (!present_target.empty()) invoke_params["url"] = present_target;

                // Placement
                json placement = json::object();
                bool has_placement = false;
                if (params.contains("x") && params["x"].is_number()) {
                    placement["x"] = params["x"].get<double>(); has_placement = true;
                }
                if (params.contains("y") && params["y"].is_number()) {
                    placement["y"] = params["y"].get<double>(); has_placement = true;
                }
                if (params.contains("width") && params["width"].is_number()) {
                    placement["width"] = params["width"].get<double>(); has_placement = true;
                }
                if (params.contains("height") && params["height"].is_number()) {
                    placement["height"] = params["height"].get<double>(); has_placement = true;
                }
                if (has_placement) invoke_params["placement"] = placement;

                invoke_node(node_id, "canvas.present", invoke_params, gw_url, gw_token, timeout_ms);
                return make_json_result({{"ok", true}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "hide"
            // ----------------------------------------------------------------
            if (action == "hide") {
                invoke_node(node_id, "canvas.hide", json::object(), gw_url, gw_token, timeout_ms);
                return make_json_result({{"ok", true}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "navigate"
            // ----------------------------------------------------------------
            if (action == "navigate") {
                // 🎯 对齐 OpenClaw: Support `target` as alias for `url`
                std::string url = read_str(params, "url");
                if (url.empty()) url = read_str(params, "target");
                if (url.empty())
                    return make_error("url is required for navigate");
                invoke_node(node_id, "canvas.navigate", {{"url", url}}, gw_url, gw_token, timeout_ms);
                return make_json_result({{"ok", true}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "eval"
            // ----------------------------------------------------------------
            if (action == "eval") {
                std::string js = read_str(params, "javaScript", true);
                auto raw = invoke_node(node_id, "canvas.eval", {{"javaScript", js}},
                                       gw_url, gw_token, timeout_ms);
                // 🎯 对齐 OpenClaw: const result = raw?.payload?.result
                if (raw.contains("payload") && raw["payload"].contains("result")) {
                    std::string result_str = raw["payload"]["result"].is_string()
                        ? raw["payload"]["result"].get<std::string>()
                        : raw["payload"]["result"].dump();
                    if (!result_str.empty()) {
                        ToolResult tool_result;
                        tool_result.success = true;
                        ToolResultContent content;
                        content.type = ToolResultContent::Type::Text;
                        content.text = result_str;
                        tool_result.content.push_back(content);
                        return tool_result;
                    }
                }
                return make_json_result({{"ok", true}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "snapshot"
            // ----------------------------------------------------------------
            if (action == "snapshot") {
                std::string format_raw = read_str(params, "outputFormat");
                if (format_raw.empty()) format_raw = "png";
                // 🎯 对齐 OpenClaw: format = formatRaw === "jpg" || "jpeg" ? "jpeg" : "png"
                std::string format = (format_raw == "jpg" || format_raw == "jpeg") ? "jpeg" : "png";

                json invoke_params = {{"format", format}};
                if (params.contains("maxWidth") && params["maxWidth"].is_number())
                    invoke_params["maxWidth"] = params["maxWidth"].get<double>();
                if (params.contains("quality") && params["quality"].is_number())
                    invoke_params["quality"] = params["quality"].get<double>();

                auto raw = invoke_node(node_id, "canvas.snapshot", invoke_params,
                                       gw_url, gw_token, timeout_ms);

                // 🎯 对齐 OpenClaw: parseCanvasSnapshotPayload(raw?.payload)
                std::string base64_data;
                std::string mime_type = (format == "jpeg") ? "image/jpeg" : "image/png";

                if (raw.contains("payload")) {
                    const auto& payload = raw["payload"];
                    if (payload.contains("base64") && payload["base64"].is_string())
                        base64_data = payload["base64"].get<std::string>();
                    if (payload.contains("format") && payload["format"].is_string()) {
                        std::string fmt = payload["format"].get<std::string>();
                        mime_type = (fmt == "jpeg") ? "image/jpeg" : "image/png";
                    }
                }

                if (!base64_data.empty()) {
                    ToolResult tool_result;
                    tool_result.success = true;
                    ToolResultContent content;
                    content.type = ToolResultContent::Type::Image;
                    content.data = base64_data;
                    content.mime_type = mime_type;
                    tool_result.content.push_back(content);
                    return tool_result;
                }
                return make_json_result({{"ok", true}, {"note", "snapshot returned no data"}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "a2ui_push"
            // ----------------------------------------------------------------
            if (action == "a2ui_push") {
                std::string jsonl = read_str(params, "jsonl");
                if (jsonl.empty()) {
                    // 🎯 对齐 OpenClaw: jsonlPath fallback
                    std::string jsonl_path = read_str(params, "jsonlPath");
                    if (!jsonl_path.empty()) {
                        // Read from file
                        jsonl = read_jsonl_from_path(jsonl_path);
                    }
                }
                if (jsonl.empty())
                    return make_error("jsonl or jsonlPath required");
                invoke_node(node_id, "canvas.a2ui.pushJSONL", {{"jsonl", jsonl}},
                            gw_url, gw_token, timeout_ms);
                return make_json_result({{"ok", true}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "a2ui_reset"
            // ----------------------------------------------------------------
            if (action == "a2ui_reset") {
                invoke_node(node_id, "canvas.a2ui.reset", json::object(),
                            gw_url, gw_token, timeout_ms);
                return make_json_result({{"ok", true}});
            }

            return make_error("Unknown action: " + action);

        } catch (const std::exception& e) {
            if (auto log = spdlog::get("default")) log->error("[CanvasTool] exception: {}", e.what());
            return make_error(std::string(e.what()));
        }
    }

private:
    CanvasToolOptions opts_;

    ToolResult make_error(const std::string& msg) const {
        ToolResult result;
        result.success = false;
        result.error = msg;
        ToolResultContent content;
        content.type = ToolResultContent::Type::Text;
        content.text = "Error: " + msg;
        result.content.push_back(content);
        return result;
    }

    ToolResult make_json_result(const json& data) const {
        ToolResult result;
        result.success = true;
        ToolResultContent content;
        content.type = ToolResultContent::Type::Json;
        content.text = data.dump();
        result.content.push_back(content);
        return result;
    }

    // 🎯 对齐 OpenClaw: resolveNodeId(gatewayOpts, node, true)
    // Auto-resolve node ID from gateway if not provided
    std::string resolve_node_id(
        const std::string& gw_url,
        const std::string& gw_token,
        int64_t timeout_ms
    ) const {
        // 🎯 对齐 OpenClaw: resolveNodeId — 从 gateway 获取第一个可用节点
        if (auto log = spdlog::get("default")) log->debug("[CanvasTool] resolve_node_id: auto-resolving from gateway");

        json rpc = {{"method", "node.list"}};
        std::string url = (gw_url.empty() ? "http://127.0.0.1:3000" : gw_url) + "/v1/rpc";
        infra::HttpClient client;
        infra::HttpRequestOptions opts;
        opts.body = rpc.dump();
        opts.headers = {{"Content-Type", "application/json"}};
        if (!gw_token.empty()) opts.headers["Authorization"] = "Bearer " + gw_token;
        opts.timeout_ms = static_cast<int>(timeout_ms);

        auto result = client.post(url, opts);
        if (result.is_ok()) {
            try {
                auto j = json::parse(result.get().body);
                auto& nodes = j.contains("result") ? j["result"]["nodes"] : j["nodes"];
                if (nodes.is_array() && !nodes.empty()) {
                    return nodes[0].value("id", std::string{});
                }
            } catch (...) {}
        }
        return "";
    }

    // 🎯 对齐 OpenClaw: invoke(command, invokeParams)
    json invoke_node(
        const std::string& node_id,
        const std::string& command,
        const json& invoke_params,
        const std::string& gw_url,
        const std::string& gw_token,
        int64_t timeout_ms
    ) const {
        // 🎯 对齐 OpenClaw: node.invoke via HTTP RPC
        if (auto log = spdlog::get("default")) log->info("[CanvasTool] invoke_node: nodeId={}, command={}", node_id, command);

        json rpc = {{"method", "node.invoke"}, {"params", {
            {"nodeId", node_id}, {"command", command}, {"params", invoke_params}
        }}};
        std::string url = (gw_url.empty() ? "http://127.0.0.1:3000" : gw_url) + "/v1/rpc";
        infra::HttpClient client;
        infra::HttpRequestOptions opts;
        opts.body = rpc.dump();
        opts.headers = {{"Content-Type", "application/json"}};
        if (!gw_token.empty()) opts.headers["Authorization"] = "Bearer " + gw_token;
        opts.timeout_ms = static_cast<int>(timeout_ms);

        auto result = client.post(url, opts);
        if (result.is_ok()) {
            try {
                auto j = json::parse(result.get().body);
                return j.contains("result") ? j["result"] : j;
            } catch (...) {}
        }
        return {{"ok", false}, {"error", result.is_ok() ? "parse error" : result.error}};
    }

    // 🎯 对齐 OpenClaw: readJsonlFromPath
    std::string read_jsonl_from_path(const std::string& path) const {
        // 🎯 对齐 OpenClaw: isInboundPathAllowed — 路径安全检查
        // 禁止路径穿越和访问敏感目录
        if (path.find("..") != std::string::npos) {
            throw std::runtime_error("Path traversal not allowed: " + path);
        }
        // 禁止访问 /etc, /proc, /sys 等系统目录
        for (const auto& banned : {"/etc/", "/proc/", "/sys/", "/dev/"}) {
            if (path.find(banned) == 0) {
                throw std::runtime_error("Access to system path not allowed: " + path);
            }
        }
        try {
            std::ifstream f(path);
            if (!f.is_open()) throw std::runtime_error("Cannot open file: " + path);
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            return content;
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("jsonlPath read failed: ") + e.what());
        }
    }
};

} // namespace

// ============================================================================
// Factory
// ============================================================================

std::shared_ptr<Tool> create_canvas_tool(const CanvasToolOptions& opts) {
    return std::make_shared<CanvasTool>(opts);
}

} // namespace tools
} // namespace hunterclaw
