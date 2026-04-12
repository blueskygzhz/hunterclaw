// subagent_list.cc
// Aligned with openclaw:
//   src/agents/subagent-list.ts
//
// Provides:
//   - SubagentListItem
//   - BuiltSubagentList
//   - build_latest_subagent_run_index
//   - create_pending_descendant_counter
//   - is_active_subagent_run
//   - build_subagent_list

#include "hunterclaw/agents/subagent.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <set>
#include <cmath>

namespace hunterclaw {
namespace agents {

namespace {

auto log() {
    static auto logger = spdlog::get("subagent_list");
    if (!logger) logger = spdlog::stdout_logger_mt("subagent_list");
    return logger;
}

// ============================================================================
// 🎯 对齐 OpenClaw: formatDurationCompact (shared/subagents-format.ts)
// ============================================================================

std::string format_duration_compact(int64_t ms) {
    if (ms < 0) return "n/a";
    if (ms < 1000) return std::to_string(ms) + "ms";

    int total_seconds = static_cast<int>(ms / 1000);
    int minutes = total_seconds / 60;
    int seconds = total_seconds % 60;

    if (minutes == 0) {
        return std::to_string(seconds) + "s";
    }
    if (minutes < 60) {
        return std::to_string(minutes) + "m" + std::to_string(seconds) + "s";
    }

    int hours = minutes / 60;
    minutes = minutes % 60;
    return std::to_string(hours) + "h" + std::to_string(minutes) + "m";
}

// ============================================================================
// 🎯 对齐 OpenClaw: truncateLine (shared/subagents-format.ts)
// ============================================================================

std::string truncate_line(const std::string& text, size_t max_chars) {
    if (text.size() <= max_chars) return text;
    if (max_chars <= 3) return text.substr(0, max_chars);
    return text.substr(0, max_chars - 3) + "...";
}

// ============================================================================
// 🎯 对齐 OpenClaw: resolveSubagentLabel (auto-reply/reply/subagents-utils.ts)
// ============================================================================

std::string resolve_subagent_label(const SubagentEntry& entry) {
    if (!entry.label.empty()) return entry.label;
    if (!entry.subagent_name.empty()) return entry.subagent_name;
    return entry.description.empty() ? "subagent" : entry.description;
}

// ============================================================================
// 🎯 对齐 OpenClaw: sortSubagentRuns (auto-reply/reply/subagents-utils.ts)
// Sort by createdAt descending (newest first)
// ============================================================================

std::vector<SubagentEntry> sort_subagent_runs(std::vector<SubagentEntry> runs) {
    std::sort(runs.begin(), runs.end(), [](const SubagentEntry& a, const SubagentEntry& b) {
        return a.spawned_at > b.spawned_at;  // descending
    });
    return runs;
}

// ============================================================================
// 🎯 对齐 OpenClaw: resolveRunStatus(entry, { pendingDescendants })
// ============================================================================

std::string resolve_run_status(const SubagentEntry& entry, int pending_descendants) {
    if (pending_descendants > 0) {
        std::string child_label = (pending_descendants == 1) ? "child" : "children";
        return "active (waiting on " + std::to_string(pending_descendants) + " " + child_label + ")";
    }
    if (entry.completed_at == 0 && entry.state != SubagentState::Completed &&
        entry.state != SubagentState::Failed && entry.state != SubagentState::Killed) {
        return "running";
    }
    if (entry.has_error) return "failed";
    if (entry.state == SubagentState::Killed) return "killed";
    if (entry.state == SubagentState::TimedOut) return "timeout";
    return "done";
}

} // anonymous namespace

// ============================================================================
// 🎯 对齐 OpenClaw: buildLatestSubagentRunIndex(runs)
// ============================================================================

SubagentRunIndex build_latest_subagent_run_index(
    const std::map<std::string, std::shared_ptr<SubagentEntry>>& runs
) {
    SubagentRunIndex index;

    // Build latest by child session key
    for (const auto& [run_id, entry] : runs) {
        std::string child_key = entry->subagent_run_id;
        // Use description or label as child session key proxy
        // In real implementation, this maps to childSessionKey field
        if (child_key.empty()) continue;

        auto existing = index.latest_by_child_session_key.find(child_key);
        if (existing == index.latest_by_child_session_key.end() ||
            entry->spawned_at > existing->second->spawned_at) {
            index.latest_by_child_session_key[child_key] = entry;
        }
    }

    // Build child sessions by controller
    for (const auto& [child_key, entry] : index.latest_by_child_session_key) {
        std::string controller = entry->controller_session_key;
        if (controller.empty()) controller = entry->requester_run_id;
        if (controller.empty()) continue;

        index.child_sessions_by_controller[controller].push_back(child_key);
    }

    // Sort child sessions
    for (auto& [key, sessions] : index.child_sessions_by_controller) {
        std::sort(sessions.begin(), sessions.end());
    }

    return index;
}

// ============================================================================
// 🎯 对齐 OpenClaw: isActiveSubagentRun(entry, pendingDescendantCount)
// ============================================================================

bool is_active_subagent_run(const SubagentEntry& entry, int pending_descendant_count) {
    bool ended = (entry.completed_at > 0 ||
                  entry.state == SubagentState::Completed ||
                  entry.state == SubagentState::Failed ||
                  entry.state == SubagentState::Killed ||
                  entry.state == SubagentState::TimedOut);
    return !ended || pending_descendant_count > 0;
}

// ============================================================================
// 🎯 对齐 OpenClaw: buildSubagentList({ cfg, runs, recentMinutes, taskMaxChars })
// ============================================================================

BuiltSubagentList build_subagent_list(
    const std::vector<SubagentEntry>& runs,
    int recent_minutes,
    int task_max_chars
) {
    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count());
    uint64_t recent_cutoff = now - (recent_minutes * 60000ULL);

    // Dedup by child session key (keep latest)
    auto sorted = sort_subagent_runs(runs);
    std::vector<SubagentEntry> deduped;
    std::set<std::string> seen_keys;
    for (const auto& entry : sorted) {
        std::string key = entry.subagent_run_id;
        if (seen_keys.count(key)) continue;
        seen_keys.insert(key);
        deduped.push_back(entry);
    }

    // Build list items
    BuiltSubagentList result;
    result.total = static_cast<int>(deduped.size());
    int index = 1;

    for (const auto& entry : deduped) {
        // Calculate pending descendants (simplified — uses SubagentRegistry in real impl)
        int pending_descendants = 0;

        bool is_active = is_active_subagent_run(entry, pending_descendants);

        // Calculate runtime
        int64_t runtime_ms = 0;
        if (entry.started_at > 0) {
            uint64_t end_time = (entry.completed_at > 0) ? entry.completed_at : now;
            runtime_ms = static_cast<int64_t>(end_time - entry.started_at);
        } else if (entry.spawned_at > 0) {
            uint64_t end_time = (entry.completed_at > 0) ? entry.completed_at : now;
            runtime_ms = static_cast<int64_t>(end_time - entry.spawned_at);
        }

        std::string status = resolve_run_status(entry, pending_descendants);
        std::string label = truncate_line(resolve_subagent_label(entry), 48);
        std::string task = truncate_line(entry.task.empty() ? entry.description : entry.task,
                                         task_max_chars > 0 ? task_max_chars : 72);
        std::string runtime = format_duration_compact(runtime_ms);

        // Build display line — 对齐 OpenClaw format
        std::string line = std::to_string(index) + ". " + label +
            " (" + runtime + ") " + status;
        if (!task.empty() && task != label) {
            line += " - " + task;
        }

        SubagentListItem item;
        item.index = index;
        item.line = line;
        item.run_id = entry.subagent_run_id;
        item.session_key = entry.subagent_run_id;
        item.label = label;
        item.task = task;
        item.status = status;
        item.pending_descendants = pending_descendants;
        item.runtime = runtime;
        item.runtime_ms = runtime_ms;
        item.started_at = entry.started_at;
        item.ended_at = entry.completed_at;

        if (is_active) {
            result.active.push_back(item);
        } else if (entry.completed_at >= recent_cutoff) {
            result.recent.push_back(item);
        }

        index++;
    }

    // Build text output — 对齐 OpenClaw: buildListText
    std::ostringstream oss;
    oss << "active subagents:\n";
    if (result.active.empty()) {
        oss << "(none)\n";
    } else {
        for (const auto& item : result.active) {
            oss << item.line << "\n";
        }
    }
    oss << "\nrecent (last " << recent_minutes << "m):\n";
    if (result.recent.empty()) {
        oss << "(none)\n";
    } else {
        for (const auto& item : result.recent) {
            oss << item.line << "\n";
        }
    }
    result.text = oss.str();

    return result;
}

} // namespace agents
} // namespace hunterclaw
