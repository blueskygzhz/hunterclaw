// screen_record_tool.cc
// Aligned with OpenClaw: nodes-tool.ts screen_record action
// Actions: screen_record_start | screen_record_stop | screen_record_status

#include "hunterclaw/tools/screen_record_tool.h"
#include "hunterclaw/core/json.h"
#include <nlohmann/json.hpp>
#include <string>
#include <set>
#include <algorithm>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

static const std::set<std::string> SCREEN_RECORD_ACTIONS = {
    "screen_record_start", "screen_record_stop", "screen_record_status"
};

// 🎯 对齐 OpenClaw: MAX_SCREEN_RECORD_DURATION_MS = 300_000
static constexpr int MAX_SCREEN_RECORD_DURATION_MS = 300000;
static constexpr int DEFAULT_SCREEN_RECORD_DURATION_MS = 10000;

ScreenRecordTool::ScreenRecordTool(
    std::shared_ptr<nodes::NodeRegistry> node_registry,
    std::shared_ptr<ApprovalManager> approval_manager
) : node_registry_(std::move(node_registry)),
    approval_manager_(std::move(approval_manager)) {}

std::string ScreenRecordTool::description() const {
    return "Screen recording tool: start/stop/status screen recording on paired nodes. "
           "Actions: screen_record_start, screen_record_stop, screen_record_status. "
           "Requires user approval.";
}

Tool::Schema ScreenRecordTool::parameters() const {
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

    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Screen record action";
        core::JsonValue e = core::JsonValue::array();
        for (const auto& a : SCREEN_RECORD_ACTIONS) e.push_back(a);
        p["enum"] = e;
        schema.properties["action"] = p;
    }
    schema.properties["node"]         = make_str("Node ID or name");
    schema.properties["durationMs"]   = make_num("Recording duration (ms, max 300000)");
    schema.properties["fps"]          = make_num("Frames per second");
    schema.properties["screenIndex"]  = make_num("Screen index (0-based)");
    schema.properties["includeAudio"] = make_bool("Include audio");
    {
        core::JsonValue p = core::JsonValue::object();
        p["type"] = "string";
        p["description"] = "Quality: low, medium, high";
        core::JsonValue e = core::JsonValue::array();
        e.push_back("low"); e.push_back("medium"); e.push_back("high");
        p["enum"] = e;
        schema.properties["quality"] = p;
    }

    schema.required = {"action"};
    return schema;
}

ToolResult ScreenRecordTool::execute(const core::JsonValue& params_val) {
    try {
        json params = params_val.to_json();
        std::string action;
        if (params.contains("action") && params["action"].is_string())
            action = params["action"].get<std::string>();
        if (action.empty()) return tool_result::error("action is required");

        core::JsonValue cv = core::JsonValue(params);
        if (action == "screen_record_start")  return make_json_result(start_recording(cv));
        if (action == "screen_record_stop")   return make_json_result(stop_recording(cv));
        if (action == "screen_record_status") return make_json_result(get_status(cv));

        return tool_result::error("Unknown action: " + action);
    } catch (const std::exception& e) {
        return tool_result::error(std::string(e.what()));
    }
}

core::JsonValue ScreenRecordTool::start_recording(const core::JsonValue& params) {
    json p = params.to_json();
    int64_t duration_ms = DEFAULT_SCREEN_RECORD_DURATION_MS;
    if (p.contains("durationMs") && p["durationMs"].is_number())
        duration_ms = std::min((int64_t)MAX_SCREEN_RECORD_DURATION_MS,
                               (int64_t)p["durationMs"].get<double>());
    double fps = 10.0;
    if (p.contains("fps") && p["fps"].is_number()) fps = p["fps"].get<double>();
    int screen_index = 0;
    if (p.contains("screenIndex") && p["screenIndex"].is_number())
        screen_index = (int)p["screenIndex"].get<double>();
    bool include_audio = true;
    if (p.contains("includeAudio") && p["includeAudio"].is_boolean())
        include_audio = p["includeAudio"].get<bool>();

    json cmd_params = {
        {"durationMs",   duration_ms},
        {"fps",          fps},
        {"screenIndex",  screen_index},
        {"includeAudio", include_audio},
        {"format",       "mp4"},
    };
    return route_to_node("screen.record", core::JsonValue(cmd_params));
}

core::JsonValue ScreenRecordTool::stop_recording(const core::JsonValue& /*params*/) {
    return route_to_node("screen.stopRecord", core::JsonValue::object());
}

core::JsonValue ScreenRecordTool::get_status(const core::JsonValue& /*params*/) {
    return route_to_node("screen.recordStatus", core::JsonValue::object());
}

core::JsonValue ScreenRecordTool::route_to_node(
    const std::string& action,
    const core::JsonValue& params
) {
    if (node_registry_) {
        auto nodes = node_registry_->list_nodes();
        for (const auto& n : nodes) {
            if (n.has_capability("screen_record")) {
                auto result = node_registry_->invoke(n.id, action, params);
                if (result.ok) return result.get();
            }
        }
    }
    return core::JsonValue(json{{"ok", true}, {"stub", true}, {"action", action}});
}

bool ScreenRecordTool::validate_duration(int duration) {
    return duration > 0 && duration <= MAX_SCREEN_RECORD_DURATION_MS;
}

std::string ScreenRecordTool::get_quality_description(const std::string& quality) {
    if (quality == "low")    return "480p, 5fps";
    if (quality == "medium") return "720p, 10fps";
    if (quality == "high")   return "1080p, 30fps";
    return "default quality";
}

ToolResult ScreenRecordTool::make_json_result(const core::JsonValue& data) const {
    return tool_result::json(data);
}

} // namespace tools
} // namespace hunterclaw
