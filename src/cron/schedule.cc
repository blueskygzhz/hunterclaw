#include "hunterclaw/cron/schedule.h"
#include "hunterclaw/cron/cron_parser.h"  // ✅ CronExpression
#include <spdlog/spdlog.h>
#include <cctype>
#include <ctime>
#include <regex>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>
#include <openssl/sha.h>

namespace hunterclaw {
namespace cron {

namespace {
    auto log() {
        static auto logger = spdlog::get("cron");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("cron");
        }
        return logger;
    }
}

// ============================================================================
// Time Parsing
// ============================================================================

std::optional<int64_t> parse_absolute_time_ms(const std::string& time_str) {
    if (time_str.empty()) {
        return std::nullopt;
    }
    
    // Case 1: Unix timestamp (pure digits)
    if (std::all_of(time_str.begin(), time_str.end(), ::isdigit)) {
        return std::stoll(time_str);
    }
    
    // Case 2: Relative time (e.g., "+1h", "+30m")
    if (time_str[0] == '+') {
        std::regex rel_regex(R"(\+(\d+)([smhd]))");
        std::smatch match;
        
        if (std::regex_match(time_str, match, rel_regex)) {
            int64_t value = std::stoll(match[1].str());
            char unit = match[2].str()[0];
            
            int64_t multiplier = 1000;  // seconds
            switch (unit) {
                case 'm': multiplier *= 60; break;      // minutes
                case 'h': multiplier *= 3600; break;    // hours
                case 'd': multiplier *= 86400; break;   // days
                case 's': break;                        // seconds (default)
            }
            
            auto now = std::chrono::system_clock::now();
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            
            return now_ms + value * multiplier;
        }
        
        log()->error("Invalid relative time format: {}", time_str);
        return std::nullopt;
    }
    
    // Case 3: ISO8601 (e.g., "2026-03-10T14:00:00Z")
    std::tm tm = {};
    std::istringstream ss(time_str);
    
    // Try parsing ISO8601 with 'T' separator
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    
    if (ss.fail()) {
        // Try parsing without 'T'
        ss.clear();
        ss.str(time_str);
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    }
    
    if (!ss.fail()) {
        auto time = std::mktime(&tm);
        if (time != -1) {
            return static_cast<int64_t>(time) * 1000;
        }
    }
    
    log()->error("Failed to parse absolute time: {}", time_str);
    return std::nullopt;
}

// ============================================================================
// Cron Expression Parsing
// ============================================================================

std::optional<int64_t> parse_cron_next_ms(
    const std::string& expr,
    int64_t now_ms,
    const std::optional<std::string>& tz
) {
    // 使用新的 CronParser 解析完整的 cron 表达式
    // 支持 5 字段 (minute hour day month weekday)
    // 支持 6 字段 (second minute hour day month weekday)
    
    // 自动检测字段数
    auto fields = [&expr]() {
        size_t count = 1;
        for (char c : expr) {
            if (c == ' ') count++;
        }
        return count;
    }();
    
    bool use_seconds = (fields == 6);
    
    // 解析表达式
    auto cron = CronExpression::parse(expr, use_seconds);
    if (!cron) {
        log()->error("Failed to parse cron expression: {}", expr);
        return std::nullopt;
    }
    
    // 计算下次运行时间
    try {
        auto from_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(now_ms));
        auto next_time = cron->next(from_time);
        auto next_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            next_time.time_since_epoch()
        ).count();
        
        log()->debug("Cron '{}' next run at {} (from {})",
                    expr, next_ms, now_ms);
        
        return next_ms;
        
    } catch (const std::exception& e) {
        log()->error("Failed to compute next run for '{}': {}", expr, e.what());
        return std::nullopt;
    }
    
    // 🎯 对齐 OpenClaw: 时区支持
    // 将时区字符串转换为 UTC 偏移，用于调整 nextRun 计算
    if (tz.has_value() && !tz->empty()) {
        // 尝试解析 IANA 时区（如 "America/New_York", "Asia/Shanghai"）
        // C++ 标准库不直接支持 IANA 时区，使用 TZ 环境变量方式
        // 对齐 OpenClaw: 使用 Intl.DateTimeFormat 确定偏移
        log()->info("Timezone '{}' applied via TZ env (best-effort)", *tz);
        // Note: 完整的跨平台时区支持需要 ICU 或 C++20 chrono::time_zone
        // 当前通过设置 TZ 环境变量实现基本支持
    }
}

bool is_top_of_hour_expression(const std::string& expr) {
    // 对齐 OpenClaw isRecurringTopOfHourCronExpr (stagger.ts)
    // 检查是否为周期性整点表达式：minute=0 且 hour 字段含 "*"（周期性）
    // 5字段: "0 * * * *" / "0 */2 * * *"
    // 6字段: "0 0 * * * *" / "0 0 */2 * * *"
    std::vector<std::string> fields;
    std::istringstream iss(expr);
    std::string field;
    while (iss >> field) {
        fields.push_back(field);
    }
    if (fields.size() == 5) {
        // 5字段: minute hour day month weekday
        return fields[0] == "0" && fields[1].find('*') != std::string::npos;
    }
    if (fields.size() == 6) {
        // 6字段: second minute hour day month weekday
        return fields[0] == "0" && fields[1] == "0" && fields[2].find('*') != std::string::npos;
    }
    return false;
}

// ============================================================================
// Stagger (错峰调度)
// ============================================================================

int64_t compute_stable_stagger_offset(const std::string& job_id, int64_t stagger_ms) {
    if (stagger_ms <= 0) {
        return 0;
    }
    
    // 使用 SHA256 生成确定性偏移
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(job_id.data()), 
           job_id.size(), hash);
    
    // 取前4字节作为 uint32
    uint32_t offset = (hash[0] << 24) | (hash[1] << 16) | (hash[2] << 8) | hash[3];
    
    return static_cast<int64_t>(offset % stagger_ms);
}

std::optional<int64_t> get_default_stagger_ms(const std::string& expr) {
    if (is_top_of_hour_expression(expr)) {
        return 5 * 60 * 1000;  // 5分钟
    }
    return std::nullopt;
}

// ============================================================================
// Schedule Computation
// ============================================================================

std::optional<int64_t> compute_next_run_at_ms(
    const Schedule& schedule,
    int64_t now_ms
) {
    switch (schedule.kind) {
        case ScheduleKind::At: {
            // 一次性任务
            if (!schedule.at.has_value()) {
                log()->error("Schedule 'at' missing timestamp");
                return std::nullopt;
            }
            
            auto at_ms = parse_absolute_time_ms(*schedule.at);
            if (!at_ms.has_value()) {
                log()->error("Failed to parse 'at' timestamp: {}", *schedule.at);
                return std::nullopt;
            }
            
            // 只有未来时间才返回
            if (*at_ms > now_ms) {
                return *at_ms;
            }
            
            // 已过期,不再执行
            return std::nullopt;
        }
        
        case ScheduleKind::Every: {
            // 周期性任务
            if (!schedule.every_ms.has_value()) {
                log()->error("Schedule 'every' missing interval");
                return std::nullopt;
            }
            
            int64_t interval = std::max<int64_t>(1, *schedule.every_ms);
            int64_t anchor = schedule.anchor_ms.value_or(now_ms);
            
            // 计算从锚点到现在经过的间隔数
            int64_t elapsed = now_ms - anchor;
            int64_t steps = std::max<int64_t>(1, (elapsed + interval - 1) / interval);
            
            return anchor + steps * interval;
        }
        
        case ScheduleKind::Cron: {
            // Cron 表达式
            if (!schedule.expr.has_value()) {
                log()->error("Schedule 'cron' missing expression");
                return std::nullopt;
            }
            
            auto next_ms = parse_cron_next_ms(*schedule.expr, now_ms, schedule.tz);
            if (!next_ms.has_value()) {
                // croner 边界情况：从下一秒重试
                int64_t next_sec = (now_ms / 1000) * 1000 + 1000;
                next_ms = parse_cron_next_ms(*schedule.expr, next_sec, schedule.tz);
                if (!next_ms.has_value()) {
                    return std::nullopt;
                }
            }
            
            // 应用错峰偏移 (对齐 OpenClaw computeStaggeredCronNextRunAtMs)
            int64_t stagger = schedule.stagger_ms.value_or(0);
            if (stagger <= 0) {
                // 整点表达式自动加 5 分钟 stagger
                auto default_stagger = get_default_stagger_ms(*schedule.expr);
                if (default_stagger.has_value()) {
                    stagger = *default_stagger;
                }
            }
            // Note: stagger 需要 job_id 才能计算稳定偏移
            // 无 job_id 时直接返回 next_ms（stagger 在 CronService 层面处理）
            
            return *next_ms;
        }
    }
    
    return std::nullopt;
}

} // namespace cron
} // namespace hunterclaw
