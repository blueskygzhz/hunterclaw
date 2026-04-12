#pragma once

// 严格对齐 OpenClaw: tasks/task-registry.store.sqlite.ts (541行)
// + tasks/task-registry.paths.ts (23行)
// SQLite persistence backend for TaskRegistry

#include "hunterclaw/tasks/task_registry.h"
#include <memory>
#include <string>

namespace hunterclaw {
namespace tasks {

// ============================================================================
// Paths (对齐 OC: task-registry.paths.ts)
// ============================================================================

/// Resolve task state directory (对齐 OC: resolveTaskStateDir)
std::string resolve_task_state_dir(const std::string& state_dir = "");

/// Resolve task registry directory (对齐 OC: resolveTaskRegistryDir)
std::string resolve_task_registry_dir(const std::string& state_dir = "");

/// Resolve task registry SQLite path (对齐 OC: resolveTaskRegistrySqlitePath)
std::string resolve_task_registry_sqlite_path(const std::string& state_dir = "");

// ============================================================================
// SQLite Store Factory (对齐 OC: task-registry.store.sqlite.ts)
// ============================================================================

/// Create a SQLite-backed TaskRegistryStore.
/// The store opens/creates the SQLite database at the given path,
/// sets WAL mode, creates the schema if needed, and returns a
/// TaskRegistryStore that can be passed to TaskRegistry::configure_store().
///
/// Schema:
///   - task_runs (24 columns, 8 indexes) — matches OC exactly
///   - task_delivery_state (3 columns)
///
/// PRAGMA: journal_mode=WAL, synchronous=NORMAL, busy_timeout=5000
std::shared_ptr<TaskRegistryStore> make_sqlite_task_registry_store(const std::string& db_path);

} // namespace tasks
} // namespace hunterclaw
