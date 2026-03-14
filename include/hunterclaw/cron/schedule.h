#pragma once

#include "hunterclaw/cron/types.h"
#include "hunterclaw/core/types.h"
#include <optional>
#include <string>

namespace hunterclaw {
namespace cron {

// ============================================================================
// Schedule Computation (对齐 OpenClaw schedule.ts)
// ============================================================================

/**
 * 计算下次运行时间
 * 
 * @param schedule 调度配置
 * @param now_ms 当前时间戳(毫秒)
 * @return 下次运行时间(毫秒), 如果没有下次运行返回 nullopt
 */
std::optional<int64_t> compute_next_run_at_ms(
    const Schedule& schedule,
    int64_t now_ms
);

/**
 * 解析绝对时间字符串为毫秒时间戳
 * 支持格式: ISO8601, Unix时间戳, 相对时间
 * 
 * @param time_str 时间字符串 (e.g., "2026-03-10T14:00:00Z", "1735473600000", "+1h")
 * @return 毫秒时间戳, 解析失败返回 nullopt
 */
std::optional<int64_t> parse_absolute_time_ms(const std::string& time_str);

/**
 * 解析 Cron 表达式并计算下次运行时间
 * 使用 ccronexpr 库解析标准 cron 表达式 (5字段或6字段)
 * 
 * @param expr Cron 表达式 (e.g., "0 7 * * *", "0 0 7 * * *")
 * @param now_ms 当前时间戳(毫秒)
 * @param tz 时区 (可选, 默认使用系统时区)
 * @return 下次运行时间(毫秒), 解析失败返回 nullopt
 */
std::optional<int64_t> parse_cron_next_ms(
    const std::string& expr,
    int64_t now_ms,
    const std::optional<std::string>& tz = std::nullopt
);

/**
 * 检查 Cron 表达式是否为整点表达式 (用于自动错峰)
 * 
 * @param expr Cron 表达式
 * @return 如果是整点表达式 (如 "0 * * * *") 返回 true
 */
bool is_top_of_hour_expression(const std::string& expr);

/**
 * 为 Job 计算确定性的错峰偏移
 * 基于 job_id 的 SHA256 哈希计算稳定的偏移量
 * 
 * @param job_id 任务ID
 * @param stagger_ms 最大偏移量(毫秒)
 * @return 错峰偏移(毫秒), 范围 [0, stagger_ms)
 */
int64_t compute_stable_stagger_offset(const std::string& job_id, int64_t stagger_ms);

/**
 * 获取默认的错峰偏移量
 * 整点表达式默认5分钟, 其他表达式不错峰
 * 
 * @param expr Cron 表达式
 * @return 默认错峰偏移(毫秒), 不需要错峰返回 nullopt
 */
std::optional<int64_t> get_default_stagger_ms(const std::string& expr);

} // namespace cron
} // namespace hunterclaw
