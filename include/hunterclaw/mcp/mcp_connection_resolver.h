#pragma once

// 🎯 对齐 OpenClaw src/agents/mcp-connection-resolver.ts 的可移植纯算法单元。
//   （requester-scoped 连接的完整解析依赖 plugin registry / gateway request scope /
//    secret redaction registry / crypto HMAC，属于宿主基础设施；此处移植与这些
//    依赖无关、可独立成立并逐行对齐的部分。）
//
//   包含：
//     - apply_mcp_connection_override: 用解析出的 url/headers 覆盖静态 server 配置
//       （仅用于 transport 构造，不修改原对象），并规范 transport、剥离 auth/oauth/
//        type/command/args。
//     - build_mcp_requester_runtime_cache_key: requester-scoped runtime 复合 cache key。
//     - redact_mcp_servers_for_fingerprint: 指纹脱敏（仅 identity+filters，绝不含凭证）。
//     - partition_mcp_servers_by_connection_scope: 按 resolver 注册表分区static/scoped。

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace mcp {

// 对齐 McpServerConnectionResolved: 解析出的连接材料（凭证，切勿日志/持久化）。
struct McpServerConnectionResolved {
    std::string url;
    std::optional<std::map<std::string, std::string>> headers;
};

// 对齐 applyMcpConnectionOverride:
//   base = {...rawServer}; base.url=override.url; headers 替换或删除；
//   transport =别名(transport) ?? 别名(type) ?? "streamable-http"；
//   删除 auth/oauth/type/command/args。返回新对象（不改rawServer）。
nlohmann::json apply_mcp_connection_override(
    const nlohmann::json& raw_server,
    const McpServerConnectionResolved& override_conn);

// 对齐 buildMcpRequesterRuntimeCacheKey:
//   JSON.stringify({sessionId, messageChannel, agentAccountId, requesterSenderId})
//   （空值归一化为""）。
std::string build_mcp_requester_runtime_cache_key(
    const std::string& session_id,
    const std::optional<std::string>& message_channel,
    const std::optional<std::string>& agent_account_id,
    const std::string& requester_sender_id);

// 对齐 redactMcpServersForFingerprint:
//   非 requester-scoped server 原样保留；scoped server 剥离
//   url/headers/command/args/env，附加 connection:"requester-scoped"。
//   按 server 名字典序遍历。
nlohmann::json redact_mcp_servers_for_fingerprint(
    const nlohmann::json& mcp_servers,
    const std::set<std::string>& requester_scoped_server_names);

// 对齐 partitionMcpServersByConnectionScope:
//   按已注册的 resolver server 名集合，将 servers 分为 static 与 requester-scoped。
struct McpServerPartition {
    nlohmann::json static_servers;                // object
    std::vector<std::string> requester_scoped_server_names;
};
McpServerPartition partition_mcp_servers_by_connection_scope(
    const nlohmann::json& mcp_servers,
    const std::set<std::string>& resolver_server_names);

} // namespace mcp
} // namespace hunterclaw
