#pragma once

// canvas_tool.h
// Strictly aligned with OpenClaw: src/agents/tools/canvas-tool.ts
//
// Tool: canvas
// Actions: present | hide | navigate | eval | snapshot | a2ui_push | a2ui_reset

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/types.h"
#include <memory>
#include <string>

namespace hunterclaw {
namespace tools {

/**
 * @brief Options for canvas tool (aligned with OpenClaw createCanvasTool options)
 */
struct CanvasToolOptions {
    // Config reference (for image sanitization limits)
    const core::Config* config = nullptr;
};

/**
 * @brief Create canvas tool
 *
 * Aligned with OpenClaw: createCanvasTool(options?)
 *
 * Provides: present, hide, navigate, eval, snapshot, a2ui_push, a2ui_reset
 */
std::shared_ptr<Tool> create_canvas_tool(const CanvasToolOptions& opts = {});

} // namespace tools
} // namespace hunterclaw
