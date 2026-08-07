#include "hunterclaw/mcp/mcp_connection_resolver.h"

#include <algorithm>
#include <cctype>

// 🎯 对齐 OpenClaw src/agents/mcp-connection-resolver.ts（可移植纯算法部分）。

namespace hunterclaw {
namespace mcp {

namespace {

using json = nlohmann::json;

std::string trim_lower(const std::string& v) {
    size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = v.find_last_not_of(" \t\r\n");
    std::string s = v.substr(b, e - b + 1);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 对齐 resolveOpenClawMcpTransportAlias: http/streamable-http→"streamable-http"，
//   sse→"sse"，其余（含 stdio/未知）→ nullopt。
std::optional<std::string> resolve_transport_alias(const json& value) {
    if (!value.is_string()) return std::nullopt;
    std::string v = trim_lower(value.get<std::string>());
    if (v == "http" || v == "streamable-http") return std::string("streamable-http");
    if (v == "sse") return std::string("sse");
    return std::nullopt;
}

} // namespace

nlohmann::json apply_mcp_connection_override(
    const nlohmann::json& raw_server,
    const McpServerConnectionResolved& override_conn) {
    // base = isRecord(rawServer) ? {...rawServer} : {}
    json base = raw_server.is_object() ? raw_server : json::object();

    base["url"] = override_conn.url;
    if (override_conn.headers.has_value()) {
        json h = json::object();
        for (const auto& [k, v] : *override_conn.headers) h[k] = v;
        base["headers"] = std::move(h);
    } else {
        base.erase("headers");
    }

    // transport = alias(base.transport) ?? alias(base.type) ?? "streamable-http"
    std::optional<std::string> from_transport;
    if (base.contains("transport")) {
        from_transport = resolve_transport_alias(base["transport"]);
    }
    std::optional<std::string> from_type;
    if (base.contains("type")) {
        from_type = resolve_transport_alias(base["type"]);
    }
    if (from_transport.has_value()) {
        base["transport"] = *from_transport;
    } else if (from_type.has_value()) {
        base["transport"] = *from_type;
    } else {
        base["transport"] = "streamable-http";
    }

    // resolver-supplied headers 是唯一 auth 面：剥离静态 auth/oauth/type/command/args。
    base.erase("auth");
    base.erase("oauth");
    base.erase("type");
    base.erase("command");
    base.erase("args");
    return base;
}

std::string build_mcp_requester_runtime_cache_key(
    const std::string& session_id,
    const std::optional<std::string>& message_channel,
    const std::optional<std::string>& agent_account_id,
    const std::string& requester_sender_id) {
    // 对齐 JSON.stringify({sessionId, messageChannel, agentAccountId, requesterSenderId})
    // 键顺序须与 OC 一致（对象字面量插入顺序）。
    json obj;
    obj["sessionId"] = session_id;
    obj["messageChannel"] = message_channel.value_or("");
    obj["agentAccountId"] = agent_account_id.value_or("");
    obj["requesterSenderId"] = requester_sender_id;
    return obj.dump();
}

nlohmann::json redact_mcp_servers_for_fingerprint(
    const nlohmann::json& mcp_servers,
    const std::set<std::string>& requester_scoped_server_names) {
    json redacted = json::object();
    if (!mcp_servers.is_object()) return redacted;

    // 按 server 名字典序遍历（对齐 toSorted）。
    std::vector<std::string> names;
    for (auto it = mcp_servers.begin(); it != mcp_servers.end(); ++it) {
        names.push_back(it.key());
    }
    std::sort(names.begin(), names.end());

    for (const auto& server_name : names) {
        const json& raw_server = mcp_servers[server_name];
        if (!requester_scoped_server_names.count(server_name)) {
            redacted[server_name] = raw_server;
            continue;
        }
        if (!raw_server.is_object()) {
            redacted[server_name] = json{{"connection", "requester-scoped"}};
            continue;
        }
        // 剥离 url/headers/command/args/env，保留其余，附加 connection 标记。
        json rest = raw_server;
        rest.erase("url");
        rest.erase("headers");
        rest.erase("command");
        rest.erase("args");
        rest.erase("env");
        rest["connection"] = "requester-scoped";
        redacted[server_name] = std::move(rest);
    }
    return redacted;
}

McpServerPartition partition_mcp_servers_by_connection_scope(
    const nlohmann::json& mcp_servers,
    const std::set<std::string>& resolver_server_names) {
    McpServerPartition out;
    out.static_servers = json::object();
    if (!mcp_servers.is_object()) return out;

    std::vector<std::string> names;
    for (auto it = mcp_servers.begin(); it != mcp_servers.end(); ++it) {
        names.push_back(it.key());
    }
    std::sort(names.begin(), names.end());

    for (const auto& server_name : names) {
        if (resolver_server_names.count(server_name)) {
            out.requester_scoped_server_names.push_back(server_name);
            continue;
        }
        out.static_servers[server_name] = mcp_servers[server_name];
    }
    return out;
}

} // namespace mcp
} // namespace hunterclaw
