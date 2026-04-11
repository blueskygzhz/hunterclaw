#pragma once

// 对齐 OpenClaw: commands/tasks.ts + commands/flows.ts
// CLI tasks list/show/cancel/audit + flows list/show

#include <memory>
#include <optional>
#include <string>

namespace hunterclaw {
namespace tasks {
class TaskRegistry;
class TaskExecutor;
class TaskFlowRegistry;
}
namespace cli {

/// tasks list (对齐 OC: tasksListCommand)
void tasks_list_command(
    std::shared_ptr<tasks::TaskRegistry> registry,
    bool json_output = false,
    const std::optional<std::string>& runtime_filter = std::nullopt,
    const std::optional<std::string>& status_filter = std::nullopt
);

/// tasks show (对齐 OC: tasksShowCommand)
void tasks_show_command(
    std::shared_ptr<tasks::TaskRegistry> registry,
    const std::string& lookup,
    bool json_output = false
);

/// tasks cancel (对齐 OC: tasksCancelCommand)
void tasks_cancel_command(
    std::shared_ptr<tasks::TaskRegistry> registry,
    const std::string& lookup
);

/// tasks audit (对齐 OC: tasksAuditCommand)
void tasks_audit_command(
    std::shared_ptr<tasks::TaskRegistry> registry,
    std::shared_ptr<tasks::TaskFlowRegistry> flow_registry,
    bool json_output = false
);

/// flows list (对齐 OC: flowsListCommand)
void flows_list_command(
    std::shared_ptr<tasks::TaskFlowRegistry> flow_registry,
    std::shared_ptr<tasks::TaskRegistry> task_registry = nullptr,
    bool json_output = false,
    const std::optional<std::string>& status_filter = std::nullopt
);

} // namespace cli
} // namespace hunterclaw
