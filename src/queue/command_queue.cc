#include "hunterclaw/queue/command_queue.h"
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <map>
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
    size_t max_concurrent = 1;  // 默认串行
    size_t active_tasks = 0;
    size_t completed_tasks = 0;
    size_t failed_tasks = 0;
    uint64_t total_wait_ms = 0;
    uint64_t max_wait_ms = 0;
    uint64_t generation = 0;  // 代际计数（用于重启恢复）
    bool cleared = false;
};

// CommandQueue实现类
class CommandQueue::Impl {
public:
    Impl() : gateway_draining_(false), shutdown_(false) {
        // 初始化通道
        lanes_[CommandLane::Main].max_concurrent = 1;  // 主通道串行
        lanes_[CommandLane::Cron].max_concurrent = 2;  // Cron通道2并发
        lanes_[CommandLane::Subagent].max_concurrent = 4;  // 子代理4并发
        lanes_[CommandLane::Nested].max_concurrent = 2;  // 嵌套2并发
        
        // 启动工作线程（每个通道一个）
        for (auto& pair : lanes_) {
            CommandLane lane = pair.first;
            worker_threads_.emplace_back([this, lane]() {
                WorkerThread(lane);
            });
        }
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
        
        // 等待所有工作线程退出
        for (auto& thread : worker_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }
    
    template<typename Func>
    auto EnqueueInLane(
        CommandLane lane, 
        Func&& task, 
        const EnqueueOptions& opts
    ) -> std::future<typename std::invoke_result<Func>::type> {
        using ReturnType = typename std::invoke_result<Func>::type;
        
        // 检查排空状态
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (gateway_draining_) {
                throw GatewayDrainingError();
            }
        }
        
        // 创建packaged_task
        auto promise_ptr = std::make_shared<std::promise<ReturnType>>();
        auto future = promise_ptr->get_future();
        
        // 包装任务
        auto wrapped_task = [task = std::forward<Func>(task), promise_ptr]() mutable {
            try {
                if constexpr (std::is_void_v<ReturnType>) {
                    task();
                    promise_ptr->set_value();
                } else {
                    promise_ptr->set_value(task());
                }
            } catch (...) {
                promise_ptr->set_exception(std::current_exception());
            }
        };
        
        // 入队
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            auto& state = lanes_[lane];
            
            // 检查通道是否被清除
            if (state.cleared) {
                throw CommandLaneClearedError(lane);
            }
            
            TaskWrapper wrapper;
            wrapper.task = std::move(wrapped_task);
            wrapper.label = opts.label;
            wrapper.urgent = opts.urgent;
            wrapper.enqueue_time_ms = GetCurrentTimeMs();
            wrapper.generation = state.generation;
            
            state.queue.push(std::move(wrapper));
        }
        
        cv_.notify_one();
        return future;
    }
    
    void SetConcurrency(CommandLane lane, size_t max_concurrent) {
        std::lock_guard<std::mutex> lock(mutex_);
        lanes_[lane].max_concurrent = max_concurrent;
        cv_.notify_all();  // 唤醒工作线程重新检查
    }
    
    void ClearLane(CommandLane lane) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = lanes_[lane];
        
        // 清空队列
        while (!state.queue.empty()) {
            state.queue.pop();
        }
        
        state.cleared = true;
    }
    
    void ResetAllLanes() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        for (auto& pair : lanes_) {
            auto& state = pair.second;
            
            // 清空队列
            while (!state.queue.empty()) {
                state.queue.pop();
            }
            
            // 递增generation
            state.generation++;
            state.cleared = false;
        }
        
        cv_.notify_all();
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
    
    size_t GetTotalQueueSize() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (const auto& pair : lanes_) {
            total += pair.second.queue.size();
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

template<typename Func>
auto CommandQueue::enqueue_command_in_lane(
    CommandLane lane, 
    Func&& task, 
    const EnqueueOptions& opts
) -> std::future<typename std::invoke_result<Func>::type> {
    return impl_->EnqueueInLane(lane, std::forward<Func>(task), opts);
}

void CommandQueue::set_command_lane_concurrency(CommandLane lane, size_t max_concurrent) {
    impl_->SetConcurrency(lane, max_concurrent);
}

void CommandQueue::clear_command_lane(CommandLane lane) {
    impl_->ClearLane(lane);
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

// 显式实例化常用模板
template auto CommandQueue::enqueue_command_in_lane<std::function<void()>>(
    CommandLane, std::function<void()>&&, const EnqueueOptions&
) -> std::future<void>;

template auto CommandQueue::enqueue_command_in_lane<std::function<int()>>(
    CommandLane, std::function<int()>&&, const EnqueueOptions&
) -> std::future<int>;

template auto CommandQueue::enqueue_command_in_lane<std::function<std::string()>>(
    CommandLane, std::function<std::string()>&&, const EnqueueOptions&
) -> std::future<std::string>;

}  // namespace queue
}  // namespace hunterclaw
