#pragma once

// 严格对齐 OpenClaw: tasks/task-flow-registry.store.sqlite.ts (405行)
// + tasks/task-flow-registry.paths.ts (11行)
// SQLite persistence backend for TaskFlowRegistry

#include "hunterclaw/tasks/task_flow_registry.h"
#include <memory>
#include <string>

namespace hunterclaw {
namespace tasks {

/// Resolve flow registry directory (对齐 OC: resolveTaskFlowRegistryDir)
std::string resolve_task_flow_registry_dir(const std::string& state_dir = "");

/// Resolve flow registry SQLite path (对齐 OC: resolveTaskFlowRegistrySqlitePath)
std::string resolve_task_flow_registry_sqlite_path(const std::string& state_dir = "");

/// Create a SQLite-backed TaskFlowRegistryStore.
std::shared_ptr<TaskFlowRegistryStore> make_sqlite_task_flow_registry_store(const std::string& db_path);

} // namespace tasks
} // namespace hunterclaw
