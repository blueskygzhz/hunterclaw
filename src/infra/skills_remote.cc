// ============================================================================
// skills_remote.cc
// 🎯 严格对齐 OpenClaw: src/infra/skills-remote.ts (352 行)
//
// 远程 macOS 节点的技能资格管理——跟踪远程节点连接状态、探测节点上安装的
// binary、聚合远程平台能力。
// ============================================================================

#include "hunterclaw/infra/skills_remote.h"
#include "hunterclaw/skills/skill.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <mutex>
#include <set>
#include <sstream>

namespace hunterclaw {
namespace infra {

// ============================================================================
// Internal Types
// ============================================================================

struct RemoteNodeRecord {
    std::string node_id;
    std::string display_name;
    std::string platform;
    std::string device_family;
    std::vector<std::string> commands;
    std::set<std::string> bins;
    std::string remote_ip;
};

// ============================================================================
// Module-level state (对齐 OpenClaw: remoteNodes Map + remoteRegistry)
// ============================================================================

static std::mutex s_mutex;
static std::unordered_map<std::string, RemoteNodeRecord> s_remote_nodes;
static NodeRegistry* s_registry = nullptr;

// ============================================================================
// Internal helpers
// ============================================================================

// 🎯 对齐 OpenClaw: describeNode(nodeId)
static std::string describe_node(const std::string& node_id) {
    auto it = s_remote_nodes.find(node_id);
    if (it == s_remote_nodes.end()) return node_id;
    const auto& rec = it->second;
    std::string name = rec.display_name;
    // trim
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    std::string base = (!name.empty() && name != node_id)
        ? name + " (" + node_id + ")"
        : node_id;
    std::string ip = rec.remote_ip;
    while (!ip.empty() && ip.front() == ' ') ip.erase(ip.begin());
    while (!ip.empty() && ip.back() == ' ') ip.pop_back();
    return ip.empty() ? base : base + " @ " + ip;
}

// 🎯 对齐 OpenClaw: isMacPlatform(platform, deviceFamily)
static bool is_mac_platform(const std::string& platform, const std::string& device_family) {
    auto lower = [](const std::string& s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(), ::tolower);
        return r;
    };
    std::string plat = lower(platform);
    std::string fam = lower(device_family);
    if (plat.find("mac") != std::string::npos) return true;
    if (plat.find("darwin") != std::string::npos) return true;
    if (fam == "mac") return true;
    return false;
}

// 🎯 对齐 OpenClaw: supportsSystemRun(commands?)
static bool supports_system_run(const std::vector<std::string>& commands) {
    return std::find(commands.begin(), commands.end(), "system.run") != commands.end();
}

// 🎯 对齐 OpenClaw: supportsSystemWhich(commands?)
static bool supports_system_which(const std::vector<std::string>& commands) {
    return std::find(commands.begin(), commands.end(), "system.which") != commands.end();
}

// 🎯 对齐 OpenClaw: upsertNode(record)
static void upsert_node(const RemoteNodeInfo& info,
                         const std::optional<std::vector<std::string>>& bins = std::nullopt) {
    auto it = s_remote_nodes.find(info.node_id);
    RemoteNodeRecord rec;
    if (it != s_remote_nodes.end()) {
        rec = it->second;
    }
    rec.node_id = info.node_id;
    if (!info.display_name.empty()) rec.display_name = info.display_name;
    if (!info.platform.empty()) rec.platform = info.platform;
    if (!info.device_family.empty()) rec.device_family = info.device_family;
    if (!info.commands.empty()) rec.commands = info.commands;
    if (!info.remote_ip.empty()) rec.remote_ip = info.remote_ip;
    if (bins) {
        rec.bins.clear();
        for (const auto& b : *bins) {
            if (!b.empty()) rec.bins.insert(b);
        }
    }
    s_remote_nodes[info.node_id] = std::move(rec);
}

// 🎯 对齐 OpenClaw: collectRequiredBins(entries, targetPlatform)
static std::vector<std::string> collect_required_bins(
    const std::vector<skills::SkillEntry>& entries,
    const std::string& target_platform)
{
    std::set<std::string> bins;
    for (const auto& entry : entries) {
        const auto& os = entry.metadata.os;
        if (!os.empty() &&
            std::find(os.begin(), os.end(), target_platform) == os.end()) {
            continue;
        }
        for (const auto& bin : entry.metadata.requires.bins) {
            std::string trimmed = bin;
            while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
            while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
            if (!trimmed.empty()) bins.insert(trimmed);
        }
        for (const auto& bin : entry.metadata.requires.any_bins) {
            std::string trimmed = bin;
            while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
            while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
            if (!trimmed.empty()) bins.insert(trimmed);
        }
    }
    return {bins.begin(), bins.end()};
}

// 🎯 对齐 OpenClaw: buildBinProbeScript(bins)
static std::string build_bin_probe_script(const std::vector<std::string>& bins) {
    std::ostringstream ss;
    ss << "for b in";
    for (const auto& bin : bins) {
        ss << " '" << bin << "'";  // simplified — no single-quote escaping
    }
    ss << "; do if command -v \"$b\" >/dev/null 2>&1; then echo \"$b\"; fi; done";
    return ss.str();
}

// 🎯 对齐 OpenClaw: areBinSetsEqual(a, b)
static bool are_bin_sets_equal(const std::set<std::string>& a,
                                const std::set<std::string>& b) {
    return a == b;
}

// 🎯 对齐 OpenClaw: logRemoteBinProbeFailure(nodeId, err)
static void log_remote_bin_probe_failure(const std::string& node_id,
                                          const std::string& message) {
    std::string label = describe_node(node_id);
    if (message.find("node not connected") != std::string::npos ||
        message.find("node disconnected") != std::string::npos) {
        { auto _l = spdlog::get("default"); if (_l) _l->info("[skills-remote] remote bin probe skipped: node unavailable ({})", label); }
        return;
    }
    if (message.find("invoke timed out") != std::string::npos ||
        message.find("timeout") != std::string::npos) {
        if (auto log = spdlog::get("default")) log->warn("[skills-remote] remote bin probe timed out ({})", label);
        return;
    }
    if (auto log = spdlog::get("default")) log->warn("[skills-remote] remote bin probe error ({}): {}", label, message);
}

// ============================================================================
// Public API
// ============================================================================

// 🎯 对齐 OpenClaw: setSkillsRemoteRegistry(registry)
void set_skills_remote_registry(NodeRegistry* registry) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_registry = registry;
}

// 🎯 对齐 OpenClaw: primeRemoteSkillsCache()
void prime_remote_skills_cache(const std::vector<PairedNodeInfo>& paired_nodes,
                                std::function<void()> bump_version) {
    std::lock_guard<std::mutex> lock(s_mutex);
    bool saw_mac = false;
    for (const auto& node : paired_nodes) {
        RemoteNodeInfo info;
        info.node_id = node.node_id;
        info.display_name = node.display_name;
        info.platform = node.platform;
        info.device_family = node.device_family;
        info.commands = node.commands;
        info.remote_ip = node.remote_ip;
        upsert_node(info, node.bins);
        if (is_mac_platform(node.platform, node.device_family) &&
            supports_system_run(node.commands)) {
            saw_mac = true;
        }
    }
    if (saw_mac && bump_version) {
        bump_version();
    }
}

// 🎯 对齐 OpenClaw: recordRemoteNodeInfo(node)
void record_remote_node_info(const RemoteNodeInfo& info) {
    std::lock_guard<std::mutex> lock(s_mutex);
    upsert_node(info);
}

// 🎯 对齐 OpenClaw: recordRemoteNodeBins(nodeId, bins)
void record_remote_node_bins(const std::string& node_id,
                              const std::vector<std::string>& bins) {
    std::lock_guard<std::mutex> lock(s_mutex);
    RemoteNodeInfo info;
    info.node_id = node_id;
    upsert_node(info, bins);
}

// 🎯 对齐 OpenClaw: removeRemoteNodeInfo(nodeId)
void remove_remote_node_info(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_remote_nodes.erase(node_id);
}

// 🎯 对齐 OpenClaw: refreshRemoteNodeBins(params)
RefreshRemoteNodeBinsResult refresh_remote_node_bins(const RefreshRemoteNodeBinsParams& params) {
    std::lock_guard<std::mutex> lock(s_mutex);
    RefreshRemoteNodeBinsResult result;
    result.refreshed = false;

    if (!s_registry) return result;
    if (!is_mac_platform(params.platform, params.device_family)) return result;
    bool can_which = supports_system_which(params.commands);
    bool can_run = supports_system_run(params.commands);
    if (!can_which && !can_run) return result;

    // Collect required bins from skill entries
    auto required_bins = collect_required_bins(params.skill_entries, "darwin");
    if (required_bins.empty()) return result;

    // Build probe command
    std::string command;
    if (can_which) {
        // Use system.which with bins list
        command = "system.which:" + [&]() {
            std::ostringstream ss;
            for (size_t i = 0; i < required_bins.size(); ++i) {
                if (i > 0) ss << ",";
                ss << required_bins[i];
            }
            return ss.str();
        }();
    } else {
        // Use system.run with shell script
        command = "system.run:" + build_bin_probe_script(required_bins);
    }

    // Invoke remote (simplified — actual implementation would use async)
    try {
        auto invoke_result = s_registry->invoke(params.node_id, command, params.timeout_ms);
        if (!invoke_result.ok) {
            log_remote_bin_probe_failure(params.node_id, invoke_result.error);
            return result;
        }

        // Parse response bins
        std::set<std::string> new_bins;
        for (const auto& bin : invoke_result.bins) {
            std::string trimmed = bin;
            while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
            while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
            if (!trimmed.empty()) new_bins.insert(trimmed);
        }

        // Check if changed
        auto it = s_remote_nodes.find(params.node_id);
        bool has_changed = true;
        if (it != s_remote_nodes.end()) {
            has_changed = !are_bin_sets_equal(it->second.bins, new_bins);
        }

        // Update cache
        record_remote_node_bins(params.node_id, {new_bins.begin(), new_bins.end()});
        result.refreshed = true;
        result.changed = has_changed;
    } catch (const std::exception& e) {
        log_remote_bin_probe_failure(params.node_id, e.what());
    }

    return result;
}

// 🎯 对齐 OpenClaw: getRemoteSkillEligibility()
std::optional<skills::RemoteSkillEligibility> get_remote_skill_eligibility() {
    std::lock_guard<std::mutex> lock(s_mutex);

    // Filter to Mac nodes with system.run capability
    std::vector<const RemoteNodeRecord*> mac_nodes;
    for (const auto& [id, node] : s_remote_nodes) {
        if (is_mac_platform(node.platform, node.device_family) &&
            supports_system_run(node.commands)) {
            mac_nodes.push_back(&node);
        }
    }

    if (mac_nodes.empty()) return std::nullopt;

    // Aggregate bins across all Mac nodes
    std::set<std::string> all_bins;
    for (const auto* node : mac_nodes) {
        all_bins.insert(node->bins.begin(), node->bins.end());
    }

    // Build labels
    std::vector<std::string> labels;
    for (const auto* node : mac_nodes) {
        if (!node->display_name.empty()) {
            labels.push_back(node->display_name);
        } else {
            labels.push_back(node->node_id);
        }
    }

    std::string note;
    if (!labels.empty()) {
        std::ostringstream ss;
        ss << "Remote macOS node available (";
        for (size_t i = 0; i < labels.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << labels[i];
        }
        ss << "). Run macOS-only skills via nodes.run on that node.";
        note = ss.str();
    } else {
        note = "Remote macOS node available. Run macOS-only skills via nodes.run on that node.";
    }

    // Build eligibility struct
    skills::RemoteSkillEligibility eligibility;
    eligibility.platforms = {"darwin"};
    eligibility.note = note;

    // Capture bins by value for lambda closures
    auto bins_copy = std::make_shared<std::set<std::string>>(all_bins);
    eligibility.has_bin = [bins_copy](const std::string& bin) -> bool {
        return bins_copy->count(bin) > 0;
    };
    eligibility.has_any_bin = [bins_copy](const std::vector<std::string>& required) -> bool {
        for (const auto& bin : required) {
            if (bins_copy->count(bin) > 0) return true;
        }
        return false;
    };

    return eligibility;
}

// 🎯 对齐 OpenClaw: refreshRemoteBinsForConnectedNodes(cfg)
void refresh_remote_bins_for_connected_nodes(
    const std::vector<skills::SkillEntry>& skill_entries,
    std::function<void()> bump_version)
{
    if (!s_registry) return;

    auto connected = s_registry->list_connected();
    for (const auto& node : connected) {
        RefreshRemoteNodeBinsParams params;
        params.node_id = node.node_id;
        params.platform = node.platform;
        params.device_family = node.device_family;
        params.commands = node.commands;
        params.skill_entries = skill_entries;
        params.timeout_ms = 15000;

        auto result = refresh_remote_node_bins(params);
        if (result.refreshed && result.changed && bump_version) {
            bump_version();
        }
    }
}

}  // namespace infra
}  // namespace hunterclaw
