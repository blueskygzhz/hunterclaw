#pragma once

// 严格对齐 OpenClaw: src/cron/run-log.ts (463行)
// Cron 运行日志系统 — JSONL 格式的追加/读取/分页/修剪

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace cron {

// ============================================================================
// CronRunLogEntry (对齐 OpenClaw CronRunLogEntry)
// ============================================================================

struct CronRunLogEntryRecord {
  int64_t ts = 0;
  std::string job_id;
  std::string action = "finished";
  std::optional<std::string> status;  // "ok" | "error" | "skipped"
  std::optional<std::string> error;
  std::optional<std::string> summary;
  std::optional<bool> delivered;
  std::optional<std::string> delivery_status;
  std::optional<std::string> delivery_error;
  std::optional<std::string> session_id;
  std::optional<std::string> session_key;
  std::optional<int64_t> run_at_ms;
  std::optional<int64_t> duration_ms;
  std::optional<int64_t> next_run_at_ms;
  std::optional<std::string> model;
  std::optional<std::string> provider;
  // Usage (对齐 OpenClaw CronRunTelemetry)
  struct Usage {
    std::optional<int64_t> input_tokens;
    std::optional<int64_t> output_tokens;
    std::optional<int64_t> total_tokens;
    std::optional<int64_t> cache_read_tokens;
    std::optional<int64_t> cache_write_tokens;
  };
  std::optional<Usage> usage;
  // Extra field for "all" scope (对齐 OpenClaw jobName enrichment)
  std::optional<std::string> job_name;
};

// ============================================================================
// Read options / result types
// ============================================================================

// 对齐 OpenClaw CronRunLogSortDir
enum class CronRunLogSortDir { ASC, DESC };

// 对齐 OpenClaw CronRunLogStatusFilter
enum class CronRunLogStatusFilter { ALL, OK, ERROR, SKIPPED };

// 对齐 OpenClaw ReadCronRunLogPageOptions
struct ReadCronRunLogPageOptions {
  int limit = 50;
  int offset = 0;
  std::optional<std::string> job_id;
  CronRunLogStatusFilter status = CronRunLogStatusFilter::ALL;
  std::vector<std::string> statuses;
  std::vector<std::string> delivery_statuses;
  std::optional<std::string> query;
  CronRunLogSortDir sort_dir = CronRunLogSortDir::DESC;
};

// 对齐 OpenClaw CronRunLogPageResult
struct CronRunLogPageResult {
  std::vector<CronRunLogEntryRecord> entries;
  int total = 0;
  int offset = 0;
  int limit = 50;
  bool has_more = false;
  std::optional<int> next_offset;
};

// 对齐 OpenClaw resolveCronRunLogPruneOptions
struct CronRunLogPruneOptions {
  int64_t max_bytes = 2'000'000;  // DEFAULT_CRON_RUN_LOG_MAX_BYTES
  int keep_lines = 2'000;         // DEFAULT_CRON_RUN_LOG_KEEP_LINES
};

// ============================================================================
// Public API
// ============================================================================

/**
 * 解析运行日志文件路径
 * 对齐 OpenClaw: resolveCronRunLogPath({ storePath, jobId })
 */
std::string resolve_cron_run_log_path(const std::string& store_path,
                                       const std::string& job_id);

/**
 * 追加运行日志条目
 * 对齐 OpenClaw: appendCronRunLog(filePath, entry, opts)
 */
void append_cron_run_log(const std::string& file_path,
                          const CronRunLogEntryRecord& entry,
                          const CronRunLogPruneOptions& prune_opts = {});

/**
 * 读取运行日志条目 (简单接口)
 * 对齐 OpenClaw: readCronRunLogEntries(filePath, opts)
 */
std::vector<CronRunLogEntryRecord> read_cron_run_log_entries(
    const std::string& file_path,
    int limit = 200,
    const std::optional<std::string>& job_id = std::nullopt);

/**
 * 分页读取运行日志
 * 对齐 OpenClaw: readCronRunLogEntriesPage(filePath, opts)
 */
CronRunLogPageResult read_cron_run_log_entries_page(
    const std::string& file_path,
    const ReadCronRunLogPageOptions& opts = {});

/**
 * 跨 job 分页读取所有运行日志
 * 对齐 OpenClaw: readCronRunLogEntriesPageAll(opts)
 */
CronRunLogPageResult read_cron_run_log_entries_page_all(
    const std::string& store_path,
    const ReadCronRunLogPageOptions& opts = {},
    const std::unordered_map<std::string, std::string>* job_name_by_id = nullptr);

/**
 * 解析修剪选项
 * 对齐 OpenClaw: resolveCronRunLogPruneOptions(cfg)
 */
CronRunLogPruneOptions resolve_cron_run_log_prune_options(
    std::optional<int64_t> max_bytes = std::nullopt,
    std::optional<int> keep_lines = std::nullopt);

}  // namespace cron
}  // namespace hunterclaw
