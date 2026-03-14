#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>

namespace hunterclaw {
namespace core {

/// 线程池 - 用于并行执行任务
/// 对齐 OpenClaw: 支持动态任务提交和并发控制
class ThreadPool {
public:
    /// 构造函数
    /// @param num_threads 线程数量 (0 = 自动检测 CPU 核心数)
    explicit ThreadPool(size_t num_threads = 0);
    
    /// 析构函数 - 自动关闭并等待所有任务完成
    ~ThreadPool();
    
    /// 提交任务 (返回 Future)
    /// @param func 任务函数
    /// @param args 任务参数
    /// @return Future<结果类型>
    template<typename Func, typename... Args>
    auto submit(Func&& func, Args&&... args)
        -> std::future<typename std::invoke_result<Func, Args...>::type>;
    
    /// 获取线程数量
    size_t num_threads() const { return workers_.size(); }
    
    /// 获取待处理任务数量
    size_t pending_tasks() const;
    
    /// 等待所有任务完成
    void wait();
    
    /// 关闭线程池 (不再接受新任务)
    void shutdown();
    
private:
    /// 工作线程
    std::vector<std::thread> workers_;
    
    /// 任务队列
    std::queue<std::function<void()>> tasks_;
    
    /// 同步原语
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable wait_condition_;
    
    /// 状态标志
    std::atomic<bool> stop_{false};
    std::atomic<size_t> active_tasks_{0};
    
    /// 工作线程循环
    void worker_thread();
};

// ============================================================================
// Template Implementation
// ============================================================================

template<typename Func, typename... Args>
auto ThreadPool::submit(Func&& func, Args&&... args)
    -> std::future<typename std::invoke_result<Func, Args...>::type>
{
    using return_type = typename std::invoke_result<Func, Args...>::type;
    
    // 创建 packaged_task
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
    );
    
    std::future<return_type> result = task->get_future();
    
    {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (stop_) {
            throw std::runtime_error("ThreadPool is stopped");
        }
        
        // 包装任务
        tasks_.emplace([task]() {
            (*task)();
        });
    }
    
    condition_.notify_one();
    
    return result;
}

} // namespace core
} // namespace hunterclaw
