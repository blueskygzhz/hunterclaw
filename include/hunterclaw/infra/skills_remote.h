#pragma once

// ============================================================================
// skills_remote.h
// 🎯 严格对齐 OpenClaw: src/infra/skills-remote.ts
//
// 远程 macOS 节点的技能资格管理接口
// ============================================================================

#include "hunterclaw/skills/skill.h"
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hunterclaw {
namespace infra {

// ============================================================================
// Node Registry interface (对齐 OpenClaw: NodeRegistry)
// ============================================================================

struct NodeInvokeResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> bins;
    std::string payload_json;
};

struct ConnectedNode {
    std::string node_id;
    std::string platform;
    std::string device_family;
    std::vector<std::string> commands;
};

class NodeRegistry {
public:
    virtual ~NodeRegistry() = default;
    virtual NodeInvokeResult invoke(const std::string& node_id,
                                     const std::string& command,
                                     int timeout_ms = 15000) = 0;
    virtual std::vector<ConnectedNode> list_connected() = 0;
};

// ============================================================================
// Data types
// ============================================================================

struct RemoteNodeInfo {
    std::string node_id;
    std::string display_name;
    std::string platform;
    std::string device_family;
    std::vector<std::string> commands;
    std::string remote_ip;
};

struct PairedNodeInfo : RemoteNodeInfo {
    std::optional<std::vector<std::string>> bins;
};

struct RefreshRemoteNodeBinsParams {
    std::string node_id;
    std::string platform;
    std::string device_family;
    std::vector<std::string> commands;
    std::vector<skills::SkillEntry> skill_entries;
    int timeout_ms = 15000;
};

struct RefreshRemoteNodeBinsResult {
    bool refreshed = false;
    bool changed = false;
};

// ============================================================================
// Public API (对齐 OpenClaw 导出函数)
// ============================================================================

// 🎯 对齐 OpenClaw: setSkillsRemoteRegistry(registry | null)
void set_skills_remote_registry(NodeRegistry* registry);

// 🎯 对齐 OpenClaw: primeRemoteSkillsCache()
void prime_remote_skills_cache(const std::vector<PairedNodeInfo>& paired_nodes,
                                std::function<void()> bump_version = nullptr);

// 🎯 对齐 OpenClaw: recordRemoteNodeInfo(node)
void record_remote_node_info(const RemoteNodeInfo& info);

// 🎯 对齐 OpenClaw: recordRemoteNodeBins(nodeId, bins)
void record_remote_node_bins(const std::string& node_id,
                              const std::vector<std::string>& bins);

// 🎯 对齐 OpenClaw: removeRemoteNodeInfo(nodeId)
void remove_remote_node_info(const std::string& node_id);

// 🎯 对齐 OpenClaw: refreshRemoteNodeBins(params)
RefreshRemoteNodeBinsResult refresh_remote_node_bins(const RefreshRemoteNodeBinsParams& params);

// 🎯 对齐 OpenClaw: getRemoteSkillEligibility()
std::optional<skills::RemoteSkillEligibility> get_remote_skill_eligibility();

// 🎯 对齐 OpenClaw: refreshRemoteBinsForConnectedNodes(cfg)
void refresh_remote_bins_for_connected_nodes(
    const std::vector<skills::SkillEntry>& skill_entries,
    std::function<void()> bump_version = nullptr);

}  // namespace infra
}  // namespace hunterclaw
