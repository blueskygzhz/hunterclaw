#include "hunterclaw/queue/command_queue.h"
#include "hunterclaw/auto_reply/message_dispatcher.h"  // DispatchResult
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <map>
#include <unordered_map>
#include <chrono>
#include <iostream>

namespace hunterclaw {
namespace queue {

// 任务包装器
struct TaskWrapper {
    std::function<void()> task;
    std::string label;
    bool urgent;
    uint64_t enqueue_time_ms;
    uint64_t generation;
    
    bool operator<(const TaskWrapper& other) const {
        // 紧急任务优先
        if (urgent != other.urgent) {
            return !urgent;  // priority_queue是大顶堆，取反
        }
        // 否则按入队时间（FIFO）
        return enqueue_time_ms > other.enqueue_time_ms;
    }
};

// 通道状态
struct LaneState {
    std::priority_queue<TaskWrapper> queue;
    size_t max_concurrent = 1;  // 默认串行（对齐 OpenClaw: session lane 默认 maxConcurrent=1）
    size_t active_tasks = 0;
    size_t completed_tasks = 0;
    size_t failed_tasks = 0;
    uint64_t total_wait_ms = 0;
    uint64_t max_wait_ms = 0;
    uint64_t generation = 0;  // 代际计数（用于重启恢复）
    bool cleared = false;
    bool is_dynamic = false;  // 是否为动态字符串 lane（如 session:xxx）
};

// CommandQueue实现类
class CommandQueue::Impl {
public:
    Impl() : gateway_draining_(false), shutdown_(false) {
        // 初始化固定枚举通道
        // 对齐 OpenClaw server-lanes.ts: applyGatewayLaneConcurrency
        lanes_[CommandLane::Main].max_concurrent = 1;      // 主通道串行（由配置覆盖）
        lanes_[CommandLane::Cron].max_concurrent = 1;      // Cron通道（由配置覆盖）
        lanes_[CommandLane::Subagent].max_concurrent = 8;  // 子代理通道（对齐 OpenClaw DEFAULT_SUBAGENT_MAX_CONCURRENT=8，由配置覆盖）
        lanes_[CommandLane::Nested].max_concurrent = 2;    // 嵌套通道
        
        // 启动工作线程（每个固定通道一个）
        for (auto& pair : lanes_) {
            CommandLane lane = pair.first;
            worker_threads_.emplace_back([this, lane]() {
                WorkerThread(lane);
            });
        }
        
        // 启动动态 lane 调度线程（处理 session:xxx 等动态 lane）
        // 对齐 OpenClaw: 每个动态 lane 独立串行，通过单一调度线程轮询
        dynamic_worker_thread_ = std::thread([this]() {
            DynamicLaneWorkerThread();
        });
    }
    
    ~Impl() {
        Shutdown();
    }
    
    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
        dynamic_cv_.notify_all();
        
        // 等待所有工作线程退出
        for (auto& thread : worker_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        if (dynamic_worker_thread_.joinable()) {
            dynamic_worker_thread_.join();
        }
    }
    
    void EnqueueToLane(CommandLane lane, std::function<void()> task, const EnqueueOptions& opts) {
        // 检查排空状态
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (gateway_draining_) {
                throw GatewayDrainingError();
            }
        }

        // 入队
        // ✅ 对齐 OpenClaw: enqueueCommandInLane 只检查 gatewayDraining，
        // 不检查 lane 级别的 cleared 标志。clear 后仍可正常 enqueue。
        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto& state = lanes_[lane];

            TaskWrapper wrapper;
            wrapper.task = std::move(task);
            wrapper.label = opts.label;
            wrapper.urgent = opts.urgent;
            wrapper.enqueue_time_ms = GetCurrentTimeMs();
            wrapper.generation = state.generation;

            state.queue.push(std::move(wrapper));
        }

        cv_.notify_one();
    }

    // ── 动态字符串 Lane 入队（对齐 OpenClaw enqueueCommandInLane(lane: string, ...)）──
    void EnqueueToDynamicLane(const std::string& lane_name, std::function<void()> task, const EnqueueOptions& opts) {
        // 检查排空状态
        {
            std::lock_guard<std::mutex> lock(dynamic_mutex_);
            if (gateway_draining_) {
                throw GatewayDrainingError();
            }
        }

        {
            std::lock_guard<std::mutex> lock(dynamic_mutex_);
            auto& state = GetOrCreateDynamicLane(lane_name);

            TaskWrapper wrapper;
            wrapper.task = std::move(task);
            wrapper.label = opts.label;
            wrapper.urgent = opts.urgent;
            wrapper.enqueue_time_ms = GetCurrentTimeMs();
            wrapper.generation = state.generation;

            state.queue.push(std::move(wrapper));
        }

        dynamic_cv_.notify_one();
    }
    
    void SetConcurrency(CommandLane lane, size_t max_concurrent) {
        std::lock_guard<std::mutex> lock(mutex_);
        lanes_[lane].max_concurrent = max_concurrent;
        cv_.notify_all();  // 唤醒工作线程重新检查
    }

    void SetDynamicLaneConcurrency(const std::string& lane_name, size_t max_concurrent) {
        std::lock_guard<std::mutex> lock(dynamic_mutex_);
        auto& state = GetOrCreateDynamicLane(lane_name);
        state.max_concurrent = std::max<size_t>(1, max_concurrent);
        dynamic_cv_.notify_all();
    }
    
    // ✅ 严格对齐 OpenClaw: clearCommandLane (command-queue.ts:223-235)
    // OpenClaw 实现:
    //   const pending = state.queue.splice(0);
    //   for (const entry of pending) { entry.reject(new CommandLaneClearedError(cleaned)); }
    //   return removed;
    //
    // 关键: clearCommandLane **不设置任何标志**，不阻止后续 enqueue。
    // 它只是清空队列中等待执行的任务。interrupt 模式依赖此行为:
    //   1. clearCommandLane(sessionLane)  ← 清空 pending
    //   2. abortEmbeddedPiRun(sessionId)  ← 取消 active
    //   3. enqueueCommandInLane(sessionLane, newTask)  ← 新任务入队（必须成功！）
    //
    // 如果 clear 设置 cleared=true，步骤 3 会 throw CommandLaneClearedError，
    // 导致 interrupt 后新 run 无法入队，这是错误行为。
    int ClearLane(CommandLane lane) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = lanes_[lane];
        
        int removed = 0;
        while (!state.queue.empty()) {
            state.queue.pop();
            removed++;
        }
        
        // ✅ 对齐 OpenClaw: 不设置 cleared 标志，不阻止后续 enqueue
        // state.cleared = true;  ← 删除！OpenClaw 没有此行为
        return removed;
    }

    int ClearDynamicLane(const std::string& lane_name) {
        std::lock_guard<std::mutex> lock(dynamic_mutex_);
        auto it = dynamic_lanes_.find(lane_name);
        if (it == dynamic_lanes_.end()) {
            return 0;
        }
        int removed = 0;
        while (!it->second.queue.empty()) {
            it->second.queue.pop();
            removed++;
        }
        // ✅ 对齐 OpenClaw: 不设置 cleared 标志
        // it->second.cleared = true;  ← 删除！
        return removed;
    }
    
    void ResetAllLanes() {
        // 重置固定枚举 lane
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& pair : lanes_) {
                auto& state = pair.second;
                while (!state.queue.empty()) {
                    state.queue.pop();
                }
                state.generation++;
                state.cleared = false;
            }
            cv_.notify_all();
        }
        // 重置动态 lane（对齐 OpenClaw resetAllLanes）
        {
            std::lock_guard<std::mutex> lock(dynamic_mutex_);
            for (auto& pair : dynamic_lanes_) {
                auto& state = pair.second;
                while (!state.queue.empty()) {
                    state.queue.pop();
                }
                state.generation++;
                state.active_tasks = 0;
                state.cleared = false;
            }
            dynamic_cv_.notify_all();
        }
    }
    
    void MarkGatewayDraining() {
        std::lock_guard<std::mutex> lock(mutex_);
        gateway_draining_ = true;
    }
    
    void UnmarkGatewayDraining() {
        std::lock_guard<std::mutex> lock(mutex_);
        gateway_draining_ = false;
    }
    
    size_t GetQueueSize(CommandLane lane) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lanes_.find(lane);
        return (it != lanes_.end()) ? it->second.queue.size() : 0;
    }

    size_t GetDynamicQueueSize(const std::string& lane_name) const {
        std::lock_guard<std::mutex> lock(dynamic_mutex_);
        auto it = dynamic_lanes_.find(lane_name);
        return (it != dynamic_lanes_.end()) ? it->second.queue.size() + it->second.active_tasks : 0;
    }
    
    size_t GetTotalQueueSize() const {
        size_t total = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& pair : lanes_) {
                total += pair.second.queue.size() + pair.second.active_tasks;
            }
        }
        {
            std::lock_guard<std::mutex> lock(dynamic_mutex_);
            for (const auto& pair : dynamic_lanes_) {
                total += pair.second.queue.size() + pair.second.active_tasks;
            }
        }
        return total;
    }
    
    size_t GetActiveTaskCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (const auto& pair : lanes_) {
            total += pair.second.active_tasks;
        }
        return total;
    }
    
    QueueStats GetStats(CommandLane lane) const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        QueueStats stats = {};
        auto it = lanes_.find(lane);
        if (it != lanes_.end()) {
            const auto& state = it->second;
            stats.queue_size = state.queue.size();
            stats.active_tasks = state.active_tasks;
            stats.completed_tasks = state.completed_tasks;
            stats.failed_tasks = state.failed_tasks;
            stats.total_wait_ms = state.total_wait_ms;
            stats.max_wait_ms = state.max_wait_ms;
        }
        
        return stats;
    }
    
    Result<void> WaitForActiveTasks(uint32_t timeout_ms) {
        auto start = std::chrono::steady_clock::now();
        
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                
                if (GetActiveTaskCount() == 0) {
                    return Result<void>::Ok();
                }
                
                // 等待任务完成或超时
                if (timeout_ms > 0) {
                    auto elapsed = std::chrono::steady_clock::now() - start;
                    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                    
                    if (elapsed_ms >= timeout_ms) {
                        return Result<void>::Err("Wait for active tasks timeout");
                    }
                    
                    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms - elapsed_ms));
                } else {
                    cv_.wait(lock);
                }
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
private:
    // 获取或创建动态 lane（调用方需持有 dynamic_mutex_）
    LaneState& GetOrCreateDynamicLane(const std::string& lane_name) {
        auto it = dynamic_lanes_.find(lane_name);
        if (it != dynamic_lanes_.end()) {
            return it->second;
        }
        LaneState& state = dynamic_lanes_[lane_name];
        state.max_concurrent = 1;  // 默认串行（对齐 OpenClaw session lane）
        state.is_dynamic = true;
        return state;
    }

    // 动态 Lane 调度线程（对齐 OpenClaw drainLane 的 pump 逻辑）
    // 单线程轮询所有动态 lane，按 maxConcurrent 控制并发
    void DynamicLaneWorkerThread() {
        while (true) {
            std::vector<std::pair<std::string, TaskWrapper>> to_run;

            {
                std::unique_lock<std::mutex> lock(dynamic_mutex_);
                dynamic_cv_.wait(lock, [&]() {
                    if (shutdown_) return true;
                    for (auto& pair : dynamic_lanes_) {
                        auto& state = pair.second;
                        if (!state.queue.empty() && state.active_tasks < state.max_concurrent) {
                            return true;
                        }
                    }
                    return false;
                });

                if (shutdown_) break;

                // 收集可执行任务（每个 lane 按 maxConcurrent 限制）
                for (auto& pair : dynamic_lanes_) {
                    auto& state = pair.second;
                    while (!state.queue.empty() && state.active_tasks < state.max_concurrent) {
                        TaskWrapper wrapper = std::move(
                            const_cast<TaskWrapper&>(state.queue.top())
                        );
                        state.queue.pop();
                        state.active_tasks++;
                        to_run.emplace_back(pair.first, std::move(wrapper));
                    }
                }
            }

            // 执行任务（无锁，每个任务在独立线程中执行）
            for (auto& [lane_name, wrapper] : to_run) {
                std::thread([this, lane_name, wrapper = std::move(wrapper)]() mutable {
                    uint64_t start_time = GetCurrentTimeMs();
                    uint64_t wait_ms = start_time - wrapper.enqueue_time_ms;

                    try {
                        wrapper.task();
                    } catch (const std::exception& e) {
                        std::cerr << "[DynamicLane] Task failed in lane " << lane_name
                                  << ": " << e.what() << std::endl;
                    } catch (...) {
                        std::cerr << "[DynamicLane] Task failed in lane " << lane_name
                                  << ": unknown exception" << std::endl;
                    }

                    {
                        std::lock_guard<std::mutex> lock(dynamic_mutex_);
                        auto it = dynamic_lanes_.find(lane_name);
                        if (it != dynamic_lanes_.end()) {
                            it->second.active_tasks--;
                            it->second.completed_tasks++;
                            it->second.total_wait_ms += wait_ms;
                            it->second.max_wait_ms = std::max(it->second.max_wait_ms, wait_ms);
                        }
                    }
                    // 通知调度线程有新的槽位可用
                    dynamic_cv_.notify_one();
                }).detach();
            }
        }
    }

    void WorkerThread(CommandLane lane) {
        while (true) {
            TaskWrapper wrapper;
            bool has_task = false;
            
            // 获取任务
            {
                std::unique_lock<std::mutex> lock(mutex_);
                
                auto& state = lanes_[lane];
                
                // 等待任务或关闭信号
                cv_.wait(lock, [&]() {
                    return shutdown_ || 
                           (!state.queue.empty() && state.active_tasks < state.max_concurrent);
                });
                
                if (shutdown_) {
                    break;
                }
                
                if (!state.queue.empty() && state.active_tasks < state.max_concurrent) {
                    wrapper = std::move(const_cast<TaskWrapper&>(state.queue.top()));
                    state.queue.pop();
                    state.active_tasks++;
                    has_task = true;
                }
            }
            
            // 执行任务（无锁）
            if (has_task) {
                uint64_t start_time = GetCurrentTimeMs();
                uint64_t wait_ms = start_time - wrapper.enqueue_time_ms;
                
                try {
                    wrapper.task();
                    
                    // 更新统计
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto& state = lanes_[lane];
                    state.active_tasks--;
                    state.completed_tasks++;
                    state.total_wait_ms += wait_ms;
                    state.max_wait_ms = std::max(state.max_wait_ms, wait_ms);
                    
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto& state = lanes_[lane];
                    state.active_tasks--;
                    state.failed_tasks++;
                    
                    std::cerr << "Task failed in lane " << GetCommandLaneName(lane) 
                              << ": " << e.what() << std::endl;
                }
                
                cv_.notify_all();  // 唤醒等待的线程
            }
        }
    }
    
    static uint64_t GetCurrentTimeMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }
    
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<CommandLane, LaneState> lanes_;
    std::vector<std::thread> worker_threads_;

    // 动态字符串 Lane（对齐 OpenClaw: Map<string, LaneState>）
    mutable std::mutex dynamic_mutex_;
    std::condition_variable dynamic_cv_;
    std::unordered_map<std::string, LaneState> dynamic_lanes_;
    std::thread dynamic_worker_thread_;

    std::atomic<bool> gateway_draining_;
    std::atomic<bool> shutdown_;
};

// ============================================================================
// CommandQueue公共接口实现
// ============================================================================

CommandQueue::CommandQueue() : impl_(std::make_unique<Impl>()) {}

CommandQueue::~CommandQueue() = default;

CommandQueue& CommandQueue::Instance() {
    static CommandQueue instance;
    return instance;
}

void CommandQueue::enqueue_to_lane(
    CommandLane lane,
    std::function<void()> task,
    const EnqueueOptions& opts) {
    impl_->EnqueueToLane(lane, std::move(task), opts);
}

void CommandQueue::enqueue_to_dynamic_lane(
    const std::string& lane_name,
    std::function<void()> task,
    const EnqueueOptions& opts) {
    impl_->EnqueueToDynamicLane(lane_name, std::move(task), opts);
}

void CommandQueue::set_command_lane_concurrency(CommandLane lane, size_t max_concurrent) {
    impl_->SetConcurrency(lane, max_concurrent);
}

void CommandQueue::set_command_lane_concurrency(const std::string& lane_name, size_t max_concurrent) {
    impl_->SetDynamicLaneConcurrency(lane_name, max_concurrent);
}

void CommandQueue::clear_command_lane(CommandLane lane) {
    impl_->ClearLane(lane);
}

void CommandQueue::clear_command_lane(const std::string& lane_name) {
    impl_->ClearDynamicLane(lane_name);
}

void CommandQueue::reset_all_lanes() {
    impl_->ResetAllLanes();
}

void CommandQueue::mark_gateway_draining() {
    impl_->MarkGatewayDraining();
}

void CommandQueue::unmark_gateway_draining() {
    impl_->UnmarkGatewayDraining();
}

size_t CommandQueue::get_queue_size(CommandLane lane) const {
    return impl_->GetQueueSize(lane);
}

size_t CommandQueue::get_queue_size(const std::string& lane_name) const {
    return impl_->GetDynamicQueueSize(lane_name);
}

size_t CommandQueue::get_total_queue_size() const {
    return impl_->GetTotalQueueSize();
}

size_t CommandQueue::get_active_task_count() const {
    return impl_->GetActiveTaskCount();
}

QueueStats CommandQueue::get_stats(CommandLane lane) const {
    return impl_->GetStats(lane);
}

Result<void> CommandQueue::wait_for_active_tasks(uint32_t timeout_ms) {
    return impl_->WaitForActiveTasks(timeout_ms);
}

Result<void> CommandQueue::wait_for_lane_tasks(CommandLane lane, uint32_t timeout_ms) {
    // 简化实现：只检查指定通道
    auto start = std::chrono::steady_clock::now();
    
    while (true) {
        auto stats = get_stats(lane);
        if (stats.active_tasks == 0) {
            return Result<void>::Ok();
        }
        
        if (timeout_ms > 0) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            
            if (elapsed_ms >= timeout_ms) {
                return Result<void>::Err("Wait for lane tasks timeout");
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace queue
}  // namespace hunterclaw
