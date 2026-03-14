#include "hunterclaw/tools/location_tool.h"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace hunterclaw {

LocationTool::LocationTool(
    std::shared_ptr<NodeRegistry> node_registry,
    std::shared_ptr<ApprovalManager> approval_manager
) : node_registry_(node_registry),
    approval_manager_(approval_manager) {}

std::string LocationTool::description() const {
    return "Get the user's location from their device (GPS coordinates, address). "
           "Supports iOS and Android. Requires user approval for privacy.";
}

JsonValue LocationTool::schema() const {
    JsonValue schema = JsonValue::object();
    schema["type"] = "object";
    
    JsonValue properties = JsonValue::object();
    
    // action
    JsonValue action_prop = JsonValue::object();
    action_prop["type"] = "string";
    action_prop["description"] = "Action to perform";
    JsonValue action_enum = JsonValue::array();
    action_enum.push_back("get_location");
    action_enum.push_back("get_address");
    action_enum.push_back("track_location");
    action_enum.push_back("stop_tracking");
    action_prop["enum"] = action_enum;
    properties["action"] = action_prop;
    
    // accuracy
    JsonValue accuracy_prop = JsonValue::object();
    accuracy_prop["type"] = "string";
    accuracy_prop["description"] = "Location accuracy level";
    JsonValue accuracy_enum = JsonValue::array();
    accuracy_enum.push_back("low");         // ~1km, battery-friendly
    accuracy_enum.push_back("balanced");    // ~100m, good balance
    accuracy_enum.push_back("high");        // ~10m, GPS required
    accuracy_enum.push_back("best");        // ~1m, highest accuracy
    accuracy_prop["enum"] = accuracy_enum;
    accuracy_prop["default"] = "balanced";
    properties["accuracy"] = accuracy_prop;
    
    // timeout
    JsonValue timeout_prop = JsonValue::object();
    timeout_prop["type"] = "integer";
    timeout_prop["description"] = "Location request timeout in seconds";
    timeout_prop["default"] = 10;
    timeout_prop["minimum"] = 1;
    timeout_prop["maximum"] = 60;
    properties["timeout"] = timeout_prop;
    
    // interval (for tracking)
    JsonValue interval_prop = JsonValue::object();
    interval_prop["type"] = "integer";
    interval_prop["description"] = "Update interval in seconds for tracking";
    interval_prop["default"] = 5;
    interval_prop["minimum"] = 1;
    interval_prop["maximum"] = 300;
    properties["interval"] = interval_prop;
    
    // node_id (optional)
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

JsonValue LocationTool::execute(const JsonValue& params) {
    // Extract action
    if (!params.has("action")) {
        JsonValue error = JsonValue::object();
        error["error"] = "Missing required parameter: action";
        return error;
    }
    
    std::string action = params["action"].as_string();
    
    // Route to appropriate handler
    if (action == "get_location") {
        return get_location(params);
    } else if (action == "get_address") {
        return get_address(params);
    } else if (action == "track_location") {
        return track_location(params);
    } else if (action == "stop_tracking") {
        return stop_tracking(params);
    } else {
        JsonValue error = JsonValue::object();
        error["error"] = "Unknown action: " + action;
        return error;
    }
}

JsonValue LocationTool::get_location(const JsonValue& params) {
    std::string accuracy = params.has("accuracy") ? 
        params["accuracy"].as_string() : "balanced";
    
    // Request approval (location is privacy-sensitive)
    std::stringstream approval_msg;
    approval_msg << "📍 Location Access Request\n\n"
                 << "Accuracy: " << get_accuracy_description(accuracy) << "\n\n"
                 << "The agent wants to access your device's location.\n\n"
                 << "⚠️ Privacy Notice: Your GPS coordinates will be shared.";
    
    if (approval_manager_) {
        bool approved = approval_manager_->request_approval(
            "location",
            approval_msg.str()
        );
        
        if (!approved) {
            JsonValue result = JsonValue::object();
            result["status"] = "denied";
            result["message"] = "User denied location permission";
            return result;
        }
    }
    
    return route_to_node("get_location", params);
}

JsonValue LocationTool::get_address(const JsonValue& params) {
    // get_address also needs location permission
    if (approval_manager_) {
        bool approved = approval_manager_->request_approval(
            "location",
            "📍 Address Lookup Request\n\nThe agent wants to get your current address."
        );
        
        if (!approved) {
            JsonValue result = JsonValue::object();
            result["status"] = "denied";
            result["message"] = "User denied location permission";
            return result;
        }
    }
    
    return route_to_node("get_address", params);
}

JsonValue LocationTool::track_location(const JsonValue& params) {
    int interval = params.has("interval") ? params["interval"].as_int() : 5;
    
    std::stringstream approval_msg;
    approval_msg << "📍 Location Tracking Request\n\n"
                 << "Update Interval: Every " << interval << " seconds\n\n"
                 << "The agent wants to continuously track your location.\n\n"
                 << "⚠️ Privacy Notice: Your location will be monitored.";
    
    if (approval_manager_) {
        bool approved = approval_manager_->request_approval(
            "location_tracking",
            approval_msg.str()
        );
        
        if (!approved) {
            JsonValue result = JsonValue::object();
            result["status"] = "denied";
            result["message"] = "User denied location tracking permission";
            return result;
        }
    }
    
    return route_to_node("track_location", params);
}

JsonValue LocationTool::stop_tracking(const JsonValue& params) {
    return route_to_node("stop_tracking", params);
}

JsonValue LocationTool::route_to_node(
    const std::string& action, 
    const JsonValue& params
) {
    if (!node_registry_) {
        JsonValue error = JsonValue::object();
        error["error"] = "Node registry not available";
        return error;
    }
    
    // Select node (prefer explicit node_id, else auto-select mobile device)
    std::string node_id;
    if (params.has("node_id")) {
        node_id = params["node_id"].as_string();
    } else {
        // Auto-select: prefer iOS > Android
        auto nodes = node_registry_->list_nodes();
        for (const auto& node : nodes) {
            if (node.platform == "ios" && node.status == "connected") {
                node_id = node.id;
                break;
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
        error["error"] = "No available mobile device found";
        error["hint"] = "Please ensure an iOS or Android device is connected";
        return error;
    }
    
    // Send command to node
    JsonValue command = JsonValue::object();
    command["tool"] = "location";
    command["action"] = action;
    
    if (params.has("accuracy")) {
        command["accuracy"] = params["accuracy"];
    }
    if (params.has("timeout")) {
        command["timeout"] = params["timeout"];
    }
    if (params.has("interval")) {
        command["interval"] = params["interval"];
    }
    
    auto response = node_registry_->send_to_node(node_id, command);
    
    // Format response
    JsonValue result = JsonValue::object();
    result["status"] = "success";
    result["action"] = action;
    result["node_id"] = node_id;
    
    // Location data
    if (response.has("latitude") && response.has("longitude")) {
        result["latitude"] = response["latitude"];
        result["longitude"] = response["longitude"];
        
        double lat = response["latitude"].as_double();
        double lon = response["longitude"].as_double();
        result["coordinates"] = format_coordinates(lat, lon);
    }
    
    if (response.has("accuracy")) {
        result["accuracy"] = response["accuracy"];
    }
    if (response.has("altitude")) {
        result["altitude"] = response["altitude"];
    }
    if (response.has("speed")) {
        result["speed"] = response["speed"];
    }
    if (response.has("bearing")) {
        result["bearing"] = response["bearing"];
    }
    if (response.has("timestamp")) {
        result["timestamp"] = response["timestamp"];
    }
    
    // Address data (for get_address)
    if (response.has("address")) {
        result["address"] = response["address"];
    }
    if (response.has("city")) {
        result["city"] = response["city"];
    }
    if (response.has("country")) {
        result["country"] = response["country"];
    }
    if (response.has("postal_code")) {
        result["postal_code"] = response["postal_code"];
    }
    
    // Tracking status
    if (response.has("tracking_id")) {
        result["tracking_id"] = response["tracking_id"];
    }
    if (response.has("tracking_status")) {
        result["tracking_status"] = response["tracking_status"];
    }
    
    return result;
}

std::string LocationTool::format_coordinates(double latitude, double longitude) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(6);
    
    // Latitude
    ss << std::abs(latitude) << "°" << (latitude >= 0 ? "N" : "S");
    ss << ", ";
    
    // Longitude
    ss << std::abs(longitude) << "°" << (longitude >= 0 ? "E" : "W");
    
    return ss.str();
}

std::string LocationTool::get_accuracy_description(const std::string& accuracy) {
    if (accuracy == "low") {
        return "Low (~1km, battery-friendly)";
    } else if (accuracy == "balanced") {
        return "Balanced (~100m, good balance)";
    } else if (accuracy == "high") {
        return "High (~10m, GPS required)";
    } else if (accuracy == "best") {
        return "Best (~1m, highest accuracy)";
    }
    return accuracy;
}

} // namespace hunterclaw
