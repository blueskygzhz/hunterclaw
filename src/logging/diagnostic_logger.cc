// 对齐 OC: src/logging/diagnostic.ts (413行) + console.ts (315行) + log-tail.ts (169行)
// 运行时诊断日志 + 控制台格式化 + 日志尾部读取
#include "hunterclaw/logging/subsystem_logger.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <regex>
#include <mutex>
#include <atomic>
#include <map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace logging {

// =========================================================================
// 对齐 OC diagnostic.ts: Webhook Statistics
// =========================================================================

struct WebhookStats {
    std::atomic<int64_t> received{0};
    std::atomic<int64_t> processed{0};
    std::atomic<int64_t> errors{0};
    std::atomic<int64_t> last_received{0};
};

static WebhookStats& webhook_stats() {
    static WebhookStats stats;
    return stats;
}

// 对齐 OC: resolveStuckSessionWarnMs
static const int64_t DEFAULT_STUCK_SESSION_WARN_MS = 120000;
static const int64_t MIN_STUCK_SESSION_WARN_MS = 1000;
static const int64_t MAX_STUCK_SESSION_WARN_MS = 24LL * 60 * 60 * 1000;

int64_t resolve_stuck_session_warn_ms(const json& config) {
    if (!config.contains("diagnostics") || !config["diagnostics"].is_object()) {
        return DEFAULT_STUCK_SESSION_WARN_MS;
    }
    auto& diag = config["diagnostics"];
    if (!diag.contains("stuckSessionWarnMs") || !diag["stuckSessionWarnMs"].is_number()) {
        return DEFAULT_STUCK_SESSION_WARN_MS;
    }
    int64_t raw = static_cast<int64_t>(diag["stuckSessionWarnMs"].get<double>());
    if (raw < MIN_STUCK_SESSION_WARN_MS || raw > MAX_STUCK_SESSION_WARN_MS) {
        return DEFAULT_STUCK_SESSION_WARN_MS;
    }
    return raw;
}

// 对齐 OC: logWebhookReceived
void log_webhook_received(const std::string& channel,
                           const std::string& update_type,
                           const std::string& chat_id) {
    auto& stats = webhook_stats();
    stats.received.fetch_add(1);
    stats.last_received.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// 对齐 OC: getWebhookStats
json get_webhook_stats() {
    auto& stats = webhook_stats();
    return {
        {"received", stats.received.load()},
        {"processed", stats.processed.load()},
        {"errors", stats.errors.load()},
        {"lastReceived", stats.last_received.load()},
    };
}

// =========================================================================
// 对齐 OC diagnostic.ts: Session State Tracking
// =========================================================================

struct DiagnosticSessionState {
    std::string session_key;
    std::string status;  // "idle", "running", "stuck"
    int64_t started_at = 0;
    int64_t last_activity_at = 0;
    std::string current_lane;
    int command_poll_count = 0;
};

static std::mutex session_state_mutex;
static std::map<std::string, DiagnosticSessionState> diagnostic_session_states;

// 对齐 OC: getDiagnosticSessionState
std::optional<DiagnosticSessionState> get_diagnostic_session_state(const std::string& key) {
    std::lock_guard<std::mutex> lock(session_state_mutex);
    auto it = diagnostic_session_states.find(key);
    if (it == diagnostic_session_states.end()) return std::nullopt;
    return it->second;
}

// 对齐 OC: pruneDiagnosticSessionStates
void prune_diagnostic_session_states(int64_t max_age_ms) {
    std::lock_guard<std::mutex> lock(session_state_mutex);
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (auto it = diagnostic_session_states.begin(); it != diagnostic_session_states.end();) {
        if (now - it->second.last_activity_at > max_age_ms) {
            it = diagnostic_session_states.erase(it);
        } else {
            ++it;
        }
    }
}

// =========================================================================
// 对齐 OC console.ts: Console Logger
// =========================================================================

// 对齐 OC console.ts: Console Logger — 使用字符串 level/style 避免与已有 enum 冲突

// 对齐 OC: normalizeConsoleLevel
static std::string normalize_console_level(const std::string& level) {
    if (level == "silent" || level == "error" || level == "warn" ||
        level == "info" || level == "debug" || level == "trace") {
        return level;
    }
    // 对齐 OC: verbose override check
    const char* verbose = std::getenv("HUNTERCLAW_VERBOSE");
    if (verbose && std::string(verbose) == "1") return "debug";
    return "info";
}

// 对齐 OC: normalizeConsoleStyle
static std::string normalize_console_style(const std::string& style) {
    if (style == "compact" || style == "json" || style == "pretty") return style;
    const char* term = std::getenv("TERM");
    if (!term || std::string(term).empty()) return "compact";
    return "pretty";
}

struct ConsoleLoggerSettings {
    std::string level = "info";
    std::string style = "pretty";
};

// 对齐 OC: resolveConsoleLoggerSettings
ConsoleLoggerSettings resolve_console_logger_settings(const json& config) {
    ConsoleLoggerSettings settings;
    if (config.contains("logging") && config["logging"].is_object()) {
        auto& log_cfg = config["logging"];
        if (log_cfg.contains("console") && log_cfg["console"].is_object()) {
            auto& console = log_cfg["console"];
            settings.level = normalize_console_level(console.value("level", "info"));
            settings.style = normalize_console_style(console.value("style", "pretty"));
        }
    }
    const char* verbose = std::getenv("HUNTERCLAW_VERBOSE");
    if (verbose && std::string(verbose) == "1") settings.level = "debug";
    return settings;
}

// =========================================================================
// 对齐 OC log-tail.ts: Log Tail Reader
// =========================================================================

static const int DEFAULT_LOG_TAIL_LIMIT = 500;
static const int DEFAULT_LOG_TAIL_MAX_BYTES = 250000;
static const int MAX_LOG_TAIL_LIMIT = 5000;
static const int MAX_LOG_TAIL_BYTES = 1000000;

struct LogTailPayload {
    std::string file;
    int64_t cursor = 0;
    int64_t size = 0;
    std::vector<std::string> lines;
    bool truncated = false;
    bool reset = false;
};

// 对齐 OC: isRollingLogFile
static bool is_rolling_log_file(const std::string& file) {
    std::regex re(R"(^hunterclaw-\d{4}-\d{2}-\d{2}\.log$)");
    return std::regex_match(fs::path(file).filename().string(), re);
}

// 对齐 OC: resolveLogFile
static std::string resolve_log_file(const std::string& file) {
    if (fs::exists(file)) return file;
    if (!is_rolling_log_file(file)) return file;

    std::string dir = fs::path(file).parent_path().string();
    std::vector<std::pair<std::string, int64_t>> candidates;
    std::regex re(R"(^hunterclaw-\d{4}-\d{2}-\d{2}\.log$)");

    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            if (!std::regex_match(entry.path().filename().string(), re)) continue;
            auto mtime = std::chrono::duration_cast<std::chrono::milliseconds>(
                entry.last_write_time().time_since_epoch()).count();
            candidates.emplace_back(entry.path().string(), mtime);
        }
    } catch (...) {
        return file;
    }

    if (candidates.empty()) return file;

    std::sort(candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    return candidates[0].first;
}

// 对齐 OC: readLogTail
LogTailPayload read_log_tail(const std::string& log_file,
                              int64_t cursor,
                              int limit,
                              int max_bytes) {
    LogTailPayload payload;
    payload.file = resolve_log_file(log_file);

    // Clamp parameters
    if (limit <= 0) limit = DEFAULT_LOG_TAIL_LIMIT;
    if (limit > MAX_LOG_TAIL_LIMIT) limit = MAX_LOG_TAIL_LIMIT;
    if (max_bytes <= 0) max_bytes = DEFAULT_LOG_TAIL_MAX_BYTES;
    if (max_bytes > MAX_LOG_TAIL_BYTES) max_bytes = MAX_LOG_TAIL_BYTES;

    try {
        if (!fs::exists(payload.file)) return payload;

        auto file_size = static_cast<int64_t>(fs::file_size(payload.file));
        payload.size = file_size;

        // 对齐 OC: reset detection — file smaller than cursor
        if (cursor > 0 && cursor > file_size) {
            payload.reset = true;
            cursor = 0;
        }
        payload.cursor = cursor;

        // Read from cursor position
        std::ifstream in(payload.file, std::ios::binary);
        if (!in.is_open()) return payload;

        if (cursor > 0) {
            in.seekg(cursor);
        }

        // Read up to max_bytes
        std::string buffer;
        buffer.resize(static_cast<size_t>(std::min(static_cast<int64_t>(max_bytes),
                                                     file_size - cursor)));
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto bytes_read = in.gcount();
        buffer.resize(static_cast<size_t>(bytes_read));

        payload.truncated = (cursor + bytes_read < file_size);

        // Split into lines
        std::istringstream stream(buffer);
        std::string line;
        int count = 0;
        while (std::getline(stream, line) && count < limit) {
            if (!line.empty()) {
                payload.lines.push_back(line);
                ++count;
            }
        }
        if (count >= limit) {
            payload.truncated = true;
        }

        // Update cursor
        payload.cursor = cursor + bytes_read;
    } catch (...) {
        // Graceful failure
    }

    return payload;
}

// =========================================================================
// 对齐 OC diagnostic.ts: Command Lane Diagnostics
// =========================================================================

struct LaneDiagnostic {
    std::string lane;
    int queue_depth = 0;
    int active_tasks = 0;
    int64_t last_enqueue_at = 0;
    int64_t last_dequeue_at = 0;
};

static std::mutex lane_diag_mutex;
static std::map<std::string, LaneDiagnostic> lane_diagnostics;

// 对齐 OC: logLaneEnqueue
void log_lane_enqueue(const std::string& lane) {
    std::lock_guard<std::mutex> lock(lane_diag_mutex);
    auto& diag = lane_diagnostics[lane];
    diag.lane = lane;
    diag.queue_depth++;
    diag.last_enqueue_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 对齐 OC: logLaneDequeue
void log_lane_dequeue(const std::string& lane) {
    std::lock_guard<std::mutex> lock(lane_diag_mutex);
    auto it = lane_diagnostics.find(lane);
    if (it == lane_diagnostics.end()) return;
    it->second.queue_depth = std::max(0, it->second.queue_depth - 1);
    it->second.last_dequeue_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 对齐 OC: getDiagnosticSnapshot
json get_diagnostic_snapshot() {
    json snapshot;
    snapshot["webhookStats"] = get_webhook_stats();

    // Lane diagnostics
    json lanes = json::object();
    {
        std::lock_guard<std::mutex> lock(lane_diag_mutex);
        for (const auto& [name, diag] : lane_diagnostics) {
            lanes[name] = {
                {"queueDepth", diag.queue_depth},
                {"activeTasks", diag.active_tasks},
                {"lastEnqueueAt", diag.last_enqueue_at},
                {"lastDequeueAt", diag.last_dequeue_at},
            };
        }
    }
    snapshot["lanes"] = lanes;

    // Session diagnostics
    json sessions = json::object();
    {
        std::lock_guard<std::mutex> lock(session_state_mutex);
        for (const auto& [key, state] : diagnostic_session_states) {
            sessions[key] = {
                {"status", state.status},
                {"startedAt", state.started_at},
                {"lastActivityAt", state.last_activity_at},
                {"currentLane", state.current_lane},
            };
        }
    }
    snapshot["sessions"] = sessions;

    return snapshot;
}

}  // namespace logging
}  // namespace hunterclaw
