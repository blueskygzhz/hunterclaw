// 严格对齐 OpenClaw: src/cron/run-log.ts (463行)
// JSONL 运行日志系统 — 追加/读取/分页/修剪

#include "hunterclaw/cron/run_log.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace cron {

namespace {

// 对齐 OpenClaw: DEFAULT_CRON_RUN_LOG_MAX_BYTES / KEEP_LINES
constexpr int64_t DEFAULT_MAX_BYTES = 2'000'000;
constexpr int DEFAULT_KEEP_LINES = 2'000;

// 对齐 OpenClaw: writesByPath (serialization mutex per path)
std::mutex g_writes_mutex;

// ── assertSafeCronRunLogJobId (对齐 OpenClaw) ──────────────────────────────

std::string assert_safe_job_id(const std::string& job_id) {
  std::string trimmed = job_id;
  auto start = trimmed.find_first_not_of(" \t\r\n");
  auto end = trimmed.find_last_not_of(" \t\r\n");
  if (start == std::string::npos) throw std::runtime_error("invalid cron run log job id");
  trimmed = trimmed.substr(start, end - start + 1);
  if (trimmed.empty()) throw std::runtime_error("invalid cron run log job id");
  if (trimmed.find('/') != std::string::npos ||
      trimmed.find('\\') != std::string::npos ||
      trimmed.find('\0') != std::string::npos) {
    throw std::runtime_error("invalid cron run log job id");
  }
  return trimmed;
}

// ── setSecureFileMode (对齐 OpenClaw) ───────────────────────────────────────

void set_secure_file_mode(const std::string& path) {
  try { fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write); }
  catch (...) {}
}

// ── Serialize entry to JSON line ────────────────────────────────────────────

std::string entry_to_json_line(const CronRunLogEntryRecord& entry) {
  json j;
  j["ts"] = entry.ts;
  j["jobId"] = entry.job_id;
  j["action"] = entry.action;
  if (entry.status) j["status"] = *entry.status;
  if (entry.error) j["error"] = *entry.error;
  if (entry.summary) j["summary"] = *entry.summary;
  if (entry.delivered.has_value()) j["delivered"] = *entry.delivered;
  if (entry.delivery_status) j["deliveryStatus"] = *entry.delivery_status;
  if (entry.delivery_error) j["deliveryError"] = *entry.delivery_error;
  if (entry.session_id) j["sessionId"] = *entry.session_id;
  if (entry.session_key) j["sessionKey"] = *entry.session_key;
  if (entry.run_at_ms) j["runAtMs"] = *entry.run_at_ms;
  if (entry.duration_ms) j["durationMs"] = *entry.duration_ms;
  if (entry.next_run_at_ms) j["nextRunAtMs"] = *entry.next_run_at_ms;
  if (entry.model) j["model"] = *entry.model;
  if (entry.provider) j["provider"] = *entry.provider;
  if (entry.usage) {
    json usage;
    if (entry.usage->input_tokens) usage["input_tokens"] = *entry.usage->input_tokens;
    if (entry.usage->output_tokens) usage["output_tokens"] = *entry.usage->output_tokens;
    if (entry.usage->total_tokens) usage["total_tokens"] = *entry.usage->total_tokens;
    if (entry.usage->cache_read_tokens) usage["cache_read_tokens"] = *entry.usage->cache_read_tokens;
    if (entry.usage->cache_write_tokens) usage["cache_write_tokens"] = *entry.usage->cache_write_tokens;
    j["usage"] = usage;
  }
  return j.dump();
}

// ── Parse JSON line to entry ────────────────────────────────────────────────

std::optional<CronRunLogEntryRecord> parse_json_line(const std::string& line,
                                                      const std::optional<std::string>& filter_job_id) {
  if (line.empty()) return std::nullopt;
  try {
    auto j = json::parse(line);
    if (!j.is_object()) return std::nullopt;
    if (j.value("action", "") != "finished") return std::nullopt;
    std::string job_id = j.value("jobId", "");
    if (job_id.empty()) return std::nullopt;
    if (!j.contains("ts") || !j["ts"].is_number()) return std::nullopt;
    if (filter_job_id && job_id != *filter_job_id) return std::nullopt;

    CronRunLogEntryRecord entry;
    entry.ts = j["ts"].get<int64_t>();
    entry.job_id = job_id;
    entry.action = "finished";
    if (j.contains("status") && j["status"].is_string()) entry.status = j["status"].get<std::string>();
    if (j.contains("error") && j["error"].is_string()) entry.error = j["error"].get<std::string>();
    if (j.contains("summary") && j["summary"].is_string()) entry.summary = j["summary"].get<std::string>();
    if (j.contains("delivered") && j["delivered"].is_boolean()) entry.delivered = j["delivered"].get<bool>();
    if (j.contains("deliveryStatus") && j["deliveryStatus"].is_string()) entry.delivery_status = j["deliveryStatus"].get<std::string>();
    if (j.contains("deliveryError") && j["deliveryError"].is_string()) entry.delivery_error = j["deliveryError"].get<std::string>();
    if (j.contains("sessionId") && j["sessionId"].is_string()) entry.session_id = j["sessionId"].get<std::string>();
    if (j.contains("sessionKey") && j["sessionKey"].is_string()) entry.session_key = j["sessionKey"].get<std::string>();
    if (j.contains("runAtMs") && j["runAtMs"].is_number()) entry.run_at_ms = j["runAtMs"].get<int64_t>();
    if (j.contains("durationMs") && j["durationMs"].is_number()) entry.duration_ms = j["durationMs"].get<int64_t>();
    if (j.contains("nextRunAtMs") && j["nextRunAtMs"].is_number()) entry.next_run_at_ms = j["nextRunAtMs"].get<int64_t>();
    if (j.contains("model") && j["model"].is_string()) entry.model = j["model"].get<std::string>();
    if (j.contains("provider") && j["provider"].is_string()) entry.provider = j["provider"].get<std::string>();
    if (j.contains("usage") && j["usage"].is_object()) {
      CronRunLogEntryRecord::Usage u;
      auto& uj = j["usage"];
      if (uj.contains("input_tokens") && uj["input_tokens"].is_number()) u.input_tokens = uj["input_tokens"].get<int64_t>();
      if (uj.contains("output_tokens") && uj["output_tokens"].is_number()) u.output_tokens = uj["output_tokens"].get<int64_t>();
      if (uj.contains("total_tokens") && uj["total_tokens"].is_number()) u.total_tokens = uj["total_tokens"].get<int64_t>();
      if (uj.contains("cache_read_tokens") && uj["cache_read_tokens"].is_number()) u.cache_read_tokens = uj["cache_read_tokens"].get<int64_t>();
      if (uj.contains("cache_write_tokens") && uj["cache_write_tokens"].is_number()) u.cache_write_tokens = uj["cache_write_tokens"].get<int64_t>();
      entry.usage = u;
    }
    return entry;
  } catch (...) {
    return std::nullopt;
  }
}

// ── pruneIfNeeded (对齐 OpenClaw) ───────────────────────────────────────────

void prune_if_needed(const std::string& file_path, const CronRunLogPruneOptions& opts) {
  try {
    auto size = fs::file_size(file_path);
    if ((int64_t)size <= opts.max_bytes) return;

    std::ifstream in(file_path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
      auto trimmed = line;
      auto s = trimmed.find_first_not_of(" \t\r\n");
      if (s == std::string::npos) continue;
      trimmed = trimmed.substr(s);
      if (!trimmed.empty()) lines.push_back(trimmed);
    }
    in.close();

    int start = std::max(0, (int)lines.size() - opts.keep_lines);
    std::string tmp = file_path + ".prune.tmp";
    std::ofstream out(tmp);
    for (int i = start; i < (int)lines.size(); ++i) {
      out << lines[i] << "\n";
    }
    out.close();
    set_secure_file_mode(tmp);
    fs::rename(tmp, file_path);
    set_secure_file_mode(file_path);
  } catch (...) {}
}

// ── filter entries ──────────────────────────────────────────────────────────

std::vector<CronRunLogEntryRecord> filter_entries(
    const std::vector<CronRunLogEntryRecord>& entries,
    const ReadCronRunLogPageOptions& opts) {
  std::vector<CronRunLogEntryRecord> result;
  for (const auto& entry : entries) {
    // Status filter
    if (!opts.statuses.empty()) {
      if (!entry.status) continue;
      bool match = false;
      for (const auto& s : opts.statuses) { if (s == *entry.status) { match = true; break; } }
      if (!match) continue;
    }
    // Delivery status filter
    if (!opts.delivery_statuses.empty()) {
      std::string ds = entry.delivery_status.value_or("not-requested");
      bool match = false;
      for (const auto& s : opts.delivery_statuses) { if (s == ds) { match = true; break; } }
      if (!match) continue;
    }
    // Query filter
    if (opts.query && !opts.query->empty()) {
      std::string haystack;
      haystack += entry.summary.value_or("");
      haystack += " ";
      haystack += entry.error.value_or("");
      haystack += " ";
      haystack += entry.job_id;
      if (entry.job_name) haystack += " " + *entry.job_name;
      std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);
      std::string query = *opts.query;
      std::transform(query.begin(), query.end(), query.begin(), ::tolower);
      if (haystack.find(query) == std::string::npos) continue;
    }
    result.push_back(entry);
  }
  return result;
}

}  // namespace

// ============================================================================
// resolveCronRunLogPath (对齐 OpenClaw)
// ============================================================================

std::string resolve_cron_run_log_path(const std::string& store_path,
                                       const std::string& job_id) {
  std::string safe_id = assert_safe_job_id(job_id);
  fs::path store = fs::path(store_path).lexically_normal();
  fs::path dir = store.parent_path();
  fs::path runs_dir = dir / "runs";
  fs::path resolved = runs_dir / (safe_id + ".jsonl");
  // Path traversal check
  std::string resolved_str = resolved.string();
  std::string runs_prefix = runs_dir.string() + std::string(1, fs::path::preferred_separator);
  if (resolved_str.rfind(runs_prefix, 0) != 0) {
    throw std::runtime_error("invalid cron run log job id");
  }
  return resolved_str;
}

// ============================================================================
// appendCronRunLog (对齐 OpenClaw)
// ============================================================================

void append_cron_run_log(const std::string& file_path,
                          const CronRunLogEntryRecord& entry,
                          const CronRunLogPruneOptions& prune_opts) {
  std::lock_guard<std::mutex> lock(g_writes_mutex);
  try {
    fs::path dir = fs::path(file_path).parent_path();
    fs::create_directories(dir);
    try { fs::permissions(dir, fs::perms::owner_all); } catch (...) {}

    std::ofstream out(file_path, std::ios::app);
    out << entry_to_json_line(entry) << "\n";
    out.close();
    set_secure_file_mode(file_path);

    prune_if_needed(file_path, prune_opts);
  } catch (...) {}
}

// ============================================================================
// readCronRunLogEntries (对齐 OpenClaw)
// ============================================================================

std::vector<CronRunLogEntryRecord> read_cron_run_log_entries(
    const std::string& file_path, int limit,
    const std::optional<std::string>& job_id) {
  int clamped_limit = std::max(1, std::min(5000, limit));
  ReadCronRunLogPageOptions opts;
  opts.limit = clamped_limit;
  opts.offset = 0;
  opts.job_id = job_id;
  opts.sort_dir = CronRunLogSortDir::DESC;
  auto page = read_cron_run_log_entries_page(file_path, opts);
  std::reverse(page.entries.begin(), page.entries.end());
  return page.entries;
}

// ============================================================================
// readCronRunLogEntriesPage (对齐 OpenClaw)
// ============================================================================

CronRunLogPageResult read_cron_run_log_entries_page(
    const std::string& file_path,
    const ReadCronRunLogPageOptions& opts) {
  int limit = std::max(1, std::min(200, opts.limit));

  // Parse all entries from file
  std::vector<CronRunLogEntryRecord> all;
  try {
    std::ifstream in(file_path);
    std::string line;
    while (std::getline(in, line)) {
      auto entry = parse_json_line(line, opts.job_id);
      if (entry) all.push_back(*entry);
    }
  } catch (...) {}

  // Filter
  auto filtered = filter_entries(all, opts);

  // Sort
  if (opts.sort_dir == CronRunLogSortDir::ASC) {
    std::sort(filtered.begin(), filtered.end(),
              [](const auto& a, const auto& b) { return a.ts < b.ts; });
  } else {
    std::sort(filtered.begin(), filtered.end(),
              [](const auto& a, const auto& b) { return a.ts > b.ts; });
  }

  int total = (int)filtered.size();
  int offset = std::max(0, std::min(total, opts.offset));
  int end = std::min(total, offset + limit);

  CronRunLogPageResult result;
  result.entries.assign(filtered.begin() + offset, filtered.begin() + end);
  result.total = total;
  result.offset = offset;
  result.limit = limit;
  result.has_more = end < total;
  result.next_offset = (end < total) ? std::optional<int>(end) : std::nullopt;
  return result;
}

// ============================================================================
// readCronRunLogEntriesPageAll (对齐 OpenClaw)
// ============================================================================

CronRunLogPageResult read_cron_run_log_entries_page_all(
    const std::string& store_path,
    const ReadCronRunLogPageOptions& opts,
    const std::unordered_map<std::string, std::string>* job_name_by_id) {
  int limit = std::max(1, std::min(200, opts.limit));
  fs::path runs_dir = fs::path(store_path).parent_path() / "runs";

  std::vector<CronRunLogEntryRecord> all;
  try {
    if (fs::exists(runs_dir)) {
      for (const auto& entry : fs::directory_iterator(runs_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".jsonl") continue;
        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line)) {
          auto parsed = parse_json_line(line, std::nullopt);
          if (parsed) all.push_back(*parsed);
        }
      }
    }
  } catch (...) {}

  auto filtered = filter_entries(all, opts);

  if (opts.sort_dir == CronRunLogSortDir::ASC) {
    std::sort(filtered.begin(), filtered.end(),
              [](const auto& a, const auto& b) { return a.ts < b.ts; });
  } else {
    std::sort(filtered.begin(), filtered.end(),
              [](const auto& a, const auto& b) { return a.ts > b.ts; });
  }

  int total = (int)filtered.size();
  int offset = std::max(0, std::min(total, opts.offset));
  int end = std::min(total, offset + limit);

  CronRunLogPageResult result;
  for (int i = offset; i < end; ++i) {
    auto& e = filtered[i];
    if (job_name_by_id) {
      auto it = job_name_by_id->find(e.job_id);
      if (it != job_name_by_id->end()) e.job_name = it->second;
    }
    result.entries.push_back(e);
  }
  result.total = total;
  result.offset = offset;
  result.limit = limit;
  result.has_more = end < total;
  result.next_offset = (end < total) ? std::optional<int>(end) : std::nullopt;
  return result;
}

// ============================================================================
// resolveCronRunLogPruneOptions (对齐 OpenClaw)
// ============================================================================

CronRunLogPruneOptions resolve_cron_run_log_prune_options(
    std::optional<int64_t> max_bytes, std::optional<int> keep_lines) {
  return {
    max_bytes.value_or(DEFAULT_MAX_BYTES),
    keep_lines.value_or(DEFAULT_KEEP_LINES)
  };
}

}  // namespace cron
}  // namespace hunterclaw
