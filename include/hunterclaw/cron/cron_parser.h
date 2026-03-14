#pragma once

#include <string>
#include <vector>
#include <optional>
#include <ctime>
#include <chrono>

namespace hunterclaw {
namespace cron {

/**
 * Cron Expression Parser (C++ 实现)
 * 支持标准的 5 字段和 6 字段 Cron 表达式
 * 
 * 格式:
 * - 5字段: "minute hour day month weekday"
 * - 6字段: "second minute hour day month weekday"
 * 
 * 支持的语法:
 * - 数字: 0, 15, 30
 * - 通配符: *
 * - 范围: 1-5, 10-20
 * - 列表: 1,3,5,7
 * - 步长: star/5, 0-30/10
 * - 特殊字符: L (last), W (weekday), hash (nth day)
 * 
 * 示例:
 * - "0 7 * * *"         每天早上7点
 * - "star/15 * * * *"   每15分钟
 * - "0 0 1 * *"         每月1号午夜
 * - "0 9-17 * * 1-5"    工作日 9:00-17:00 每小时
 * - "0 0 7 * * *"       每天早上7点 (6字段)
 */
class CronExpression {
public:
    /**
     * 解析 Cron 表达式
     * @param expr Cron 表达式字符串
     * @param use_seconds 是否使用6字段格式 (包含秒字段)
     * @return 解析成功返回 CronExpression, 失败返回 nullopt
     */
    static std::optional<CronExpression> parse(
        const std::string& expr,
        bool use_seconds = false
    );

    /**
     * 计算下次运行时间
     * @param from_time 从此时间开始计算 (默认当前时间)
     * @return 下次运行的时间点
     */
    std::chrono::system_clock::time_point next(
        std::chrono::system_clock::time_point from_time = std::chrono::system_clock::now()
    ) const;

    /**
     * 计算下次运行时间 (毫秒时间戳)
     * @param from_ms 从此时间戳开始计算 (毫秒)
     * @return 下次运行的毫秒时间戳
     */
    int64_t next_ms(int64_t from_ms) const;

    /**
     * 检查指定时间是否匹配此 Cron 表达式
     * @param time 要检查的时间点
     * @return 匹配返回 true
     */
    bool matches(std::chrono::system_clock::time_point time) const;

    /**
     * 获取原始表达式字符串
     */
    std::string to_string() const { return expr_; }

private:
    // 私有构造函数,只能通过 parse() 创建
    CronExpression(
        const std::string& expr,
        bool use_seconds,
        std::vector<int> seconds,
        std::vector<int> minutes,
        std::vector<int> hours,
        std::vector<int> days,
        std::vector<int> months,
        std::vector<int> weekdays
    );

    // 解析单个字段 (支持 *, 数字, 范围, 列表, 步长)
    static std::optional<std::vector<int>> parse_field(
        const std::string& field,
        int min_val,
        int max_val
    );

    // 检查时间是否匹配字段
    bool matches_field(const std::vector<int>& field, int value) const;

    // 增加时间到下一个匹配点
    void increment_to_next_match(std::tm& tm) const;

private:
    std::string expr_;           // 原始表达式
    bool use_seconds_;           // 是否使用6字段格式
    std::vector<int> seconds_;   // 0-59
    std::vector<int> minutes_;   // 0-59
    std::vector<int> hours_;     // 0-23
    std::vector<int> days_;      // 1-31
    std::vector<int> months_;    // 1-12
    std::vector<int> weekdays_;  // 0-6 (0=Sunday)
};

/**
 * 便捷函数: 直接解析表达式并计算下次运行时间
 * 
 * @param expr Cron 表达式
 * @param from_ms 从此时间戳开始计算 (毫秒)
 * @param use_seconds 是否使用6字段格式
 * @return 下次运行的毫秒时间戳, 解析失败返回 nullopt
 */
std::optional<int64_t> cron_next(
    const std::string& expr,
    int64_t from_ms,
    bool use_seconds = false
);

/**
 * 验证 Cron 表达式是否合法
 * 
 * @param expr Cron 表达式
 * @param use_seconds 是否使用6字段格式
 * @return 合法返回 true
 */
bool validate_cron_expression(
    const std::string& expr,
    bool use_seconds = false
);

} // namespace cron
} // namespace hunterclaw
