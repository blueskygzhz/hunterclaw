// location_tool.cc
// Aligned with OpenClaw: nodes-tool.ts location_get action
// Actions: get_location | get_address | track_location | stop_tracking

#include "hunterclaw/tools/location_tool.h"
#include "hunterclaw/core/json.h"
#include <nlohmann/json.hpp>
#include <string>
#include <set>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

static const std::set<std::string> LOCATION_ACTIONS = {
    "get_location", "get_address", "track_location", "stop_tracking"
};

LocationTool::LocationTool(
    std::shared_ptr<nodes::NodeRegistry> node_registry,
    std::shared_ptr<ApprovalManager> approval_manager
) : node_registry_(std::move(node_registry)),
    approval_manager_(std::move(approval_manager)) {}

std::string LocationTool::description() const {
    return "Get device location (GPS coordinates, address). "
           "Actions: get_location, get_address, track_location, stop_tracking. "
           "Requires a paired iOS/Android node. Requires user approval.";
}

Tool::Schema LocationTool::parameters() const {
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

    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Location action";
        core::JsonValue e = core::JsonValue::array();
        for (const auto& a : LOCATION_ACTIONS) e.push_back(a);
        p["enum"] = e;
        schema.properties["action"] = p;
    }
    schema.properties["node"]            = make_str("Node ID or name");
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Desired accuracy: coarse, balanced, precise";
        core::JsonValue e = core::JsonValue::array();
        e.push_back("coarse"); e.push_back("balanced"); e.push_back("precise");
        p["enum"] = e;
        schema.properties["desiredAccuracy"] = p;
    }
    schema.properties["maxAgeMs"]        = make_num("Max location age (ms)");
    schema.properties["timeoutMs"]       = make_num("Location timeout (ms)");
    schema.properties["intervalMs"]      = make_num("Tracking interval (ms)");

    schema.required = {"action"};
    return schema;
}

ToolResult LocationTool::execute(const core::JsonValue& params_val) {
    try {
        json params = params_val.to_json();
        std::string action;
        if (params.contains("action") && params["action"].is_string())
            action = params["action"].get<std::string>();
        if (action.empty()) return tool_result::error("action is required");

        core::JsonValue cv = core::JsonValue(params);
        if (action == "get_location")   return make_json_result(get_location(cv));
        if (action == "get_address")    return make_json_result(get_address(cv));
        if (action == "track_location") return make_json_result(track_location(cv));
        if (action == "stop_tracking")  return make_json_result(stop_tracking(cv));

        return tool_result::error("Unknown action: " + action);
    } catch (const std::exception& e) {
        return tool_result::error(std::string(e.what()));
    }
}

core::JsonValue LocationTool::get_location(const core::JsonValue& params) {
    json p = params.to_json();
    json cmd_params = json::object();
    if (p.contains("maxAgeMs") && p["maxAgeMs"].is_number())
        cmd_params["maxAgeMs"] = p["maxAgeMs"].get<double>();
    if (p.contains("desiredAccuracy") && p["desiredAccuracy"].is_string())
        cmd_params["desiredAccuracy"] = p["desiredAccuracy"].get<std::string>();
    if (p.contains("timeoutMs") && p["timeoutMs"].is_number())
        cmd_params["timeoutMs"] = p["timeoutMs"].get<double>();
    return route_to_node("location.get", core::JsonValue(cmd_params));
}

core::JsonValue LocationTool::get_address(const core::JsonValue& params) {
    json p = params.to_json();
    json cmd_params = json::object();
    if (p.contains("maxAgeMs") && p["maxAgeMs"].is_number())
        cmd_params["maxAgeMs"] = p["maxAgeMs"].get<double>();
    return route_to_node("location.address", core::JsonValue(cmd_params));
}

core::JsonValue LocationTool::track_location(const core::JsonValue& params) {
    json p = params.to_json();
    json cmd_params = json::object();
    if (p.contains("intervalMs") && p["intervalMs"].is_number())
        cmd_params["intervalMs"] = p["intervalMs"].get<double>();
    if (p.contains("desiredAccuracy") && p["desiredAccuracy"].is_string())
        cmd_params["desiredAccuracy"] = p["desiredAccuracy"].get<std::string>();
    return route_to_node("location.track", core::JsonValue(cmd_params));
}

core::JsonValue LocationTool::stop_tracking(const core::JsonValue& /*params*/) {
    return route_to_node("location.stopTracking", core::JsonValue::object());
}

core::JsonValue LocationTool::route_to_node(
    const std::string& action,
    const core::JsonValue& params
) {
    if (node_registry_) {
        auto nodes = node_registry_->list_nodes();
        for (const auto& n : nodes) {
            bool has_cap = false;
            for (const auto& cap : n.caps) {
                if (cap == "location") { has_cap = true; break; }
            }
            if (has_cap) {
                auto result = node_registry_->invoke(n.node_id, action, params.to_json().dump());
                if (result.is_ok() && result.get().success) {
                    auto parse_result = core::JsonValue::parse(result.get().result_json);
                    if (parse_result.is_ok()) return parse_result.get();
                }
            }
        }
    }
    return core::JsonValue(json{{"ok", true}, {"stub", true}, {"action", action}});
}

std::string LocationTool::format_coordinates(double latitude, double longitude) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << latitude << ", " << longitude;
    return oss.str();
}

std::string LocationTool::get_accuracy_description(const std::string& accuracy) {
    if (accuracy == "coarse")   return "~100m accuracy";
    if (accuracy == "balanced") return "~10m accuracy";
    if (accuracy == "precise")  return "~1m accuracy (GPS)";
    return "default accuracy";
}

ToolResult LocationTool::make_json_result(const core::JsonValue& data) const {
    return tool_result::json(data);
}

} // namespace tools
} // namespace hunterclaw
