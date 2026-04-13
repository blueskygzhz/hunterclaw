// 对齐 OC: src/process/command-queue.ts (409行) + src/process/exec.ts (445行)
// 命令队列 + 进程执行封装
#include "hunterclaw/process/supervisor.h"
#include <nlohmann/json.hpp>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <thread>
#include <map>
#include <set>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <optional>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>

using json = nlohmann::json;

namespace hunterclaw {
namespace process {

// =========================================================================
// 对齐 OC command-queue.ts: Error Types
// =========================================================================

// 对齐 OC: CommandLaneClearedError
class CommandLaneClearedError : public std::runtime_error {
public:
    explicit CommandLaneClearedError(const std::string& lane = "")
        : std::runtime_error(lane.empty() ? "Command lane cleared"
                                           : "Command lane \"" + lane + "\" cleared"),
          lane_(lane) {}
    const std::string& lane() const { return lane_; }
private:
    std::string lane_;
};

// 对齐 OC: GatewayDrainingError
class GatewayDrainingError : public std::runtime_error {
public:
    GatewayDrainingError()
        : std::runtime_error("Gateway is draining for restart; new tasks are not accepted") {}
};

// =========================================================================
// 对齐 OC command-queue.ts: Lane-based Command Queue
// =========================================================================

struct QueueEntry {
    std::function<void()> task;
    std::function<void()> on_done;
    std::function<void(std::exception_ptr)> on_error;
    int64_t enqueued_at = 0;
    int64_t warn_after_ms = 30000;
    std::function<void(int64_t wait_ms, int queued_ahead)> on_wait;
};

struct LaneState {
    std::string lane;
    std::queue<QueueEntry> queue;
    std::set<int> active_task_ids;
    int max_concurrent = 1;
    bool draining = false;
    int generation = 0;
    int next_task_id = 0;
};

class CommandQueue {
public:
    // 对齐 OC: instance()
    static CommandQueue& instance() {
        static CommandQueue queue;
        return queue;
    }

    // 对齐 OC: enqueue
    void enqueue(const std::string& lane_name,
                 std::function<void()> task,
                 int64_t warn_after_ms = 30000) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& lane = get_or_create_lane(lane_name);

        if (lane.draining) {
            throw GatewayDrainingError();
        }

        QueueEntry entry;
        entry.task = std::move(task);
        entry.enqueued_at = now_ms();
        entry.warn_after_ms = warn_after_ms;

        lane.queue.push(std::move(entry));
        maybe_dispatch_lane(lane);
    }

    // 对齐 OC: clearLane
    void clear_lane(const std::string& lane_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lanes_.find(lane_name);
        if (it == lanes_.end()) return;

        auto& lane = it->second;
        lane.generation++;

        // Reject all queued entries
        while (!lane.queue.empty()) {
            auto& entry = lane.queue.front();
            if (entry.on_error) {
                try {
                    throw CommandLaneClearedError(lane_name);
                } catch (...) {
                    entry.on_error(std::current_exception());
                }
            }
            lane.queue.pop();
        }
    }

    // 对齐 OC: startDraining
    void start_draining(const std::string& lane_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lanes_.find(lane_name);
        if (it == lanes_.end()) return;
        it->second.draining = true;
    }

    // 对齐 OC: getLaneQueueDepth
    int get_lane_queue_depth(const std::string& lane_name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lanes_.find(lane_name);
        if (it == lanes_.end()) return 0;
        return static_cast<int>(it->second.queue.size());
    }

    // 对齐 OC: getActiveTasks
    int get_active_tasks(const std::string& lane_name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lanes_.find(lane_name);
        if (it == lanes_.end()) return 0;
        return static_cast<int>(it->second.active_task_ids.size());
    }

    // 对齐 OC: waitForIdle
    bool wait_for_idle(const std::string& lane_name, int64_t timeout_ms) {
        auto deadline = now_ms() + timeout_ms;
        while (now_ms() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = lanes_.find(lane_name);
                if (it == lanes_.end() ||
                    (it->second.queue.empty() && it->second.active_task_ids.empty())) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

private:
    CommandQueue() = default;

    LaneState& get_or_create_lane(const std::string& name) {
        auto it = lanes_.find(name);
        if (it != lanes_.end()) return it->second;
        auto& lane = lanes_[name];
        lane.lane = name;
        return lane;
    }

    void maybe_dispatch_lane(LaneState& lane) {
        while (!lane.queue.empty() &&
               static_cast<int>(lane.active_task_ids.size()) < lane.max_concurrent) {
            auto entry = std::move(lane.queue.front());
            lane.queue.pop();

            int task_id = lane.next_task_id++;
            lane.active_task_ids.insert(task_id);

            // Dispatch asynchronously
            std::string lane_name = lane.lane;
            int gen = lane.generation;

            std::thread([this, entry = std::move(entry), task_id, lane_name, gen]() {
                try {
                    entry.task();
                } catch (...) {
                    if (entry.on_error) {
                        entry.on_error(std::current_exception());
                    }
                }

                // Task completed — remove from active and dispatch next
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = lanes_.find(lane_name);
                if (it != lanes_.end() && it->second.generation == gen) {
                    it->second.active_task_ids.erase(task_id);
                    maybe_dispatch_lane(it->second);
                }
            }).detach();
        }
    }

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    mutable std::mutex mutex_;
    std::map<std::string, LaneState> lanes_;
};

// =========================================================================
// 对齐 OC exec.ts: Process Execution
// =========================================================================

struct ExecResult {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
    bool timed_out = false;
    std::string error;
};

struct ExecOptions {
    std::string working_dir;
    std::map<std::string, std::string> env;
    int64_t timeout_ms = 0;
    bool capture_output = true;
    int max_output_bytes = 1024 * 1024;  // 1MB
};

// 对齐 OC: runExec (simplified fork+exec)
ExecResult run_exec(const std::string& command,
                     const std::vector<std::string>& args,
                     const ExecOptions& options) {
    ExecResult result;

    // Build full command line
    std::string full_cmd = command;
    for (const auto& arg : args) {
        full_cmd += " '";
        // Simple shell quoting
        for (char c : arg) {
            if (c == '\'') full_cmd += "'\\''";
            else full_cmd += c;
        }
        full_cmd += "'";
    }

    if (!options.working_dir.empty()) {
        full_cmd = "cd '" + options.working_dir + "' && " + full_cmd;
    }

    // Add env vars
    std::string env_prefix;
    for (const auto& [key, value] : options.env) {
        env_prefix += key + "='" + value + "' ";
    }
    if (!env_prefix.empty()) {
        full_cmd = env_prefix + full_cmd;
    }

    // Redirect stderr to stdout for combined capture
    if (options.capture_output) {
        full_cmd += " 2>&1";
    }

    // Execute via popen
    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) {
        result.error = "Failed to execute command";
        return result;
    }

    // Read output
    std::string output;
    std::array<char, 4096> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
        if (options.max_output_bytes > 0 &&
            static_cast<int>(output.size()) > options.max_output_bytes) {
            output = output.substr(0, options.max_output_bytes);
            break;
        }
    }

    int status = pclose(pipe);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.stdout_text = output;

    return result;
}

// 对齐 OC exec.ts: execFileAsync equivalent
ExecResult exec_file_async(const std::string& file,
                            const std::vector<std::string>& args,
                            const ExecOptions& options) {
    return run_exec(file, args, options);
}

// 对齐 OC exec.ts: captureExec — run and capture stdout+stderr
ExecResult capture_exec(const std::string& command,
                         const std::vector<std::string>& args,
                         int64_t timeout_ms) {
    ExecOptions opts;
    opts.timeout_ms = timeout_ms;
    opts.capture_output = true;
    return run_exec(command, args, opts);
}

}  // namespace process
}  // namespace hunterclaw
