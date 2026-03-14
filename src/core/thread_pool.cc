#include "hunterclaw/core/thread_pool.h"
#include <algorithm>

namespace hunterclaw {
namespace core {

ThreadPool::ThreadPool(size_t num_threads) {
    // 自动检测 CPU 核心数
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        // 最少 2 个,最多 8 个
        num_threads = std::max<size_t>(2, std::min<size_t>(8, num_threads));
    }
    
    // 创建工作线程
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] { worker_thread(); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

size_t ThreadPool::pending_tasks() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return tasks_.size();
}

void ThreadPool::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    wait_condition_.wait(lock, [this] {
        return tasks_.empty() && active_tasks_ == 0;
    });
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stop_) {
            return;  // 已经关闭
        }
        stop_ = true;
    }
    
    condition_.notify_all();
    
    // 等待所有工作线程结束
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::worker_thread() {
    while (true) {
        std::function<void()> task;
        
        {
            std::unique_lock<std::mutex> lock(mutex_);
            
            // 等待任务或停止信号
            condition_.wait(lock, [this] {
                return stop_ || !tasks_.empty();
            });
            
            // 停止且没有任务,退出
            if (stop_ && tasks_.empty()) {
                return;
            }
            
            // 取出任务
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
                ++active_tasks_;
            }
        }
        
        // 执行任务 (不持有锁)
        if (task) {
            task();
            --active_tasks_;
            
            // 通知 wait() 可能所有任务都完成了
            wait_condition_.notify_all();
        }
    }
}

} // namespace core
} // namespace hunterclaw
