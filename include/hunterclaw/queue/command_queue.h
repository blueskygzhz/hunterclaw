#pragma once

#include "hunterclaw/queue/lanes.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <functional>
#include <future>
#include <chrono>
#include <stdexcept>

namespace hunterclaw {
namespace queue {

// 引入 Result 类型
using hunterclaw::core::Result;

// 命令队列错误
class CommandLaneClearedError : public std::runtime_error {
public:
    explicit CommandLaneClearedError(CommandLane lane)
        : std::runtime_error(
            std::string("Command lane cleared: ") + GetCommandLaneName(lane)
          ), 
          lane_(lane) {}
    
    CommandLane lane() const { return lane_; }
    
private:
    CommandLane lane_;
};

class GatewayDrainingError : public std::runtime_error {
public:
    GatewayDrainingError()
        : std::runtime_error("Gateway is draining, no new commands accepted") {}
};

// 入队选项
struct EnqueueOptions {
    std::string label;          // 任务标签（用于日志）
    bool urgent = false;        // 紧急任务（优先执行）
    uint32_t timeout_ms = 0;    // 任务超时（0=无限）
};

// 队列统计信息
struct QueueStats {
    size_t queue_size;          // 队列中等待任务数
    size_t active_tasks;        // 正在执行任务数
    size_t completed_tasks;     // 已完成任务数
    size_t failed_tasks;        // 失败任务数
    uint64_t total_wait_ms;     // 累计等待时间
    uint64_t max_wait_ms;       // 最大等待时间
};

// 命令队列管理器（单例）
class CommandQueue {
public:
    // 获取单例实例
    static CommandQueue& Instance();
    
    // 禁止拷贝和移动
    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;
    
    // 入队（主通道，串行执行）
    template<typename Func>
    auto enqueue_command(Func&& task, const EnqueueOptions& opts = {})
        -> std::future<typename std::invoke_result<Func>::type> {
        return enqueue_command_in_lane(CommandLane::Main, std::forward<Func>(task), opts);
    }
    
    // 入队（指定枚举通道）
    template<typename Func>
    auto enqueue_command_in_lane(
        CommandLane lane, 
        Func&& task, 
        const EnqueueOptions& opts = {}
    ) -> std::future<typename std::invoke_result<Func>::type>;

    // ── 动态字符串 Lane（对齐 OpenClaw enqueueCommandInLane(lane: string, ...)）──
    // 支持 session:xxx 等动态 lane 名称，每个 lane 默认 maxConcurrent=1（串行）
    template<typename Func>
    auto enqueue_command_in_lane(
        const std::string& lane_name,
        Func&& task,
        const EnqueueOptions& opts = {}
    ) -> std::future<typename std::invoke_result<Func>::type>;

    // 设置通道并发度（枚举版本）
    void set_command_lane_concurrency(CommandLane lane, size_t max_concurrent);

    // 设置通道并发度（字符串版本，用于动态 session lane）
    // 对齐 OpenClaw: setCommandLaneConcurrency(lane: string, maxConcurrent: number)
    void set_command_lane_concurrency(const std::string& lane_name, size_t max_concurrent);

    // 清空通道队列
    // 将队列中未执行的任务标记为取消
    void clear_command_lane(CommandLane lane);

    // 清空字符串通道队列
    void clear_command_lane(const std::string& lane_name);

    // 重置所有通道（用于重启恢复）
    // 清空队列，递增generation，等待活动任务完成
    void reset_all_lanes();
    
    // 标记网关排空状态
    // 排空状态下，新任务将被拒绝（GatewayDrainingError）
    void mark_gateway_draining();
    
    // 取消排空状态
    void unmark_gateway_draining();
    
    // 获取队列大小
    size_t get_queue_size(CommandLane lane) const;

    // 获取字符串 lane 队列大小
    size_t get_queue_size(const std::string& lane_name) const;
    
    // 获取所有通道的总队列大小
    size_t get_total_queue_size() const;
    
    // 获取活动任务数
    size_t get_active_task_count() const;
    
    // 获取队列统计信息
    QueueStats get_stats(CommandLane lane) const;
    
    // 等待活动任务完成
    // 参数:
    //   timeout_ms: 超时时间（毫秒），0=无限
    // 返回:
    //   Result<void> 成功或超时错误
    Result<void> wait_for_active_tasks(uint32_t timeout_ms = 0);
    
    // 等待指定通道的活动任务完成
    Result<void> wait_for_lane_tasks(CommandLane lane, uint32_t timeout_ms = 0);
    
private:
    CommandQueue();
    ~CommandQueue();
    
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace queue
}  // namespace hunterclaw
