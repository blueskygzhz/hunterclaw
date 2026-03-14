#include "hunterclaw/cron/cron_parser.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>
#include <regex>

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

    // 分割字符串
    std::vector<std::string> split(const std::string& s, char delim) {
        std::vector<std::string> tokens;
        std::istringstream iss(s);
        std::string token;
        while (std::getline(iss, token, delim)) {
            tokens.push_back(token);
        }
        return tokens;
    }

    // 去除字符串首尾空格
    std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }
}

// ============================================================================
// CronExpression Implementation
// ============================================================================

CronExpression::CronExpression(
    const std::string& expr,
    bool use_seconds,
    std::vector<int> seconds,
    std::vector<int> minutes,
    std::vector<int> hours,
    std::vector<int> days,
    std::vector<int> months,
    std::vector<int> weekdays
) : expr_(expr),
    use_seconds_(use_seconds),
    seconds_(std::move(seconds)),
    minutes_(std::move(minutes)),
    hours_(std::move(hours)),
    days_(std::move(days)),
    months_(std::move(months)),
    weekdays_(std::move(weekdays)) {}

std::optional<CronExpression> CronExpression::parse(
    const std::string& expr,
    bool use_seconds
) {
    auto fields = split(trim(expr), ' ');
    
    // 去除空字段
    fields.erase(
        std::remove_if(fields.begin(), fields.end(),
                      [](const std::string& s) { return s.empty(); }),
        fields.end()
    );
    
    size_t expected_fields = use_seconds ? 6 : 5;
    if (fields.size() != expected_fields) {
        log()->error("Invalid cron expression: expected {} fields, got {}",
                    expected_fields, fields.size());
        return std::nullopt;
    }
    
    // 解析各字段
    size_t idx = 0;
    std::vector<int> seconds, minutes, hours, days, months, weekdays;
    
    // Seconds (如果使用6字段格式)
    if (use_seconds) {
        auto s = parse_field(fields[idx++], 0, 59);
        if (!s) return std::nullopt;
        seconds = std::move(*s);
    } else {
        seconds = {0};  // 默认为第0秒
    }
    
    // Minutes
    auto m = parse_field(fields[idx++], 0, 59);
    if (!m) return std::nullopt;
    minutes = std::move(*m);
    
    // Hours
    auto h = parse_field(fields[idx++], 0, 23);
    if (!h) return std::nullopt;
    hours = std::move(*h);
    
    // Days
    auto d = parse_field(fields[idx++], 1, 31);
    if (!d) return std::nullopt;
    days = std::move(*d);
    
    // Months
    auto mo = parse_field(fields[idx++], 1, 12);
    if (!mo) return std::nullopt;
    months = std::move(*mo);
    
    // Weekdays
    auto w = parse_field(fields[idx++], 0, 6);
    if (!w) return std::nullopt;
    weekdays = std::move(*w);
    
    return CronExpression(
        expr, use_seconds,
        std::move(seconds),
        std::move(minutes),
        std::move(hours),
        std::move(days),
        std::move(months),
        std::move(weekdays)
    );
}

std::optional<std::vector<int>> CronExpression::parse_field(
    const std::string& field,
    int min_val,
    int max_val
) {
    std::vector<int> values;
    
    // 通配符 "*" - 所有值
    if (field == "*") {
        for (int i = min_val; i <= max_val; ++i) {
            values.push_back(i);
        }
        return values;
    }
    
    // 步长 "*/N" 或 "start-end/step"
    std::regex step_regex(R"((\*|(\d+)-(\d+))/(\d+))");
    std::smatch match;
    if (std::regex_match(field, match, step_regex)) {
        int start = min_val;
        int end = max_val;
        
        if (match[1].str() != "*") {
            start = std::stoi(match[2].str());
            end = std::stoi(match[3].str());
        }
        
        int step = std::stoi(match[4].str());
        if (step <= 0) {
            log()->error("Invalid step value: {}", step);
            return std::nullopt;
        }
        
        for (int i = start; i <= end; i += step) {
            if (i >= min_val && i <= max_val) {
                values.push_back(i);
            }
        }
        return values;
    }
    
    // 范围 "start-end"
    std::regex range_regex(R"((\d+)-(\d+))");
    if (std::regex_match(field, match, range_regex)) {
        int start = std::stoi(match[1].str());
        int end = std::stoi(match[2].str());
        
        if (start < min_val || end > max_val || start > end) {
            log()->error("Invalid range: {}-{} (allowed: {}-{})",
                        start, end, min_val, max_val);
            return std::nullopt;
        }
        
        for (int i = start; i <= end; ++i) {
            values.push_back(i);
        }
        return values;
    }
    
    // 列表 "1,3,5,7"
    if (field.find(',') != std::string::npos) {
        auto parts = split(field, ',');
        for (const auto& part : parts) {
            auto sub_values = parse_field(trim(part), min_val, max_val);
            if (!sub_values) return std::nullopt;
            values.insert(values.end(), sub_values->begin(), sub_values->end());
        }
        
        // 去重并排序
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        return values;
    }
    
    // 单个数字
    try {
        int val = std::stoi(field);
        if (val < min_val || val > max_val) {
            log()->error("Value {} out of range [{}, {}]", val, min_val, max_val);
            return std::nullopt;
        }
        values.push_back(val);
        return values;
    } catch (const std::exception& e) {
        log()->error("Failed to parse field '{}': {}", field, e.what());
        return std::nullopt;
    }
}

bool CronExpression::matches_field(const std::vector<int>& field, int value) const {
    return std::find(field.begin(), field.end(), value) != field.end();
}

bool CronExpression::matches(std::chrono::system_clock::time_point time) const {
    auto time_t = std::chrono::system_clock::to_time_t(time);
    std::tm tm = *std::localtime(&time_t);
    
    // 计算秒 (如果需要)
    auto since_epoch = time.time_since_epoch();
    auto seconds_total = std::chrono::duration_cast<std::chrono::seconds>(since_epoch).count();
    int second = seconds_total % 60;
    
    return matches_field(seconds_, second) &&
           matches_field(minutes_, tm.tm_min) &&
           matches_field(hours_, tm.tm_hour) &&
           matches_field(days_, tm.tm_mday) &&
           matches_field(months_, tm.tm_mon + 1) &&
           matches_field(weekdays_, tm.tm_wday);
}

void CronExpression::increment_to_next_match(std::tm& tm) const {
    // 增加1秒/分钟 (取决于精度)
    if (use_seconds_) {
        tm.tm_sec++;
    } else {
        tm.tm_min++;
    }
    
    std::mktime(&tm);  // 规范化时间 (处理进位)
}

std::chrono::system_clock::time_point CronExpression::next(
    std::chrono::system_clock::time_point from_time
) const {
    auto time_val = std::chrono::system_clock::to_time_t(from_time);
    std::tm tm = *std::localtime(&time_val);
    
    // 增加时间到下一个单位 (避免匹配当前时间)
    increment_to_next_match(tm);
    
    // 最多尝试4年 (防止无限循环)
    const int max_iterations = 365 * 4 * 24 * 60;
    
    for (int i = 0; i < max_iterations; ++i) {
        // 检查是否匹配
        int second = use_seconds_ ? tm.tm_sec : 0;
        
        bool matches_all =
            matches_field(seconds_, second) &&
            matches_field(minutes_, tm.tm_min) &&
            matches_field(hours_, tm.tm_hour) &&
            matches_field(days_, tm.tm_mday) &&
            matches_field(months_, tm.tm_mon + 1) &&
            matches_field(weekdays_, tm.tm_wday);
        
        if (matches_all) {
            // 转换回 time_point
            time_t result_time = std::mktime(&tm);
            return std::chrono::system_clock::from_time_t(result_time);
        }
        
        // 增加到下一个时间单位
        increment_to_next_match(tm);
    }
    
    // 无法找到匹配 (不应该发生)
    log()->error("Failed to find next match for cron expression: {}", expr_);
    return from_time + std::chrono::hours(1);  // 返回1小时后
}

int64_t CronExpression::next_ms(int64_t from_ms) const {
    auto from_time = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(from_ms)
    );
    
    auto next_time = next(from_time);
    
    auto next_ms_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        next_time.time_since_epoch()
    );
    
    return next_ms_duration.count();
}

// ============================================================================
// 便捷函数
// ============================================================================

std::optional<int64_t> cron_next(
    const std::string& expr,
    int64_t from_ms,
    bool use_seconds
) {
    auto cron = CronExpression::parse(expr, use_seconds);
    if (!cron) {
        return std::nullopt;
    }
    
    return cron->next_ms(from_ms);
}

bool validate_cron_expression(
    const std::string& expr,
    bool use_seconds
) {
    auto cron = CronExpression::parse(expr, use_seconds);
    return cron.has_value();
}

} // namespace cron
} // namespace hunterclaw
