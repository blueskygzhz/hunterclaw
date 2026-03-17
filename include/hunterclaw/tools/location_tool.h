#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/nodes/node_registry.h"
#include "hunterclaw/tools/approval.h"
#include <string>
#include <memory>

namespace hunterclaw {
namespace tools {

// Forward declarations

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
        std::shared_ptr<nodes::NodeRegistry> node_registry,
        std::shared_ptr<ApprovalManager> approval_manager
    );

    std::string name() const override { return "location"; }
    std::string description() const override;
    Schema parameters() const override;
    ToolResult execute(const core::JsonValue& params) override;

private:
    std::shared_ptr<nodes::NodeRegistry> node_registry_;
    std::shared_ptr<ApprovalManager> approval_manager_;

    // Action handlers
    core::JsonValue get_location(const core::JsonValue& params);
    core::JsonValue get_address(const core::JsonValue& params);
    core::JsonValue track_location(const core::JsonValue& params);
    core::JsonValue stop_tracking(const core::JsonValue& params);

    // Helper methods
    core::JsonValue route_to_node(const std::string& action, const core::JsonValue& params);
    std::string format_coordinates(double latitude, double longitude);
    std::string get_accuracy_description(const std::string& accuracy);
};

} // namespace tools
} // namespace hunterclaw
