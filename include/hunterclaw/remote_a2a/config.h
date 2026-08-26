#pragma once

// ============================================================================
// remote_a2a/config.h
// Remote A2A 配置解析。
//
// 配置形态（config.yaml → nlohmann::json）：
//
//   remoteA2a:
//     enabled: true
//     callbackBaseUrl: "https://hc.example.com"   # 启用回调时必填
//     defaults:
//       yieldMs: 30000
//       maxTaskMs: 1800000
//       idleTimeoutMs: 120000
//       cardTtlMs: 900000
//       probe: { baseIntervalMs: 5000, maxIntervalMs: 60000, maxNoProgress: 6 }
//     agents:
//       - name: research
//         card: "https://research.example.com"     # base URL / well-known / 本地文件
//         auth: { kind: bearer, tokenEnv: RESEARCH_TOKEN }
//         requiresApproval: true
//         enableCallback: false
//         forceStreaming: false
//
// ── 三条纪律 ────────────────────────────────────────────────────────────────
// ① 明文凭据【禁止】进配置 —— 由 parse_auth_spec 强制（auth.h:86）
// ② 策略字段（requiresApproval / ownerOnly）只在这里读，【绝不】从卡读
// ③ 单个 agent 配置错误【不】导致整体失败 —— 收集 warning 后跳过该项
//    （対标 MCP：一个 server 离线不该让所有工具消失）
// ============================================================================

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hunterclaw/core/types.h"
#include "hunterclaw/remote_a2a/agent_tool.h"
#include "hunterclaw/remote_a2a/card_resolver.h"
#include "hunterclaw/remote_a2a/execution_engine.h"
#include "hunterclaw/remote_a2a/remote_agent_client.h"

namespace hunterclaw {
namespace remote_a2a {

/// 单个远端的完整配置
struct RemoteAgentConfig {
    /// 逻辑名（工具名与日志用）。必填、需唯一。
    std::string name;
    /// 卡来源
    std::string card_source;

    CardResolverConfig card;
    RemoteAgentClientConfig client;
    ExecutionConfig exec;
    AgentToolPolicy policy;

    bool enabled = true;
};

/// 全局配置
struct RemoteA2aConfig {
    bool enabled = false;
    std::string callback_base_url;
    std::vector<RemoteAgentConfig> agents;
    PendingStoreConfig store;

    /// 解析期收集的警告（配置错误、被跳过的 agent 等）。
    /// ★ 非空【不】代表失败 —— 调用方应记日志但继续启动。
    std::vector<std::string> warnings;
};

/// 解析 remoteA2a 配置节点。
///
/// @param node  config.data["remoteA2a"]（缺失/非 object 时返回 enabled=false）
///
/// 返回 Err 只在【全局性】错误时发生：
///   · enabled=true 但 agents 为空或全部无效
///   · enableCallback=true 但 callbackBaseUrl 缺失/非 https
/// 单个 agent 的错误进 warnings 并跳过。
core::Result<RemoteA2aConfig> parse_remote_a2a_config(const nlohmann::json& node);

/// 默认值（単测与文档用）
RemoteAgentConfig default_remote_agent_config();

}  // namespace remote_a2a
}  // namespace hunterclaw
