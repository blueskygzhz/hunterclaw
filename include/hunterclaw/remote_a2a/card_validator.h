#pragma once

// ============================================================================
// remote_a2a/card_validator.h
// AgentCard 的 RPC URL SSRF 校验。
//
// 移植 Google ADK 的 _validate_card_rpc_targets（remote_a2a_agent.py:908-951）
// 并增加一条 ADK 没有的检查（解析后 IP 网段）。
//
// ── ADK 原注释（:908-919）──────────────────────────────────────────────────
//   "Constrains where a card fetched over the network may aim RPC traffic.
//    Every URL the card offers is checked, not only the one this ADK version
//    would select, because the client factory negotiates the endpoint across
//    the card's whole interface list. Each must be https and share the origin
//    the card was fetched from; plain http stays allowed on a loopback host...
//    A card passed in directly or read from a local file did not come off the
//    network here, so its target is left to the caller."
// ============================================================================

#include <string>

#include "hunterclaw/core/types.h"
#include "hunterclaw/remote_a2a/agent_card.h"

namespace hunterclaw {
namespace remote_a2a {

/// 五条校验，任一不过即 Err：
///   ① 只校验网络来源的卡（source 非 http/https → 直接放行，由调用方负责）
///   ② 每个 RPC URL 必须 https；例外：loopback 主机上的 http（本地开发）
///   ③ 每个 RPC URL 的 origin 必须等于拉卡的 origin
///      （防 evil.com 返回一张把 RPC 指向 internal-svc:8080 的卡）
///   ④ 遍历卡里【全部】 interface 的 URL，不只是当前会选中的那一个
///   ⑤ ★ ADK 没有的增强：解析后的 IP 不得落私有/保留网段
///      （光看域名不够 —— evil.com 可以解析到 10.0.0.1）
///
/// @param card         已解析的卡（消杀前或后均可，本函数只看 URL）
/// @param source_url   拉卡的 URL；本地文件路径或空串表示非网络来源
/// @param resolve_dns  是否执行第 ⑤ 条（单测可关闭以避免真实 DNS）
core::Result<bool> validate_card_rpc_targets(const AgentCard& card,
                                             const std::string& source_url,
                                             bool resolve_dns = true);

/// 判断 source 是否网络来源（http:// 或 https:// 前缀）
bool is_network_source(const std::string& source);

}  // namespace remote_a2a
}  // namespace hunterclaw
