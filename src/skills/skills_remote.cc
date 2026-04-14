// Aligned with openclaw: src/infra/skills-remote.ts (364 lines)
// Remote node skill eligibility tracking: record nodes, probe bins, build eligibility context.
#include "hunterclaw/skills/skill.h"
#include <spdlog/spdlog.h>
#include <mutex>
#include <map>
#include <algorithm>

namespace hunterclaw {
namespace skills {

namespace {
    auto log() {
        static auto logger = spdlog::get("skills-remote");
        if (!logger) logger = spdlog::stdout_logger_mt("skills-remote");
        return logger;
    }

    // 对齐 OC: isMacPlatform (line 81-94)
    bool is_mac_platform(const std::string& platform, const std::string& device_family) {
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return s;
        };
        std::string p = lower(platform);
        std::string f = lower(device_family);
        return p.find("mac") != std::string::npos ||
               p.find("darwin") != std::string::npos ||
               f == "mac";
    }

    // 对齐 OC: supportsSystemRun / supportsSystemWhich (line 96-102)
    bool supports_command(const std::vector<std::string>& commands, const std::string& cmd) {
        return std::find(commands.begin(), commands.end(), cmd) != commands.end();
    }
}

// ============================================================================
// SkillsRemote::Impl
// ============================================================================

struct SkillsRemote::Impl {
    mutable std::mutex mutex;
    std::map<std::string, RemoteNodeRecord> nodes;

    // 对齐 OC: upsertNode (line 104-124)
    void upsert(const RemoteNodeRecord& record) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = nodes.find(record.node_id);
        if (it != nodes.end()) {
            auto& existing = it->second;
            if (!record.display_name.empty()) existing.display_name = record.display_name;
            if (!record.platform.empty()) existing.platform = record.platform;
            if (!record.device_family.empty()) existing.device_family = record.device_family;
            if (!record.commands.empty()) existing.commands = record.commands;
            if (!record.remote_ip.empty()) existing.remote_ip = record.remote_ip;
            if (!record.bins.empty()) existing.bins = record.bins;
        } else {
            nodes[record.node_id] = record;
        }
    }

    void remove(const std::string& node_id) {
        std::lock_guard<std::mutex> lock(mutex);
        nodes.erase(node_id);
    }

    void update_bins(const std::string& node_id, const std::vector<std::string>& bins) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = nodes.find(node_id);
        if (it != nodes.end()) {
            it->second.bins = std::set<std::string>(bins.begin(), bins.end());
        }
    }

    // 对齐 OC: getRemoteSkillEligibility (line 319-347)
    std::optional<RemoteSkillEligibility> get_eligibility(bool advertise_exec_node) const {
        std::lock_guard<std::mutex> lock(mutex);

        std::vector<const RemoteNodeRecord*> mac_nodes;
        for (const auto& [id, node] : nodes) {
            if (is_mac_platform(node.platform, node.device_family) &&
                supports_command(node.commands, "system.run")) {
                mac_nodes.push_back(&node);
            }
        }
        if (mac_nodes.empty()) return std::nullopt;

        // Merge all bins
        std::set<std::string> all_bins;
        std::vector<std::string> labels;
        for (const auto* node : mac_nodes) {
            for (const auto& bin : node->bins) all_bins.insert(bin);
            std::string label = node->display_name.empty() ? node->node_id : node->display_name;
            if (!label.empty()) labels.push_back(label);
        }

        RemoteSkillEligibility elig;
        elig.platforms = {"darwin"};
        elig.has_bin = [all_bins](const std::string& bin) {
            return all_bins.count(bin) > 0;
        };
        elig.has_any_bin = [all_bins](const std::vector<std::string>& required) {
            return std::any_of(required.begin(), required.end(),
                [&](const std::string& bin) { return all_bins.count(bin) > 0; });
        };
        if (advertise_exec_node && !labels.empty()) {
            elig.note = "Remote macOS node available (" +
                [&]() {
                    std::string s;
                    for (size_t i = 0; i < labels.size(); i++) {
                        if (i > 0) s += ", ";
                        s += labels[i];
                    }
                    return s;
                }() +
                "). Run macOS-only skills via exec host=node on that node.";
        }
        return elig;
    }
};

// ============================================================================
// SkillsRemote Singleton
// ============================================================================

SkillsRemote::SkillsRemote() : impl_(std::make_unique<Impl>()) {}

SkillsRemote& SkillsRemote::instance() {
    static SkillsRemote inst;
    return inst;
}

void SkillsRemote::record_node(const RemoteNodeRecord& node) {
    impl_->upsert(node);
    log()->info("Recorded remote node: {} ({})", node.node_id,
                node.display_name.empty() ? "unnamed" : node.display_name);
}

void SkillsRemote::remove_node(const std::string& node_id) {
    impl_->remove(node_id);
    log()->info("Removed remote node: {}", node_id);
}

void SkillsRemote::record_bins(const std::string& node_id, const std::vector<std::string>& bins) {
    impl_->update_bins(node_id, bins);
    log()->debug("Updated bins for node {}: {} bins", node_id, bins.size());
}

std::optional<RemoteSkillEligibility> SkillsRemote::get_eligibility(bool advertise_exec_node) const {
    return impl_->get_eligibility(advertise_exec_node);
}

// 对齐 OC: refreshRemoteNodeBins (line 249-317) — simplified
void SkillsRemote::refresh_bins(const std::string& node_id, std::shared_ptr<core::Config> config) {
    // In full impl, this would invoke system.which/system.run on the remote node
    // via the gateway registry. Simplified here as the gateway layer handles invocation.
    log()->info("Refreshing remote bins for node: {} (stub — requires gateway registry)", node_id);
}

// 对齐 OC: primeRemoteSkillsCache (line 130-154) — simplified
void SkillsRemote::prime_cache() {
    // In full impl, reads paired node list and pre-populates cache.
    log()->info("Priming remote skills cache (stub — requires node-pairing integration)");
}

}  // namespace skills
}  // namespace hunterclaw
