#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/nodes/node_registry.h"
#include "hunterclaw/tools/approval.h"
#include <string>
#include <memory>

namespace hunterclaw {
namespace tools {


/**
 * Screen Recording Tool
 * 
 * Captures screen recordings on user devices through Nodes routing system.
 * Supports multiple quality levels and time limits.
 * 
 * Actions:
 * - screen_record_start: Start screen recording
 * - screen_record_stop: Stop current recording
 * - screen_record_status: Check recording status
 * 
 * OpenClaw Reference:
 * - src/agents/tools/screen-record-tool.ts
 * - Supports iOS, Android, macOS nodes
 * - Requires user approval (privacy)
 */
class ScreenRecordTool : public Tool {
public:
    ScreenRecordTool(
        std::shared_ptr<nodes::NodeRegistry> node_registry,
        std::shared_ptr<ApprovalManager> approval_manager
    );

    std::string name() const override { return "screen_record"; }
    std::string description() const override;
    Schema parameters() const override;
    ToolResult execute(const core::JsonValue& params) override;

private:
    std::shared_ptr<nodes::NodeRegistry> node_registry_;
    std::shared_ptr<ApprovalManager> approval_manager_;

    // Action handlers
    core::JsonValue start_recording(const core::JsonValue& params);
    core::JsonValue stop_recording(const core::JsonValue& params);
    core::JsonValue get_status(const core::JsonValue& params);

    // Helper methods
    core::JsonValue route_to_node(const std::string& action, const core::JsonValue& params);
    bool validate_duration(int duration);
    std::string get_quality_description(const std::string& quality);
};

} // namespace tools
} // namespace hunterclaw
