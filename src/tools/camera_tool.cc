// camera_tool.cc
// Aligned with OpenClaw: nodes-tool.ts camera actions
// Actions: camera_snap | camera_clip | camera_list | photos_latest

#include "hunterclaw/tools/camera_tool.h"
#include "hunterclaw/core/json.h"
#include <nlohmann/json.hpp>
#include <string>
#include <set>
#include <algorithm>
#include <cmath>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

static const std::set<std::string> CAMERA_ACTIONS = {
    "camera_snap", "camera_clip", "camera_list", "photos_latest"
};

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

CameraTool::CameraTool(
    const ToolContext& context,
    std::shared_ptr<nodes::NodeRegistry> node_registry
) : context_(context), node_registry_(std::move(node_registry)) {}

std::string CameraTool::description() const {
    return "Camera tool: take photos (camera_snap), record video clips (camera_clip), "
           "list cameras (camera_list), get latest photos (photos_latest). "
           "Requires a paired iOS/Android node.";
}

Schema CameraTool::parameters() const {
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

    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Camera action";
        core::JsonValue e = core::JsonValue::array();
        for (const auto& a : CAMERA_ACTIONS) e.push_back(a);
        p["enum"] = e;
        schema.properties["action"] = p;
    }
    schema.properties["node"]         = make_str("Node ID or name");
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Camera facing: front, back, both";
        core::JsonValue e = core::JsonValue::array();
        e.push_back("front"); e.push_back("back"); e.push_back("both");
        p["enum"] = e;
        schema.properties["facing"] = p;
    }
    schema.properties["maxWidth"]     = make_num("Max image width (pixels)");
    schema.properties["quality"]      = make_num("Image quality (0-1)");
    schema.properties["delayMs"]      = make_num("Delay before capture (ms)");
    schema.properties["deviceId"]     = make_str("Camera device ID");
    schema.properties["durationMs"]   = make_num("Video duration (ms, max 300000)");
    schema.properties["includeAudio"] = make_bool("Include audio in video");
    schema.properties["limit"]        = make_num("Max photos to return");

    schema.required = {"action"};
    return schema;
}

ToolResult CameraTool::execute(const core::JsonValue& params_val) {
    try {
        ToolParams params(params_val);
        std::string action = params.read_string("action", true);
        if (!CAMERA_ACTIONS.count(action))
            return make_error("Unknown action: " + action);

        if (action == "camera_snap")    return action_camera_snap(params);
        if (action == "camera_clip")    return action_camera_clip(params);
        if (action == "camera_list")    return action_camera_list(params);
        if (action == "photos_latest")  return action_photos_latest(params);
        return make_error("Unknown action: " + action);
    } catch (const std::exception& e) {
        return make_error(std::string(e.what()));
    }
}

ToolResult CameraTool::action_camera_snap(const ToolParams& params) {
    std::string node_id = select_camera_node();
    json cmd_params = {
        {"facing",   params.read_string("facing", false, "front")},
        {"maxWidth", params.read_number("maxWidth", false, 1600.0)},
        {"quality",  params.read_number("quality",  false, 0.95)},
        {"format",   "jpg"},
    };
    if (params.has("delayMs"))  cmd_params["delayMs"]  = params.read_number("delayMs");
    if (params.has("deviceId")) cmd_params["deviceId"] = params.read_string("deviceId");

    auto result = invoke_node_command(node_id, "camera.snap", core::JsonValue(cmd_params));
    if (!result.ok) return make_error(result.error);

    const auto& payload = result.get();
    json payload_json = payload.to_json();
    if (payload_json.contains("base64") && payload_json["base64"].is_string()) {
        ToolResult tr;
        tr.success = true;
        ToolResultContent c;
        c.type = ToolResultContent::Type::Image;
        c.data = payload_json["base64"].get<std::string>();
        c.mime_type = "image/jpeg";
        tr.content.push_back(c);
        return tr;
    }
    return make_json_result(payload_json);
}

ToolResult CameraTool::action_camera_clip(const ToolParams& params) {
    std::string node_id = select_camera_node();
    std::string facing = params.read_string("facing", false, "front");
    if (facing == "both") facing = "front";
    int64_t duration_ms = std::min((int64_t)300000,
        (int64_t)params.read_number("durationMs", false, 3000.0));
    json cmd_params = {
        {"facing",       facing},
        {"durationMs",   duration_ms},
        {"includeAudio", params.read_bool("includeAudio", false, true)},
        {"format",       "mp4"},
    };
    auto result = invoke_node_command(node_id, "camera.clip", core::JsonValue(cmd_params));
    if (!result.ok) return make_error(result.error);
    return make_json_result(result.get().to_json());
}

ToolResult CameraTool::action_camera_list(const ToolParams& /*params*/) {
    std::string node_id = select_camera_node();
    auto result = invoke_node_command(node_id, "camera.list", core::JsonValue::object());
    if (!result.ok) return make_error(result.error);
    return make_json_result(result.get().to_json());
}

ToolResult CameraTool::action_photos_latest(const ToolParams& params) {
    std::string node_id = select_camera_node();
    int limit = std::max(1, std::min(20, (int)params.read_number("limit", false, 1.0)));
    json cmd_params = {
        {"limit",    limit},
        {"maxWidth", params.read_number("maxWidth", false, 1600.0)},
        {"quality",  params.read_number("quality",  false, 0.85)},
    };
    auto result = invoke_node_command(node_id, "photos.latest", core::JsonValue(cmd_params));
    if (!result.ok) return make_error(result.error);
    return make_json_result(result.get().to_json());
}

std::string CameraTool::select_camera_node() {
    if (node_registry_) {
        auto nodes = node_registry_->list_nodes();
        for (const auto& n : nodes) {
            if (n.has_capability("camera")) return n.id;
        }
    }
    return "";
}

core::Result<core::JsonValue> CameraTool::invoke_node_command(
    const std::string& node_id,
    const std::string& command,
    const core::JsonValue& command_params
) {
    if (node_registry_) {
        return node_registry_->invoke(node_id, command, command_params);
    }
    return core::Result<core::JsonValue>::Err("No node registry available");
}

ToolResult CameraTool::make_error(const std::string& msg) const {
    ToolResult r; r.success = false; r.error = msg;
    ToolResultContent c; c.type = ToolResultContent::Type::Text; c.text = "Error: " + msg;
    r.content.push_back(c); return r;
}

ToolResult CameraTool::make_json_result(const json& data) const {
    ToolResult r; r.success = true;
    ToolResultContent c; c.type = ToolResultContent::Type::Json; c.text = data.dump();
    r.content.push_back(c); return r;
}

} // namespace tools
} // namespace hunterclaw
