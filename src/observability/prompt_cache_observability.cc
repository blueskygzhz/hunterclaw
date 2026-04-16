// ✅ 严格对齐 OpenClaw: src/agents/pi-embedded-runner/prompt-cache-observability.ts (217行)
// LLM 提示词缓存可观测实现

#include "hunterclaw/observability/prompt_cache_observability.h"
#include <openssl/sha.h>
#include <algorithm>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>

namespace hunterclaw {
namespace observability {

// ============================================================================
// Internal State — 严格对齐 OpenClaw
// ============================================================================

struct PromptCacheTracker {
    PromptCacheSnapshot snapshot;
    std::optional<int64_t> last_cache_read;
    std::optional<std::vector<PromptCacheChange>> pending_changes;
};

static std::mutex g_tracker_mutex;
static std::map<std::string, PromptCacheTracker> g_trackers;
static constexpr size_t MAX_TRACKERS = 512;

// ✅ 对齐 OpenClaw: MIN_CACHE_BREAK_TOKEN_DROP = 1_000
static constexpr int64_t MIN_CACHE_BREAK_TOKEN_DROP = 1000;
// ✅ 对齐 OpenClaw: MAX_STABLE_CACHE_READ_RATIO = 0.95
static constexpr double MAX_STABLE_CACHE_READ_RATIO = 0.95;

// ============================================================================
// digestText — 对齐 OpenClaw: digestText (L54-56)
// ============================================================================

static std::string digest_text(const std::string& value) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// ============================================================================
// buildTrackerKey — 对齐 OpenClaw: buildTrackerKey (L58-60)
// ============================================================================

static std::string build_tracker_key(const std::string& session_key,
                                      const std::string& session_id) {
    std::string trimmed = session_key;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    if (start != std::string::npos) {
        trimmed = trimmed.substr(start);
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);
    }
    return trimmed.empty() ? session_id : trimmed;
}

// ============================================================================
// buildToolDigest — 对齐 OpenClaw: buildToolDigest (L62-66)
// ============================================================================

static std::string build_tool_digest(const std::vector<std::string>& tool_names) {
    // ✅ 对齐 OpenClaw: [...toolNames].toSorted() — 排序后序列化
    auto sorted = tool_names;
    std::sort(sorted.begin(), sorted.end());

    std::string joined = "[";
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0) joined += ",";
        joined += "\"" + sorted[i] + "\"";
    }
    joined += "]";
    return digest_text(joined);
}

// ============================================================================
// setTracker — 对齐 OpenClaw: setTracker (L68-78)
// LRU 淘汰策略
// ============================================================================

static void set_tracker(const std::string& key, PromptCacheTracker tracker) {
    // ✅ 对齐 OpenClaw: 如果 key 已存在则先删除（移到末尾模拟 LRU）
    auto it = g_trackers.find(key);
    if (it != g_trackers.end()) {
        g_trackers.erase(it);
    } else if (g_trackers.size() >= MAX_TRACKERS) {
        // ✅ 对齐 OpenClaw: 淘汰最老的 entry
        g_trackers.erase(g_trackers.begin());
    }
    g_trackers[key] = std::move(tracker);
}

// ============================================================================
// diffSnapshots — 严格对齐 OpenClaw: diffSnapshots (L80-130)
// ============================================================================

static std::optional<std::vector<PromptCacheChange>> diff_snapshots(
    const PromptCacheSnapshot& previous,
    const PromptCacheSnapshot& next) {

    std::vector<PromptCacheChange> changes;

    // ✅ 对齐 OpenClaw: model change
    if (previous.provider != next.provider || previous.model_id != next.model_id) {
        changes.push_back({PromptCacheChangeCode::MODEL,
            previous.provider + "/" + previous.model_id + " -> " +
            next.provider + "/" + next.model_id});
    } else if (previous.model_api != next.model_api) {
        std::string prev_api = previous.model_api.empty() ? "unknown" : previous.model_api;
        std::string next_api = next.model_api.empty() ? "unknown" : next.model_api;
        changes.push_back({PromptCacheChangeCode::MODEL,
            prev_api + " -> " + next_api});
    }

    // ✅ 对齐 OpenClaw: cacheRetention change
    if (previous.cache_retention != next.cache_retention) {
        std::string prev_cr = previous.cache_retention.empty() ? "default" : previous.cache_retention;
        std::string next_cr = next.cache_retention.empty() ? "default" : next.cache_retention;
        changes.push_back({PromptCacheChangeCode::CACHE_RETENTION,
            prev_cr + " -> " + next_cr});
    }

    // ✅ 对齐 OpenClaw: transport change
    if (previous.transport != next.transport) {
        std::string prev_t = previous.transport.empty() ? "default" : previous.transport;
        std::string next_t = next.transport.empty() ? "default" : next.transport;
        changes.push_back({PromptCacheChangeCode::TRANSPORT,
            prev_t + " -> " + next_t});
    }

    // ✅ 对齐 OpenClaw: streamStrategy change
    if (previous.stream_strategy != next.stream_strategy) {
        changes.push_back({PromptCacheChangeCode::STREAM_STRATEGY,
            previous.stream_strategy + " -> " + next.stream_strategy});
    }

    // ✅ 对齐 OpenClaw: systemPrompt change
    if (previous.system_prompt_digest != next.system_prompt_digest) {
        changes.push_back({PromptCacheChangeCode::SYSTEM_PROMPT,
            "system prompt digest changed"});
    }

    // ✅ 对齐 OpenClaw: tools change
    if (previous.tool_digest != next.tool_digest) {
        std::string detail = (previous.tool_count == next.tool_count)
            ? "tool set changed with same count"
            : std::to_string(previous.tool_count) + " -> " +
              std::to_string(next.tool_count) + " tools";
        changes.push_back({PromptCacheChangeCode::TOOLS, detail});
    }

    return changes.empty() ? std::nullopt
                           : std::make_optional(std::move(changes));
}

// ============================================================================
// Public API
// ============================================================================

// ✅ 对齐 OpenClaw: collectPromptCacheToolNames (L132-134)
std::vector<std::string> collect_prompt_cache_tool_names(
    const std::vector<std::string>& tool_names) {
    std::vector<std::string> result;
    for (const auto& name : tool_names) {
        std::string trimmed = name;
        size_t start = trimmed.find_first_not_of(" \t");
        if (start != std::string::npos) {
            size_t end = trimmed.find_last_not_of(" \t");
            trimmed = trimmed.substr(start, end - start + 1);
        }
        if (!trimmed.empty()) {
            result.push_back(trimmed);
        }
    }
    return result;
}

// ✅ 对齐 OpenClaw: beginPromptCacheObservation (L136-173)
PromptCacheObservationStart begin_prompt_cache_observation(
    const BeginPromptCacheObservationParams& params) {

    std::lock_guard<std::mutex> lock(g_tracker_mutex);

    std::string key = build_tracker_key(params.session_key, params.session_id);

    PromptCacheSnapshot snapshot;
    snapshot.provider = params.provider;
    snapshot.model_id = params.model_id;
    snapshot.model_api = params.model_api;
    snapshot.cache_retention = params.cache_retention;
    snapshot.stream_strategy = params.stream_strategy;
    snapshot.transport = params.transport;
    snapshot.system_prompt_digest = digest_text(params.system_prompt);
    snapshot.tool_digest = build_tool_digest(params.tool_names);
    snapshot.tool_count = static_cast<int>(params.tool_names.size());
    snapshot.tool_names = params.tool_names;

    auto prev_it = g_trackers.find(key);
    std::optional<std::vector<PromptCacheChange>> changes;
    std::optional<int64_t> previous_cache_read;

    if (prev_it != g_trackers.end()) {
        changes = diff_snapshots(prev_it->second.snapshot, snapshot);
        previous_cache_read = prev_it->second.last_cache_read;
    }

    PromptCacheTracker tracker;
    tracker.snapshot = snapshot;
    tracker.last_cache_read = previous_cache_read;
    tracker.pending_changes = changes;
    set_tracker(key, std::move(tracker));

    return {snapshot, changes, previous_cache_read};
}

// ✅ 对齐 OpenClaw: completePromptCacheObservation (L175-212)
std::optional<PromptCacheBreak> complete_prompt_cache_observation(
    const CompletePromptCacheObservationParams& params) {

    std::lock_guard<std::mutex> lock(g_tracker_mutex);

    std::string key = build_tracker_key(params.session_key, params.session_id);
    auto it = g_trackers.find(key);
    if (it == g_trackers.end()) return std::nullopt;

    auto& tracker = it->second;

    if (!params.cache_read.has_value()) {
        tracker.pending_changes = std::nullopt;
        return std::nullopt;
    }

    int64_t cache_read = *params.cache_read;
    auto previous_cache_read = tracker.last_cache_read;
    tracker.last_cache_read = cache_read;

    if (!previous_cache_read.has_value() || *previous_cache_read <= 0) {
        tracker.pending_changes = std::nullopt;
        return std::nullopt;
    }

    // ✅ 对齐 OpenClaw: meaningful drop detection
    int64_t token_drop = *previous_cache_read - cache_read;
    bool has_meaningful_drop =
        cache_read < static_cast<int64_t>(*previous_cache_read * MAX_STABLE_CACHE_READ_RATIO) &&
        token_drop >= MIN_CACHE_BREAK_TOKEN_DROP;

    std::optional<PromptCacheBreak> result;
    if (has_meaningful_drop) {
        result = PromptCacheBreak{
            *previous_cache_read,
            cache_read,
            tracker.pending_changes
        };
    }

    tracker.pending_changes = std::nullopt;
    return result;
}

// ✅ 对齐 OpenClaw: resetPromptCacheObservabilityForTest (L214-216)
void reset_prompt_cache_observability_for_test() {
    std::lock_guard<std::mutex> lock(g_tracker_mutex);
    g_trackers.clear();
}

}  // namespace observability
}  // namespace hunterclaw
