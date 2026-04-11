#pragma once

// 严格对齐 OpenClaw pi-embedded-runner/context-engine-maintenance.ts (84行)
// Context engine transcript maintenance + rewrite helpers.

#include "hunterclaw/context/context_engine.h"
#include <functional>
#include <optional>
#include <string>

namespace hunterclaw {
namespace context {

// ============================================================================
// Context Engine Maintenance (对齐 context-engine-maintenance.ts:16-40)
// ============================================================================

/// Build a runtime context with transcript rewrite capability.
/// 对齐 buildContextEngineMaintenanceRuntimeContext
ContextEngineRuntimeContext build_context_engine_maintenance_runtime_context(
    const std::string& session_id,
    const std::string& session_file,
    const std::optional<std::string>& session_key = std::nullopt,
    const ContextEngineRuntimeContext& base_context = {}
);

// ============================================================================
// Run Maintenance (对齐 context-engine-maintenance.ts:45-83)
// ============================================================================

/// Reason for running maintenance.
enum class MaintenanceReason {
  BOOTSTRAP,
  COMPACTION,
  TURN
};

/// Run optional context-engine transcript maintenance.
/// Returns std::nullopt if engine doesn't support maintain().
/// 对齐 runContextEngineMaintenance
std::optional<ContextEngineMaintenanceResult> run_context_engine_maintenance(
    ContextEngine* engine,
    const std::string& session_id,
    const std::string& session_file,
    MaintenanceReason reason,
    const std::optional<std::string>& session_key = std::nullopt,
    const ContextEngineRuntimeContext& runtime_context = {}
);

/// Convert MaintenanceReason to string.
std::string maintenance_reason_to_string(MaintenanceReason reason);

}  // namespace context
}  // namespace hunterclaw
