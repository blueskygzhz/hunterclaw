#pragma once

// nodes_tool.h
// Strictly aligned with OpenClaw: src/agents/tools/nodes-tool.ts
//
// Tool: nodes
// Actions: status | describe | pending | approve | reject | notify |
//          camera_snap | camera_list | camera_clip | photos_latest |
//          screen_record | location_get | notifications_list |
//          notifications_action | device_status | device_info |
//          device_permissions | device_health | run | invoke
// ownerOnly: true

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/types.h"
#include <memory>
#include <string>

namespace hunterclaw {
namespace tools {

/**
 * @brief Options for nodes tool (aligned with OpenClaw createNodesTool options)
 */
struct NodesToolOptions {
    // Current agent session key
    std::string agent_session_key;

    // Config reference (for image sanitization limits)
    const core::Config* config = nullptr;

    // Whether the model has vision capability
    bool model_has_vision = false;

    // Whether to allow media invoke commands (camera.snap, etc.)
    bool allow_media_invoke_commands = false;
};

/**
 * @brief Create nodes tool
 *
 * Aligned with OpenClaw: createNodesTool(options?)
 *
 * Provides: status, describe, pending, approve, reject, notify,
 *           camera_snap, camera_list, camera_clip, photos_latest,
 *           screen_record, location_get, notifications_list,
 *           notifications_action, device_status, device_info,
 *           device_permissions, device_health, run, invoke
 * ownerOnly: true
 */
std::shared_ptr<Tool> create_nodes_tool(const NodesToolOptions& opts = {});

} // namespace tools
} // namespace hunterclaw
