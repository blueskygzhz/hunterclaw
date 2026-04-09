// nodes_tool.cc
// Strictly aligned with OpenClaw: src/agents/tools/nodes-tool.ts
//
// Tool: nodes
// Actions: status | describe | pending | approve | reject | notify |
//          camera_snap | camera_list | camera_clip | photos_latest |
//          screen_record | location_get | notifications_list |
//          notifications_action | device_status | device_info |
//          device_permissions | device_health | run | invoke
// ownerOnly: true

#include "hunterclaw/tools/nodes_tool.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/tools/tool.h"
#include <nlohmann/json.hpp>
#include <string>
#include <set>
#include <map>
#include <cmath>
#include <fstream>
#include <sstream>
#include <random>
#include <algorithm>
#include <cstdio>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

namespace {

// ============================================================================
// 🎯 对齐 OpenClaw: NODES_TOOL_ACTIONS
// ============================================================================
const std::set<std::string> NODES_TOOL_ACTIONS = {
    "status", "describe", "pending", "approve", "reject", "notify",
    "camera_snap", "camera_list", "camera_clip", "photos_latest",
    "screen_record", "location_get", "notifications_list",
    "notifications_action", "device_status", "device_info",
    "device_permissions", "device_health", "run", "invoke",
};

// 🎯 对齐 OpenClaw: NODE_READ_ACTION_COMMANDS
const std::map<std::string, std::string> NODE_READ_ACTION_COMMANDS = {
    {"camera_list",          "camera.list"},
    {"notifications_list",   "notifications.list"},
    {"device_status",        "device.status"},
    {"device_info",          "device.info"},
    {"device_permissions",   "device.permissions"},
    {"device_health",        "device.health"},
};

// 🎯 对齐 OpenClaw: MEDIA_INVOKE_ACTIONS (blocked in invoke action)
const std::map<std::string, std::string> MEDIA_INVOKE_ACTIONS = {
    {"camera.snap",    "camera_snap"},
    {"camera.clip",    "camera_clip"},
    {"photos.latest",  "photos_latest"},
    {"screen.record",  "screen_record"},
};

// 🎯 对齐 OpenClaw: DEFAULT_PHOTOS_LIMIT = 1, MAX_PHOTOS_LIMIT = 20
constexpr int DEFAULT_PHOTOS_LIMIT    = 1;
constexpr int MAX_PHOTOS_LIMIT        = 20;
constexpr double DEFAULT_PHOTOS_MAX_WIDTH = 1600.0;
constexpr double DEFAULT_PHOTOS_QUALITY   = 0.85;

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
// NodesTool
// ============================================================================
class NodesTool : public Tool {
public:
    explicit NodesTool(NodesToolOptions opts)
        : opts_(std::move(opts)) {}

    // 🎯 对齐 OpenClaw: name: "nodes"
    std::string name() const override { return "nodes"; }

    // 🎯 对齐 OpenClaw: ownerOnly: true
    bool owner_only() const override { return true; }

    // 🎯 对齐 OpenClaw: description
    std::string description() const override {
        return "Discover and control paired nodes (status/describe/pairing/notify/"
               "camera/photos/screen/location/notifications/run/invoke).";
    }

    // 🎯 对齐 OpenClaw: NodesToolSchema (flattened)
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
            p["description"] = "Action to perform";
            core::JsonValue e = core::JsonValue::array();
            for (const auto& a : NODES_TOOL_ACTIONS) e.push_back(a);
            p["enum"] = e;
            schema.properties["action"] = p;
        }

        schema.properties["gatewayUrl"]             = make_str("Gateway URL override");
        schema.properties["gatewayToken"]           = make_str("Gateway token override");
        schema.properties["timeoutMs"]              = make_num("Request timeout in milliseconds");
        schema.properties["node"]                   = make_str("Node ID or name");
        schema.properties["requestId"]              = make_str("Pairing request ID");
        // notify
        schema.properties["title"]                  = make_str("Notification title");
        schema.properties["body"]                   = make_str("Notification body");
        schema.properties["sound"]                  = make_str("Notification sound");
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            core::JsonValue e = core::JsonValue::array();
            e.push_back("passive"); e.push_back("active"); e.push_back("timeSensitive");
            p["enum"] = e;
            schema.properties["priority"] = p;
        }
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            core::JsonValue e = core::JsonValue::array();
            e.push_back("system"); e.push_back("overlay"); e.push_back("auto");
            p["enum"] = e;
            schema.properties["delivery"] = p;
        }
        // camera
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            p["description"] = "camera_snap: front/back/both; camera_clip: front/back only.";
            core::JsonValue e = core::JsonValue::array();
            e.push_back("front"); e.push_back("back"); e.push_back("both");
            p["enum"] = e;
            schema.properties["facing"] = p;
        }
        schema.properties["maxWidth"]               = make_num("Max image width (pixels)");
        schema.properties["quality"]                = make_num("Image quality (0-1)");
        schema.properties["delayMs"]                = make_num("Delay before capture (ms)");
        schema.properties["deviceId"]               = make_str("Camera device ID");
        schema.properties["limit"]                  = make_num("Max items to return");
        schema.properties["duration"]               = make_str("Duration string (e.g. '10s')");
        schema.properties["durationMs"]             = make_num("Duration in milliseconds (max 300000)");
        schema.properties["includeAudio"]           = make_bool("Include audio in recording");
        // screen_record
        schema.properties["fps"]                    = make_num("Frames per second");
        schema.properties["screenIndex"]            = make_num("Screen index");
        schema.properties["outPath"]                = make_str("Output file path");
        // location_get
        schema.properties["maxAgeMs"]               = make_num("Max location age (ms)");
        schema.properties["locationTimeoutMs"]      = make_num("Location timeout (ms)");
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            core::JsonValue e = core::JsonValue::array();
            e.push_back("coarse"); e.push_back("balanced"); e.push_back("precise");
            p["enum"] = e;
            schema.properties["desiredAccuracy"] = p;
        }
        // notifications_action
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            core::JsonValue e = core::JsonValue::array();
            e.push_back("open"); e.push_back("dismiss"); e.push_back("reply");
            p["enum"] = e;
            schema.properties["notificationAction"] = p;
        }
        schema.properties["notificationKey"]        = make_str("Notification key");
        schema.properties["notificationReplyText"]  = make_str("Reply text for notification");
        // run
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "array";
            p["description"] = "Command argv array (e.g. ['echo', 'Hello'])";
            core::JsonValue items = core::JsonValue::object();
            items["type"] = "string";
            p["items"] = items;
            schema.properties["command"] = p;
        }
        schema.properties["cwd"]                    = make_str("Working directory");
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "array";
            p["description"] = "Environment variables (KEY=VALUE pairs)";
            core::JsonValue items = core::JsonValue::object();
            items["type"] = "string";
            p["items"] = items;
            schema.properties["env"] = p;
        }
        schema.properties["commandTimeoutMs"]       = make_num("Command timeout (ms)");
        schema.properties["invokeTimeoutMs"]        = make_num("Invoke timeout (ms)");
        schema.properties["needsScreenRecording"]   = make_bool("Needs screen recording");
        // invoke
        schema.properties["invokeCommand"]          = make_str("Node command to invoke");
        schema.properties["invokeParamsJson"]       = make_str("JSON params for invoke");

        schema.required = {"action"};
        return schema;
    }

    // 🎯 对齐 OpenClaw: execute
    ToolResult execute(const core::JsonValue& params_val) override {
        try {
            json params = params_val.to_json();
            std::string action = read_str(params, "action");
            if (action.empty())
                return make_error("action is required");
            if (!NODES_TOOL_ACTIONS.count(action))
                return make_error("Unknown action: " + action);

            // Gateway opts
            std::string gw_url   = read_str(params, "gatewayUrl");
            std::string gw_token = read_str(params, "gatewayToken");
            int64_t timeout_ms = 60000;
            if (params.contains("timeoutMs") && params["timeoutMs"].is_number()) {
                double v = params["timeoutMs"].get<double>();
                if (std::isfinite(v)) timeout_ms = (int64_t)v;
            }

            // ----------------------------------------------------------------
            // 🎯 case "status"
            // ----------------------------------------------------------------
            if (action == "status") {
                auto result = call_gateway("node.list", gw_url, gw_token, timeout_ms, {});
                return make_json_result(result);
            }

            // ----------------------------------------------------------------
            // 🎯 case "describe"
            // ----------------------------------------------------------------
            if (action == "describe") {
                std::string node = read_str(params, "node", true);
                std::string node_id = resolve_node_id(node, gw_url, gw_token, timeout_ms);
                auto result = call_gateway("node.describe", gw_url, gw_token, timeout_ms,
                                           {{"nodeId", node_id}});
                return make_json_result(result);
            }

            // ----------------------------------------------------------------
            // 🎯 case "pending"
            // ----------------------------------------------------------------
            if (action == "pending") {
                auto result = call_gateway("node.pair.list", gw_url, gw_token, timeout_ms, {});
                return make_json_result(result);
            }

            // ----------------------------------------------------------------
            // 🎯 case "approve"
            // ----------------------------------------------------------------
            if (action == "approve") {
                std::string request_id = read_str(params, "requestId", true);
                auto result = call_gateway("node.pair.approve", gw_url, gw_token, timeout_ms,
                                           {{"requestId", request_id}});
                return make_json_result(result);
            }

            // ----------------------------------------------------------------
            // 🎯 case "reject"
            // ----------------------------------------------------------------
            if (action == "reject") {
                std::string request_id = read_str(params, "requestId", true);
                auto result = call_gateway("node.pair.reject", gw_url, gw_token, timeout_ms,
                                           {{"requestId", request_id}});
                return make_json_result(result);
            }

            // ----------------------------------------------------------------
            // 🎯 case "notify"
            // ----------------------------------------------------------------
            if (action == "notify") {
                std::string node = read_str(params, "node", true);
                std::string title = read_str(params, "title");
                std::string body  = read_str(params, "body");
                if (title.empty() && body.empty())
                    return make_error("title or body required");
                std::string node_id = resolve_node_id(node, gw_url, gw_token, timeout_ms);
                json notify_params = json::object();
                if (!title.empty()) notify_params["title"] = title;
                if (!body.empty())  notify_params["body"]  = body;
                std::string sound = read_str(params, "sound");
                if (!sound.empty()) notify_params["sound"] = sound;
                std::string priority = read_str(params, "priority");
                if (!priority.empty()) notify_params["priority"] = priority;
                std::string delivery = read_str(params, "delivery");
                if (!delivery.empty()) notify_params["delivery"] = delivery;
                call_gateway("node.invoke", gw_url, gw_token, timeout_ms, {
                    {"nodeId",          node_id},
                    {"command",         "system.notify"},
                    {"params",          notify_params},
                    {"idempotencyKey",  generate_uuid()},
                });
                return make_json_result({{"ok", true}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "camera_snap"
            // ----------------------------------------------------------------
            if (action == "camera_snap") {
                std::string node = read_str(params, "node", true);
                std::string node_id = resolve_node_id(node, gw_url, gw_token, timeout_ms);
                std::string facing_raw = read_str(params, "facing");
                if (facing_raw.empty()) facing_raw = "front";
                std::transform(facing_raw.begin(), facing_raw.end(), facing_raw.begin(), ::tolower);
                if (facing_raw != "front" && facing_raw != "back" && facing_raw != "both")
                    return make_error("invalid facing (front|back|both)");

                double max_width = 1600.0;
                if (params.contains("maxWidth") && params["maxWidth"].is_number())
                    max_width = params["maxWidth"].get<double>();
                double quality = 0.95;
                if (params.contains("quality") && params["quality"].is_number())
                    quality = params["quality"].get<double>();

                std::vector<std::string> facings;
                if (facing_raw == "both") { facings = {"front", "back"}; }
                else { facings = {facing_raw}; }

                json content_arr = json::array();
                json details_arr = json::array();

                for (const auto& facing : facings) {
                    json snap_params = {
                        {"facing",   facing},
                        {"maxWidth", max_width},
                        {"quality",  quality},
                        {"format",   "jpg"},
                    };
                    if (params.contains("delayMs") && params["delayMs"].is_number())
                        snap_params["delayMs"] = params["delayMs"].get<double>();
                    std::string device_id = read_str(params, "deviceId");
                    if (!device_id.empty()) snap_params["deviceId"] = device_id;

                    auto raw = call_gateway("node.invoke", gw_url, gw_token, timeout_ms, {
                        {"nodeId",         node_id},
                        {"command",        "camera.snap"},
                        {"params",         snap_params},
                        {"idempotencyKey", generate_uuid()},
                    });

                    // Extract base64 image from payload
                    if (raw.contains("payload") && raw["payload"].contains("base64") &&
                        raw["payload"]["base64"].is_string()) {
                        std::string b64 = raw["payload"]["base64"].get<std::string>();
                        std::string fmt = "jpg";
                        if (raw["payload"].contains("format") && raw["payload"]["format"].is_string())
                            fmt = raw["payload"]["format"].get<std::string>();
                        std::string mime = (fmt == "png") ? "image/png" : "image/jpeg";
                        content_arr.push_back({{"type", "image"}, {"data", b64}, {"mimeType", mime}});
                        json detail = {{"facing", facing}};
                        if (raw["payload"].contains("width"))  detail["width"]  = raw["payload"]["width"];
                        if (raw["payload"].contains("height")) detail["height"] = raw["payload"]["height"];
                        details_arr.push_back(detail);
                    }
                }

                ToolResult result;
                result.success = true;
                for (const auto& item : content_arr) {
                    ToolResultContent c;
                    if (item.contains("type") && item["type"] == "image") {
                        c.type = ToolResultContent::Type::Image;
                        c.data = item["data"].get<std::string>();
                        c.mime_type = item["mimeType"].get<std::string>();
                    } else {
                        c.type = ToolResultContent::Type::Text;
                        c.text = item.dump();
                    }
                    result.content.push_back(c);
                }
                return result;
            }

            // ----------------------------------------------------------------
            // 🎯 case "camera_list" | "notifications_list" | "device_*"
            // ----------------------------------------------------------------
            {
                auto it = NODE_READ_ACTION_COMMANDS.find(action);
                if (it != NODE_READ_ACTION_COMMANDS.end()) {
                    std::string node = read_str(params, "node", true);
                    std::string node_id = resolve_node_id(node, gw_url, gw_token, timeout_ms);
                    auto raw = call_gateway("node.invoke", gw_url, gw_token, timeout_ms, {
                        {"nodeId",         node_id},
                        {"command",        it->second},
                        {"params",         json::object()},
                        {"idempotencyKey", generate_uuid()},
                    });
                    json payload = raw.contains("payload") ? raw["payload"] : json::object();
                    return make_json_result(payload);
                }
            }

            // ----------------------------------------------------------------
            // 🎯 case "notifications_action"
            // ----------------------------------------------------------------
            if (action == "notifications_action") {
                std::string node = read_str(params, "node", true);
                std::string node_id = resolve_node_id(node, gw_url, gw_token, timeout_ms);
                std::string notification_key = read_str(params, "notificationKey", true);
                std::string notification_action = read_str(params, "notificationAction", true);
                std::transform(notification_action.begin(), notification_action.end(),
                               notification_action.begin(), ::tolower);
                if (notification_action != "open" && notification_action != "dismiss" &&
                    notification_action != "reply")
                    return make_error("notificationAction must be open|dismiss|reply");
                std::string reply_text = read_str(params, "notificationReplyText");
                if (notification_action == "reply" && reply_text.empty())
                    return make_error("notificationReplyText required when notificationAction=reply");
                json cmd_params = {
                    {"key",    notification_key},
                    {"action", notification_action},
                };
                if (!reply_text.empty()) cmd_params["replyText"] = reply_text;
                auto raw = call_gateway("node.invoke", gw_url, gw_token, timeout_ms, {
                    {"nodeId",         node_id},
                    {"command",        "notifications.actions"},
                    {"params",         cmd_params},
                    {"idempotencyKey", generate_uuid()},
                });
                json payload = raw.contains("payload") ? raw["payload"] : json::object();
                return make_json_result(payload);
            }

            // ----------------------------------------------------------------
            // 🎯 case "camera_clip"
            // ----------------------------------------------------------------
            if (action == "camera_clip") {
                std::string node = read_str(params, "node", true);
                std::string node_id = resolve_node_id(node, gw_url, gw_token, timeout_ms);
                std::string facing = read_str(params, "facing");
                if (facing.empty()) facing = "front";
                std::transform(facing.begin(), facing.end(), facing.begin(), ::tolower);
                if (facing != "front" && facing != "back")
                    return make_error("invalid facing (front|back)");
                int64_t duration_ms = 3000;
                if (params.contains("durationMs") && params["durationMs"].is_number())
                    duration_ms = (int64_t)params["durationMs"].get<double>();
                bool include_audio = true;
                if (params.contains("includeAudio") && params["includeAudio"].is_boolean())
                    include_audio = params["includeAudio"].get<bool>();
                json clip_params = {
                    {"facing",       facing},
                    {"durationMs",   duration_ms},
                    {"includeAudio", include_audio},
                    {"format",       "mp4"},
                };
                std::string device_id = read_str(params, "deviceId");
                if (!device_id.empty()) clip_params["deviceId"] = device_id;
                auto raw = call_gateway("node.invoke", gw_url, gw_token, timeout_ms, {
                    {"nodeId",         node_id},
                    {"command",        "camera.clip"},
                    {"params",         clip_params},
                    {"idempotencyKey", generate_uuid()},
                });
                json payload = raw.contains("payload") ? raw["payload"] : json::object();
                return make_json_result({{"ok", true}, {"payload", payload}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "photos_latest"
            // ----------------------------------------------------------------
            if (action == "photos_latest") {
                std::string node = read_str(params, "node", true);
                std::string node_id = resolve_node_id(node, gw_url, gw_token, timeout_ms);
                int limit = DEFAULT_PHOTOS_LIMIT;
                if (params.contains("limit") && params["limit"].is_number())
                    limit = std::max(1, std::min(MAX_PHOTOS_LIMIT, (int)params["limit"].get<double>()));
                double max_width = DEFAULT_PHOTOS_MAX_WIDTH;
                if (params.contains("maxWidth") && params["maxWidth"].is_number())
                    max_width = params["maxWidth"].get<double>();
                double quality = DEFAULT_PHOTOS_QUALITY;
                if (params.contains("quality") && params["quality"].is_number())
                    quality = params["quality"].get<double>();
                auto raw = call_gateway("node.invoke", gw_url, gw_token, timeout_ms, {
                    {"nodeId",         node_id},
                    {"command",        "photos.latest"},
                    {"params",         {{"limit", limit}, {"maxWidth", max_width}, {"quality", quality}}},
                    {"idempotencyKey", generate_uuid()},
                });
                json payload = raw.contains("payload") ? raw["payload"] : json::object();
                return make_json_result(payload);
            }

            // ----------------------------------------------------------------
            // 🎯 case "screen_record"
            // ----------------------------------------------------------------
            if (action == "screen_record") {
                std::string node = read_str(params, "node", true);
                std::string node_id = resolve_node_id(node, gw_url, gw_token, timeout_ms);
                int64_t duration_ms = 10000;
                if (params.contains("durationMs") && params["durationMs"].is_number())
                    duration_ms = std::min((int64_t)300000, (int64_t)params["durationMs"].get<double>());
                double fps = 10.0;
                if (params.contains("fps") && params["fps"].is_number())
                    fps = params["fps"].get<double>();
                int screen_index = 0;
                if (params.contains("screenIndex") && params["screenIndex"].is_number())
                    screen_index = (int)params["screenIndex"].get<double>();
                bool include_audio = true;
                if (params.contains("includeAudio") && params["includeAudio"].is_boolean())
                    include_audio = params["includeAudio"].get<bool>();
                auto raw = call_gateway("node.invoke", gw_url, gw_token, timeout_ms, {
                    {"nodeId",         node_id},
                    {"command",        "screen.record"},
                    {"params",         {
                        {"durationMs",   duration_ms},
                        {"screenIndex",  screen_index},
                        {"fps",          fps},
                        {"format",       "mp4"},
                        {"includeAudio", include_audio},
                    }},
                    {"idempotencyKey", generate_uuid()},
                });
                json payload = raw.contains("payload") ? raw["payload"] : json::object();
                return make_json_result({{"ok", true}, {"payload", payload}});
            }

            // ----------------------------------------------------------------
            // 🎯 case "location_get"
            // ----------------------------------------------------------------
            if (action == "location_get") {
                std::string node = read_str(params, "node", true);
                std::string node_id = resolve_node_id(node, gw_url, gw_token, timeout_ms);
                json loc_params = json::object();
                if (params.contains("maxAgeMs") && params["maxAgeMs"].is_number())
                    loc_params["maxAgeMs"] = params["maxAgeMs"].get<double>();
                std::string desired_accuracy = read_str(params, "desiredAccuracy");
                if (!desired_accuracy.empty()) loc_params["desiredAccuracy"] = desired_accuracy;
                if (params.contains("locationTimeoutMs") && params["locationTimeoutMs"].is_number())
                    loc_params["timeoutMs"] = params["locationTimeoutMs"].get<double>();
                auto raw = call_gateway("node.invoke", gw_url, gw_token, timeout_ms, {
                    {"nodeId",         node_id},
                    {"command",        "location.get"},
                    {"params",         loc_params},
                    {"idempotencyKey", generate_uuid()},
                });
                json payload = raw.contains("payload") ? raw["payload"] : json::object();
                return make_json_result(payload);
            }

            // ----------------------------------------------------------------
            // 🎯 case "run"
            // ----------------------------------------------------------------
            if (action == "run") {
                std::string node = read_str(params, "node", true);
                std::string node_id = resolve_node_id(node, gw_url, gw_token, timeout_ms);
                if (!params.contains("command") || !params["command"].is_array())
                    return make_error("command required (argv array, e.g. ['echo', 'Hello'])");
                json command_arr = params["command"];
                if (command_arr.empty())
                    return make_error("command must not be empty");
                std::string cwd = read_str(params, "cwd");
                json run_params = {
                    {"command", command_arr},
                    {"agentId", opts_.agent_session_key},
                };
                if (!cwd.empty()) run_params["cwd"] = cwd;
                if (params.contains("env") && params["env"].is_array())
                    run_params["env"] = params["env"];
                if (params.contains("commandTimeoutMs") && params["commandTimeoutMs"].is_number())
                    run_params["timeoutMs"] = params["commandTimeoutMs"].get<double>();
                auto raw = call_gateway("node.invoke", gw_url, gw_token, timeout_ms, {
                    {"nodeId",         node_id},
                    {"command",        "system.run"},
                    {"params",         run_params},
                    {"idempotencyKey", generate_uuid()},
                });
                json payload = raw.contains("payload") ? raw["payload"] : json::object();
                return make_json_result(payload);
            }

            // ----------------------------------------------------------------
            // 🎯 case "invoke"
            // ----------------------------------------------------------------
            if (action == "invoke") {
                std::string node = read_str(params, "node", true);
                std::string node_id = resolve_node_id(node, gw_url, gw_token, timeout_ms);
                std::string invoke_command = read_str(params, "invokeCommand", true);
                std::string invoke_command_lower = invoke_command;
                std::transform(invoke_command_lower.begin(), invoke_command_lower.end(),
                               invoke_command_lower.begin(), ::tolower);

                // 🎯 对齐 OpenClaw: block media invoke commands
                auto media_it = MEDIA_INVOKE_ACTIONS.find(invoke_command_lower);
                if (media_it != MEDIA_INVOKE_ACTIONS.end()) {
                    return make_error(
                        "invokeCommand \"" + invoke_command + "\" returns media payloads and is "
                        "blocked to prevent base64 context bloat; use action=\"" +
                        media_it->second + "\"");
                }

                json invoke_params = json::object();
                std::string invoke_params_json = read_str(params, "invokeParamsJson");
                if (!invoke_params_json.empty()) {
                    try {
                        invoke_params = json::parse(invoke_params_json);
                    } catch (const std::exception& e) {
                        return make_error(std::string("invokeParamsJson must be valid JSON: ") + e.what());
                    }
                }

                int64_t invoke_timeout_ms = timeout_ms;
                if (params.contains("invokeTimeoutMs") && params["invokeTimeoutMs"].is_number())
                    invoke_timeout_ms = (int64_t)params["invokeTimeoutMs"].get<double>();

                auto raw = call_gateway("node.invoke", gw_url, gw_token, invoke_timeout_ms, {
                    {"nodeId",         node_id},
                    {"command",        invoke_command},
                    {"params",         invoke_params},
                    {"idempotencyKey", generate_uuid()},
                });
                return make_json_result(raw);
            }

            return make_error("Unknown action: " + action);

        } catch (const std::exception& e) {
            return make_error(std::string(e.what()));
        }
    }

private:
    NodesToolOptions opts_;

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

    // 🎯 对齐 OpenClaw: resolveNodeId(gatewayOpts, node)
    std::string resolve_node_id(
        const std::string& node,
        const std::string& gw_url,
        const std::string& gw_token,
        int64_t timeout_ms
    ) const {
        if (node.empty()) return "";
        // If node looks like a UUID or starts with "node:", use as-is
        if (node.find("node:") == 0 || node.size() == 36) return node;
        // Otherwise, list nodes and find by name
        auto nodes_result = call_gateway("node.list", gw_url, gw_token, timeout_ms, {});
        if (nodes_result.contains("nodes") && nodes_result["nodes"].is_array()) {
            for (const auto& n : nodes_result["nodes"]) {
                if (n.contains("name") && n["name"].is_string() &&
                    n["name"].get<std::string>() == node) {
                    if (n.contains("nodeId") && n["nodeId"].is_string())
                        return n["nodeId"].get<std::string>();
                }
                if (n.contains("nodeId") && n["nodeId"].is_string() &&
                    n["nodeId"].get<std::string>() == node) {
                    return node;
                }
            }
        }
        return node; // fallback: use as-is
    }

    // 🎯 对齐 OpenClaw: callGatewayTool(method, gatewayOpts, params)
    json call_gateway(
        const std::string& method,
        const std::string& gw_url,
        const std::string& gw_token,
        int64_t timeout_ms,
        const json& call_params
    ) const {
        // 🎯 对齐 OpenClaw: callGatewayTool — HTTP RPC 调用
        json rpc = {{"method", method}};
        if (!call_params.empty()) rpc["params"] = call_params;

        std::string url = gw_url.empty() ? "http://127.0.0.1:3000" : gw_url;
        url += "/v1/rpc";

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
            } catch (...) {
                return {{"ok", result.get().ok}, {"body", result.get().body}};
            }
        }
        return {{"ok", false}, {"error", result.error}, {"method", method}};
    }

    // Generate UUID
    static std::string generate_uuid() {
        // Simple UUID v4 generation
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
        char buf[37];
        uint32_t a = dis(gen), b = dis(gen), c = dis(gen), d = dis(gen);
        snprintf(buf, sizeof(buf),
                 "%08x-%04x-%04x-%04x-%012llx",
                 a, (b >> 16) & 0xFFFF,
                 ((b & 0xFFFF) & 0x0FFF) | 0x4000,
                 ((c >> 16) & 0x3FFF) | 0x8000,
                 (uint64_t)(c & 0xFFFF) << 32 | d);
        return buf;
    }
};

} // namespace

// ============================================================================
// Factory
// ============================================================================

std::shared_ptr<Tool> create_nodes_tool(const NodesToolOptions& opts) {
    return std::make_shared<NodesTool>(opts);
}

} // namespace tools
} // namespace hunterclaw
