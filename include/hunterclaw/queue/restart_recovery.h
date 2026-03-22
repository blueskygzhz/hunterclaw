#ifndef HUNTERCLAW_QUEUE_RESTART_RECOVERY_H
#define HUNTERCLAW_QUEUE_RESTART_RECOVERY_H

#include <functional>

namespace hunterclaw {
namespace queue {

/**
 * Returns an iteration hook for in-process restart loops.
 * The first call is considered initial startup and does nothing.
 * Each subsequent call represents a restart iteration and invokes onRestart.
 * Aligned with OpenClaw: createRestartIterationHook()
 */
inline std::function<bool()> create_restart_iteration_hook(std::function<void()> on_restart) {
    auto is_first = std::make_shared<bool>(true);
    return [is_first, on_restart]() -> bool {
        if (*is_first) {
            *is_first = false;
            return false;
        }
        if (on_restart) on_restart();
        return true;
    };
}

} // namespace queue
} // namespace hunterclaw

#endif // HUNTERCLAW_QUEUE_RESTART_RECOVERY_H
