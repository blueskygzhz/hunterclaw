#pragma once

// ============================================================================
// remote_a2a/agent_card.h
// AgentCard 数据模型 —— ★ 只读方向（客户端从不生成卡）。
//
// ⚠ 卡是【对端完全可控】的输入，等同不可信用户输入。三条纪律：
//   ① 所有文本字段进 prompt 前必须过 card_sanitizer
//   ② 所有 URL 必须过 validate_card_rpc_targets（SSRF）
//   ③ 策略字段（requires_approval 等）【绝不】从卡读，只从本地配置读
//
// ── capabilities 不可信 ─────────────────────────────────────────────────────
// Google ADK 的 AgentCardBuilder 默认 `capabilities or AgentCapabilities()`
// （agent_card_builder.py:72），三张官方示例卡全是 `"capabilities": {}`。
// 所以「读卡决定用不用 SSE」在真实世界会永远走 false 分支。
// 解法见 remote_agent_client.h 的 ProbedCapabilities（乐观尝试+结果缓存）。
// ============================================================================

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hunterclaw/core/types.h"

namespace hunterclaw {
namespace remote_a2a {

struct AgentSkill {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> tags;
    std::vector<std::string> examples;
    std::vector<std::string> input_modes;
    std::vector<std::string> output_modes;
};

struct AgentCapabilities {
    /// ⚠ 缺字段一律 false（安全默认），不猜、不推导
    bool streaming = false;
    bool push_notifications = false;
    bool state_transition_history = false;
    std::vector<std::string> extensions;
};

/// 卡上的传输绑定条目（A2A 1.x 支持多绑定）
struct AgentInterface {
    std::string url;
    std::string transport;  // "JSONRPC" / "HTTP+JSON" / "GRPC"
};

struct AgentCard {
    std::string protocol_version;
    std::string name;
    std::string description;
    std::string version;
    /// 主 RPC URL
    std::string url;
    /// 额外传输绑定。★ SSRF 校验必须遍历【全部】，不能只看 url。
    std::vector<AgentInterface> interfaces;

    AgentCapabilities capabilities;
    std::vector<std::string> default_input_modes;
    std::vector<std::string> default_output_modes;
    std::vector<AgentSkill> skills;

    /// 原样保留供鉴权一致性检查（不进 prompt）
    nlohmann::json security_schemes;
    nlohmann::json security;
};

/// 解析卡。畸形字段一律用安全默认，不抛异常。
/// 仅在「根本不是 object」或「无任何 RPC URL」时返回 Err。
core::Result<AgentCard> parse_agent_card(const nlohmann::json&);

/// 收集卡里所有可能的 RPC URL（url + interfaces[].url），去重、保序。
///
/// ⚠ 必须遍历全部而非只取 url —— ADK 注释（remote_a2a_agent.py:911-915）指出：
///   client factory 会在整个 interface 列表里协商端点，
///   只校验当前版本会选中的那一个，等于没校验。
std::vector<std::string> collect_all_rpc_urls(const AgentCard&);

/// 按偏好传输选出实际要用的 RPC URL。
/// 优先 JSONRPC，其次 HTTP+JSON，最后回落到 card.url。
std::string select_rpc_url(const AgentCard&);

}  // namespace remote_a2a
}  // namespace hunterclaw
