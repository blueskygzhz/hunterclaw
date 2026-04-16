// 🎯 对齐 OpenClaw: gateway/node-pending-work.ts (完整实现)
// Pending work 队列管理

#include "hunterclaw/nodes/node_pending_work.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <map>
#include <set>

namespace hunterclaw {
namespace nodes {

// ============================================================================
// 内部状态 (对齐 OpenClaw line 18-46)
// ============================================================================

static const std::string DEFAULT_STATUS_ITEM_ID = "baseline-status";
static const int DEFAULT_MAX_ITEMS = 4;
static const int MAX_ITEMS = 10;

static int priority_rank(NodePendingWorkPriority p) {
    switch (p) {
        case NodePendingWorkPriority::High:    return 3;
        case NodePendingWorkPriority::Normal:  return 2;
        case NodePendingWorkPriority::Default: return 1;
    }
    return 1;
}

struct NodePendingWorkState {
    int revision = 0;
    std::map<std::string, NodePendingWorkItem> items_by_id;
};

static std::mutex g_pending_mutex;
static std::map<std::string, NodePendingWorkState> g_state_by_node_id;

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static int g_next_id = 1;
static std::string generate_id() {
    return "pw-" + std::to_string(g_next_id++);
}

// ============================================================================
// pruneExpired (对齐 OpenClaw line 60-72)
// ============================================================================

static bool prune_expired(NodePendingWorkState& state, int64_t now) {
    bool changed = false;
    auto it = state.items_by_id.begin();
    while (it != state.items_by_id.end()) {
        if (it->second.expires_at_ms && *it->second.expires_at_ms <= now) {
            it = state.items_by_id.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) state.revision++;
    return changed;
}

// ============================================================================
// sortedItems (对齐 OpenClaw line 80-91)
// ============================================================================

static std::vector<NodePendingWorkItem> sorted_items(const NodePendingWorkState& state) {
    std::vector<NodePendingWorkItem> items;
    items.reserve(state.items_by_id.size());
    for (const auto& [id, item] : state.items_by_id) {
        items.push_back(item);
    }
    std::sort(items.begin(), items.end(), [](const NodePendingWorkItem& a, const NodePendingWorkItem& b) {
        int pd = priority_rank(b.priority) - priority_rank(a.priority);
        if (pd != 0) return pd < 0;
        if (a.created_at_ms != b.created_at_ms) return a.created_at_ms < b.created_at_ms;
        return a.id < b.id;
    });
    return items;
}

// ============================================================================
// enqueueNodePendingWork (对齐 OpenClaw line 103-135)
// ============================================================================

EnqueueResult enqueue_node_pending_work(
    const std::string& node_id,
    NodePendingWorkType type,
    NodePendingWorkPriority priority,
    std::optional<int> expires_in_ms,
    const std::map<std::string, std::string>& payload) {

    std::lock_guard<std::mutex> lock(g_pending_mutex);

    auto& state = g_state_by_node_id[node_id];
    int64_t now = now_ms();
    prune_expired(state, now);

    // 🎯 对齐 OpenClaw: 按 type 去重
    for (const auto& [id, item] : state.items_by_id) {
        if (item.type == type) {
            return {state.revision, item, true};
        }
    }

    NodePendingWorkItem item;
    item.id = generate_id();
    item.type = type;
    item.priority = priority;
    item.created_at_ms = now;
    if (expires_in_ms) {
        int ms = std::max(1000, *expires_in_ms);
        item.expires_at_ms = now + ms;
    }
    item.payload = payload;

    state.items_by_id[item.id] = item;
    state.revision++;

    return {state.revision, item, false};
}

// ============================================================================
// drainNodePendingWork (对齐 OpenClaw line 137-164)
// ============================================================================

DrainResult drain_node_pending_work(
    const std::string& node_id,
    int max_items,
    bool include_default_status) {

    std::lock_guard<std::mutex> lock(g_pending_mutex);

    auto sit = g_state_by_node_id.find(node_id);
    int revision = sit != g_state_by_node_id.end() ? sit->second.revision : 0;

    if (sit != g_state_by_node_id.end()) {
        prune_expired(sit->second, now_ms());
        if (sit->second.items_by_id.empty()) {
            g_state_by_node_id.erase(sit);
            sit = g_state_by_node_id.end();
        }
    }

    int effective_max = std::min(MAX_ITEMS, std::max(1, max_items));
    auto explicit_items = sit != g_state_by_node_id.end() ? sorted_items(sit->second) : std::vector<NodePendingWorkItem>{};

    std::vector<NodePendingWorkItem> items;
    for (size_t i = 0; i < explicit_items.size() && (int)items.size() < effective_max; ++i) {
        items.push_back(explicit_items[i]);
    }

    // 🎯 对齐 OpenClaw: 如果没有 status.request 且 includeDefaultStatus，添加 baseline
    bool has_explicit_status = false;
    for (const auto& item : explicit_items) {
        if (item.type == NodePendingWorkType::StatusRequest) {
            has_explicit_status = true;
            break;
        }
    }
    bool include_baseline = include_default_status && !has_explicit_status;
    if (include_baseline && (int)items.size() < effective_max) {
        NodePendingWorkItem baseline;
        baseline.id = DEFAULT_STATUS_ITEM_ID;
        baseline.type = NodePendingWorkType::StatusRequest;
        baseline.priority = NodePendingWorkPriority::Default;
        baseline.created_at_ms = now_ms();
        items.push_back(baseline);
    }

    int explicit_returned = 0;
    bool baseline_included = false;
    for (const auto& item : items) {
        if (item.id == DEFAULT_STATUS_ITEM_ID) baseline_included = true;
        else explicit_returned++;
    }

    bool has_more = (int)explicit_items.size() > explicit_returned ||
                    (include_baseline && !baseline_included);

    return {revision, items, has_more};
}

// ============================================================================
// acknowledgeNodePendingWork (对齐 OpenClaw line 166-193)
// ============================================================================

AckResult acknowledge_node_pending_work(
    const std::string& node_id,
    const std::vector<std::string>& item_ids) {

    std::lock_guard<std::mutex> lock(g_pending_mutex);

    auto sit = g_state_by_node_id.find(node_id);
    if (sit == g_state_by_node_id.end()) {
        return {0, {}};
    }

    std::vector<std::string> removed;
    for (const auto& item_id : item_ids) {
        std::string trimmed = item_id;
        while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        if (trimmed.empty() || trimmed == DEFAULT_STATUS_ITEM_ID) continue;
        if (sit->second.items_by_id.erase(trimmed)) {
            removed.push_back(trimmed);
        }
    }
    if (!removed.empty()) {
        sit->second.revision++;
    }
    if (sit->second.items_by_id.empty()) {
        int rev = sit->second.revision;
        g_state_by_node_id.erase(sit);
        return {rev, removed};
    }
    return {sit->second.revision, removed};
}

// ============================================================================
// resetNodePendingWorkForTests (对齐 OpenClaw line 195-197)
// ============================================================================

void reset_node_pending_work_for_tests() {
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    g_state_by_node_id.clear();
}

} // namespace nodes
} // namespace hunterclaw
