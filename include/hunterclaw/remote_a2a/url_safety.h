#pragma once

// ============================================================================
// remote_a2a/url_safety.h
// URL 解析与 SSRF 相关的网段判定。纯函数（除 DNS 解析）。
//
// ⚠ 这一层存在的唯一理由：AgentCard 是【对端完全可控】的输入。
//   卡里的 RPC URL 若指向内网/metadata 服务，我们就成了 SSRF 跳板。
//
// Google ADK 做了域名级同源校验（remote_a2a_agent.py:908-951），
// 但【没有】解析后 IP 的网段校验 —— evil.com 可以解析到 10.0.0.1。
// 本文件补上这一条。
// ============================================================================

#include <optional>
#include <string>
#include <string_view>

namespace hunterclaw {
namespace remote_a2a {

struct ParsedUrl {
    std::string scheme;  // 已小写
    std::string host;    // 已小写；IPv6 已去掉方括号
    std::string port;    // 显式端口，未指定则为空
    std::string path;
    /// 是否为方括号形式的 IPv6 字面量
    bool host_is_ipv6_literal = false;

    /// scheme://host[:port]，端口未显式指定时补默认端口，
    /// 保证 http://a:80 与 http://a 判定同源。
    std::string origin() const;
};

/// 解析 URL。仅支持 http/https，其余（含 file://）返回 nullopt。
std::optional<ParsedUrl> parse_url(std::string_view url);

/// 便捷：只取 origin
std::optional<std::string> url_origin(std::string_view url);

/// 是否 loopback 主机：localhost / 127.0.0.0/8 / ::1 / [::1]
bool is_loopback_host(const std::string& host);

/// 字符串是否为 IPv4 字面量
bool is_ipv4_literal(const std::string& s);
/// 字符串是否为 IPv6 字面量（不含方括号）
bool is_ipv6_literal(const std::string& s);

/// 私有 / 保留 / 特殊用途网段判定。
///
/// IPv4: 0/8 · 10/8 · 100.64/10(CGNAT) · 127/8 · 169.254/16(link-local，
///       含云 metadata 169.254.169.254) · 172.16/12 · 192.0.0/24 ·
///       192.0.2/24 · 192.168/16 · 198.18/15 · 198.51.100/24 ·
///       203.0.113/24 · 224/4(multicast) · 240/4(reserved)
/// IPv6: :: · ::1 · fc00::/7(ULA) · fe80::/10(link-local) · ff00::/8(multicast)
///       · ::ffff:0:0/96(IPv4-mapped，递归判内嵌 v4)
///
/// ⚠ 传入非法 IP 字符串时返回 true（保守拒绝），不返回 false。
bool is_private_or_reserved(const std::string& ip);

/// 已知的云 metadata 主机名（DNS 层就该拒）
bool is_metadata_hostname(const std::string& host);

/// DNS 解析取第一个 A/AAAA 记录。失败返回 nullopt。
/// ⚠ 这是本文件唯一有 IO 的函数。
std::optional<std::string> resolve_first_ip(const std::string& host);

}  // namespace remote_a2a
}  // namespace hunterclaw
