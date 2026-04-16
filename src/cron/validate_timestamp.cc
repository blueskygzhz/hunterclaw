// 严格对齐 OpenClaw: src/cron/validate-timestamp.ts (68行)
// at 类型调度的时间戳验证

#include "hunterclaw/cron/validate_timestamp.h"
#include "hunterclaw/cron/schedule.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <sstream>
#include <iomanip>

namespace hunterclaw {
namespace cron {

namespace {

int64_t system_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string format_iso(int64_t ms) {
  time_t secs = ms / 1000;
  std::tm tm = *std::gmtime(&secs);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

}  // namespace

// ============================================================================
// validateScheduleTimestamp (对齐 OpenClaw validateScheduleTimestamp)
// ============================================================================

TimestampValidationResult validate_schedule_timestamp(
    const CronSchedule& schedule, int64_t now_ms) {

  if (schedule.kind != CronSchedule::Kind::AT) {
    return {true, std::nullopt};
  }

  if (now_ms == 0) now_ms = system_now_ms();

  auto at_ms = parse_absolute_time_ms(schedule.at);
  if (!at_ms.has_value()) {
    return {false, "Invalid schedule.at: expected ISO-8601 timestamp (got " + schedule.at + ")"};
  }

  int64_t diff_ms = *at_ms - now_ms;

  // Check if timestamp is in the past (allow 1 minute grace period)
  // 对齐 OpenClaw: ONE_MINUTE_MS
  if (diff_ms < -VALIDATE_ONE_MINUTE_MS) {
    std::string now_date = format_iso(now_ms);
    std::string at_date = format_iso(*at_ms);
    int64_t minutes_ago = (-diff_ms) / VALIDATE_ONE_MINUTE_MS;
    return {false,
            "schedule.at is in the past: " + at_date +
            " (" + std::to_string(minutes_ago) + " minutes ago). Current time: " + now_date};
  }

  // Check if timestamp is too far in the future
  // 对齐 OpenClaw: TEN_YEARS_MS
  if (diff_ms > VALIDATE_TEN_YEARS_MS) {
    std::string at_date = format_iso(*at_ms);
    int64_t years_ahead = diff_ms / (int64_t)(365.25 * 24 * 60 * 60 * 1000);
    return {false,
            "schedule.at is too far in the future: " + at_date +
            " (" + std::to_string(years_ahead) + " years ahead). Maximum allowed: 10 years"};
  }

  return {true, std::nullopt};
}

}  // namespace cron
}  // namespace hunterclaw
