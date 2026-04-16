#pragma once

// 严格对齐 OpenClaw: src/cron/validate-timestamp.ts (68行)
// 验证 at 类型调度的时间戳（不能太旧/太远）

#include "hunterclaw/cron/service.h"
#include <cstdint>
#include <optional>
#include <string>

namespace hunterclaw {
namespace cron {

// 对齐 OpenClaw: ONE_MINUTE_MS, TEN_YEARS_MS
constexpr int64_t VALIDATE_ONE_MINUTE_MS = 60 * 1000;
constexpr int64_t VALIDATE_TEN_YEARS_MS = static_cast<int64_t>(10 * 365.25 * 24 * 60 * 60 * 1000);

/**
 * TimestampValidationResult (对齐 OpenClaw)
 */
struct TimestampValidationResult {
  bool ok = true;
  std::optional<std::string> message;
};

/**
 * 验证调度时间戳
 * 对齐 OpenClaw: validateScheduleTimestamp(schedule, nowMs)
 *
 * Rejects timestamps that are:
 * - More than 1 minute in the past
 * - More than 10 years in the future
 *
 * @param schedule  CronSchedule to validate
 * @param now_ms    Current time in milliseconds (default: system clock)
 * @return validation result
 */
TimestampValidationResult validate_schedule_timestamp(
    const CronSchedule& schedule,
    int64_t now_ms = 0);

}  // namespace cron
}  // namespace hunterclaw
