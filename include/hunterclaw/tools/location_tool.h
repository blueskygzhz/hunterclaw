#pragma once

#include "tool.h"
#include "../core/json_value.h"
#include "../nodes/node_registry.h"
#include "../approval/approval_manager.h"
#include <string>
#include <memory>

namespace hunterclaw {

/**
 * Location Tool
 * 
 * Gets the user's device location through Nodes routing system.
 * Supports GPS coordinates, address lookup, and location tracking.
 * 
 * Actions:
 * - get_location: Get current GPS coordinates
 * - get_address: Get current address (reverse geocoding)
 * - track_location: Start location tracking
 * - stop_tracking: Stop location tracking
 * 
 * OpenClaw Reference:
 * - src/agents/tools/location-tool.ts
 * - Supports iOS, Android nodes
 * - Requires user approval (privacy)
 */
class LocationTool : public Tool {
public:
    LocationTool(
        std::shared_ptr<NodeRegistry> node_registry,
        std::shared_ptr<ApprovalManager> approval_manager
    );

    std::string name() const override { return "location"; }
    std::string description() const override;
    JsonValue schema() const override;
    JsonValue execute(const JsonValue& params) override;

private:
    std::shared_ptr<NodeRegistry> node_registry_;
    std::shared_ptr<ApprovalManager> approval_manager_;

    // Action handlers
    JsonValue get_location(const JsonValue& params);
    JsonValue get_address(const JsonValue& params);
    JsonValue track_location(const JsonValue& params);
    JsonValue stop_tracking(const JsonValue& params);

    // Helper methods
    JsonValue route_to_node(const std::string& action, const JsonValue& params);
    std::string format_coordinates(double latitude, double longitude);
    std::string get_accuracy_description(const std::string& accuracy);
};

} // namespace hunterclaw
