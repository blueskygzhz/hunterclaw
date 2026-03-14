#include "hunterclaw/tools/screen_record_tool.h"
#include <sstream>

namespace hunterclaw {

ScreenRecordTool::ScreenRecordTool(
    std::shared_ptr<NodeRegistry> node_registry,
    std::shared_ptr<ApprovalManager> approval_manager
) : node_registry_(node_registry),
    approval_manager_(approval_manager) {}

std::string ScreenRecordTool::description() const {
    return "Record the user's screen. Supports iOS, Android, and macOS devices. "
           "Requires user approval for privacy. Max duration: 60 seconds.";
}

JsonValue ScreenRecordTool::schema() const {
    JsonValue schema = JsonValue::object();
    schema["type"] = "object";
    
    JsonValue properties = JsonValue::object();
    
    // action
    JsonValue action_prop = JsonValue::object();
    action_prop["type"] = "string";
    action_prop["description"] = "Action to perform";
    JsonValue action_enum = JsonValue::array();
    action_enum.push_back("screen_record_start");
    action_enum.push_back("screen_record_stop");
    action_enum.push_back("screen_record_status");
    action_prop["enum"] = action_enum;
    properties["action"] = action_prop;
    
    // duration (for start)
    JsonValue duration_prop = JsonValue::object();
    duration_prop["type"] = "integer";
    duration_prop["description"] = "Recording duration in seconds (1-60)";
    duration_prop["default"] = 10;
    duration_prop["minimum"] = 1;
    duration_prop["maximum"] = 60;
    properties["duration"] = duration_prop;
    
    // quality
    JsonValue quality_prop = JsonValue::object();
    quality_prop["type"] = "string";
    quality_prop["description"] = "Recording quality";
    JsonValue quality_enum = JsonValue::array();
    quality_enum.push_back("low");      // 480p, smaller file
    quality_enum.push_back("medium");   // 720p, balanced
    quality_enum.push_back("high");     // 1080p, best quality
    quality_prop["enum"] = quality_enum;
    quality_prop["default"] = "medium";
    properties["quality"] = quality_prop;
    
    // node_id (optional, auto-select if not provided)
    JsonValue node_prop = JsonValue::object();
    node_prop["type"] = "string";
    node_prop["description"] = "Target node ID (auto-select if not provided)";
    properties["node_id"] = node_prop;
    
    schema["properties"] = properties;
    
    JsonValue required = JsonValue::array();
    required.push_back("action");
    schema["required"] = required;
    
    return schema;
}

JsonValue ScreenRecordTool::execute(const JsonValue& params) {
    // Extract action
    if (!params.has("action")) {
        JsonValue error = JsonValue::object();
        error["error"] = "Missing required parameter: action";
        return error;
    }
    
    std::string action = params["action"].as_string();
    
    // Route to appropriate handler
    if (action == "screen_record_start") {
        return start_recording(params);
    } else if (action == "screen_record_stop") {
        return stop_recording(params);
    } else if (action == "screen_record_status") {
        return get_status(params);
    } else {
        JsonValue error = JsonValue::object();
        error["error"] = "Unknown action: " + action;
        return error;
    }
}

JsonValue ScreenRecordTool::start_recording(const JsonValue& params) {
    // Validate duration
    int duration = params.has("duration") ? params["duration"].as_int() : 10;
    if (!validate_duration(duration)) {
        JsonValue error = JsonValue::object();
        error["error"] = "Invalid duration. Must be between 1 and 60 seconds.";
        return error;
    }
    
    std::string quality = params.has("quality") ? 
        params["quality"].as_string() : "medium";
    
    // Request approval (screen recording is privacy-sensitive)
    std::stringstream approval_msg;
    approval_msg << "🎥 Screen Recording Request\n\n"
                 << "Duration: " << duration << " seconds\n"
                 << "Quality: " << get_quality_description(quality) << "\n\n"
                 << "The agent wants to record your screen.\n"
                 << "This will capture everything visible on your display.\n\n"
                 << "⚠️ Privacy Notice: Screen recording may capture sensitive information.";
    
    if (approval_manager_) {
        bool approved = approval_manager_->request_approval(
            "screen_record",
            approval_msg.str()
        );
        
        if (!approved) {
            JsonValue result = JsonValue::object();
            result["status"] = "denied";
            result["message"] = "User denied screen recording permission";
            return result;
        }
    }
    
    // Route to node
    return route_to_node("screen_record_start", params);
}

JsonValue ScreenRecordTool::stop_recording(const JsonValue& params) {
    return route_to_node("screen_record_stop", params);
}

JsonValue ScreenRecordTool::get_status(const JsonValue& params) {
    return route_to_node("screen_record_status", params);
}

JsonValue ScreenRecordTool::route_to_node(
    const std::string& action, 
    const JsonValue& params
) {
    if (!node_registry_) {
        JsonValue error = JsonValue::object();
        error["error"] = "Node registry not available";
        return error;
    }
    
    // Select node (prefer explicit node_id, else auto-select)
    std::string node_id;
    if (params.has("node_id")) {
        node_id = params["node_id"].as_string();
    } else {
        // Auto-select: prefer macOS > iOS > Android
        auto nodes = node_registry_->list_nodes();
        for (const auto& node : nodes) {
            if (node.platform == "macos" && node.status == "connected") {
                node_id = node.id;
                break;
            }
        }
        if (node_id.empty()) {
            for (const auto& node : nodes) {
                if (node.platform == "ios" && node.status == "connected") {
                    node_id = node.id;
                    break;
                }
            }
        }
        if (node_id.empty()) {
            for (const auto& node : nodes) {
                if (node.platform == "android" && node.status == "connected") {
                    node_id = node.id;
                    break;
                }
            }
        }
    }
    
    if (node_id.empty()) {
        JsonValue error = JsonValue::object();
        error["error"] = "No available node found for screen recording";
        error["hint"] = "Please ensure a device (iOS/Android/macOS) is connected";
        return error;
    }
    
    // Send command to node
    JsonValue command = JsonValue::object();
    command["tool"] = "screen_record";
    command["action"] = action;
    
    if (params.has("duration")) {
        command["duration"] = params["duration"];
    }
    if (params.has("quality")) {
        command["quality"] = params["quality"];
    }
    
    auto response = node_registry_->send_to_node(node_id, command);
    
    // Format response
    JsonValue result = JsonValue::object();
    result["status"] = "success";
    result["action"] = action;
    result["node_id"] = node_id;
    
    if (response.has("video_path")) {
        result["video_path"] = response["video_path"];
    }
    if (response.has("video_base64")) {
        result["video_base64"] = response["video_base64"];
    }
    if (response.has("recording_status")) {
        result["recording_status"] = response["recording_status"];
    }
    if (response.has("duration")) {
        result["duration"] = response["duration"];
    }
    if (response.has("file_size")) {
        result["file_size"] = response["file_size"];
    }
    
    return result;
}

bool ScreenRecordTool::validate_duration(int duration) {
    return duration >= 1 && duration <= 60;
}

std::string ScreenRecordTool::get_quality_description(const std::string& quality) {
    if (quality == "low") {
        return "Low (480p, smaller file)";
    } else if (quality == "medium") {
        return "Medium (720p, balanced)";
    } else if (quality == "high") {
        return "High (1080p, best quality)";
    }
    return quality;
}

} // namespace hunterclaw
