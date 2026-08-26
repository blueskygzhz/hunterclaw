// ============================================================================
// remote_a2a/url_safety.cc
// URL 解析与 SSRF 网段判定实现。
// ============================================================================

#include "hunterclaw/remote_a2a/url_safety.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>

namespace hunterclaw {
namespace remote_a2a {

namespace {

std::string to_lower_copy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim_copy(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

/// 解析点分十进制为 32 位主机序整数
bool parse_ipv4(const std::string& s, uint32_t* out) {
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    *out = ntohl(a.s_addr);
    return true;
}

bool parse_ipv6(const std::string& s, std::array<uint8_t, 16>* out) {
    in6_addr a{};
    if (inet_pton(AF_INET6, s.c_str(), &a) != 1) return false;
    std::memcpy(out->data(), &a, 16);
    return true;
}

/// ip 是否落在 base/prefix_len 网段内（IPv4，主机序）
bool in_v4_net(uint32_t ip, const char* base, int prefix_len) {
    uint32_t b = 0;
    if (!parse_ipv4(base, &b)) return false;
    if (prefix_len <= 0) return true;
    if (prefix_len >= 32) return ip == b;
    const uint32_t mask = 0xFFFFFFFFu << (32 - prefix_len);
    return (ip & mask) == (b & mask);
}

const char* default_port_for(const std::string& scheme) {
    if (scheme == "https") return "443";
    if (scheme == "http") return "80";
    return "";
}

}  // namespace

// ── ParsedUrl ───────────────────────────────────────────────────────────────

std::string ParsedUrl::origin() const {
    std::string h = host_is_ipv6_literal ? ("[" + host + "]") : host;
    // ★ 端口归一化：显式默认端口与省略端口必须判为同源
    std::string p = port.empty() ? std::string(default_port_for(scheme)) : port;
    return scheme + "://" + h + (p.empty() ? "" : (":" + p));
}

std::optional<ParsedUrl> parse_url(std::string_view raw) {
    const std::string url = trim_copy(raw);
    if (url.empty()) return std::nullopt;

    const size_t sep = url.find("://");
    if (sep == std::string::npos) return std::nullopt;

    ParsedUrl u;
    u.scheme = to_lower_copy(url.substr(0, sep));
    // ★ 只接受 http/https。file:// 之类必须拒绝，否则会被当成"本地卡"处理。
    if (u.scheme != "http" && u.scheme != "https") return std::nullopt;

    std::string rest = url.substr(sep + 3);
    if (rest.empty()) return std::nullopt;

    // 去掉 userinfo（user:pass@host）—— 我们不支持 URL 内嵌凭据
    const size_t at = rest.find('@');
    const size_t first_slash_for_at = rest.find_first_of("/?#");
    if (at != std::string::npos &&
        (first_slash_for_at == std::string::npos || at < first_slash_for_at)) {
        rest = rest.substr(at + 1);
    }

    // 切出 authority 与 path
    const size_t slash = rest.find_first_of("/?#");
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    u.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    if (authority.empty()) return std::nullopt;

    // IPv6 字面量：[::1]:8080
    if (authority.front() == '[') {
        const size_t rb = authority.find(']');
        if (rb == std::string::npos) return std::nullopt;
        u.host = to_lower_copy(authority.substr(1, rb - 1));
        u.host_is_ipv6_literal = true;
        if (u.host.empty()) return std::nullopt;
        if (rb + 1 < authority.size()) {
            if (authority[rb + 1] != ':') return std::nullopt;
            u.port = authority.substr(rb + 2);
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            u.host = to_lower_copy(authority.substr(0, colon));
            u.port = authority.substr(colon + 1);
        } else {
            u.host = to_lower_copy(authority);
        }
        if (u.host.empty()) return std::nullopt;
    }

    // 端口必须全数字且在范围内
    if (!u.port.empty()) {
        if (u.port.size() > 5) return std::nullopt;
        for (char c : u.port) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        }
        const long v = std::strtol(u.port.c_str(), nullptr, 10);
        if (v <= 0 || v > 65535) return std::nullopt;
    }
    return u;
}

std::optional<std::string> url_origin(std::string_view url) {
    auto u = parse_url(url);
    if (!u) return std::nullopt;
    return u->origin();
}

// ── 主机判定 ────────────────────────────────────────────────────────────────

bool is_ipv4_literal(const std::string& s) {
    uint32_t dummy = 0;
    return parse_ipv4(s, &dummy);
}

bool is_ipv6_literal(const std::string& s) {
    std::array<uint8_t, 16> dummy{};
    return parse_ipv6(s, &dummy);
}

bool is_loopback_host(const std::string& host_in) {
    std::string host = to_lower_copy(trim_copy(host_in));
    // 容忍带方括号的写法
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    if (host.empty()) return false;
    if (host == "localhost" || host == "localhost.localdomain") return true;
    if (host == "::1" || host == "0:0:0:0:0:0:0:1") return true;

    uint32_t v4 = 0;
    if (parse_ipv4(host, &v4)) {
        return in_v4_net(v4, "127.0.0.0", 8);
    }
    std::array<uint8_t, 16> v6{};
    if (parse_ipv6(host, &v6)) {
        static const std::array<uint8_t, 16> kLoop = {0, 0, 0, 0, 0, 0, 0, 0,
                                                     0, 0, 0, 0, 0, 0, 0, 1};
        if (v6 == kLoop) return true;
        // ::ffff:127.0.0.1
        static const uint8_t kMappedPrefix[12] = {0, 0, 0, 0, 0, 0,
                                                  0, 0, 0, 0, 0xFF, 0xFF};
        if (std::memcmp(v6.data(), kMappedPrefix, 12) == 0) {
            const uint32_t emb = (static_cast<uint32_t>(v6[12]) << 24) |
                                 (static_cast<uint32_t>(v6[13]) << 16) |
                                 (static_cast<uint32_t>(v6[14]) << 8) |
                                 static_cast<uint32_t>(v6[15]);
            return in_v4_net(emb, "127.0.0.0", 8);
        }
    }
    return false;
}

bool is_metadata_hostname(const std::string& host_in) {
    const std::string h = to_lower_copy(trim_copy(host_in));
    static const char* const kNames[] = {
        "metadata",                      // 通用
        "metadata.google.internal",      // GCP
        "metadata.goog",                 // GCP
        "instance-data",                 // AWS 旧
        "metadata.tencentyun.com",       // 腾讯云
        "100.100.100.200",               // 阿里云 metadata
    };
    for (const char* n : kNames) {
        if (h == n) return true;
    }
    return false;
}

bool is_private_or_reserved(const std::string& ip_in) {
    const std::string ip = trim_copy(ip_in);
    if (ip.empty()) return true;  // ★ 保守：无法判定即拒绝

    uint32_t v4 = 0;
    if (parse_ipv4(ip, &v4)) {
        struct Net { const char* base; int len; };
        static const Net kNets[] = {
            {"0.0.0.0", 8},        // "this network"
            {"10.0.0.0", 8},       // 私有 A
            {"100.64.0.0", 10},    // CGNAT
            {"127.0.0.0", 8},      // loopback
            {"169.254.0.0", 16},   // link-local，含 169.254.169.254 metadata
            {"172.16.0.0", 12},    // 私有 B
            {"192.0.0.0", 24},     // IETF 协议分配
            {"192.0.2.0", 24},     // TEST-NET-1
            {"192.168.0.0", 16},   // 私有 C
            {"198.18.0.0", 15},    // 基准测试
            {"198.51.100.0", 24},  // TEST-NET-2
            {"203.0.113.0", 24},   // TEST-NET-3
            {"224.0.0.0", 4},      // multicast
            {"240.0.0.0", 4},      // reserved（含 255.255.255.255）
        };
        for (const auto& n : kNets) {
            if (in_v4_net(v4, n.base, n.len)) return true;
        }
        return false;
    }

    std::array<uint8_t, 16> v6{};
    if (parse_ipv6(ip, &v6)) {
        bool all_zero = true;
        for (uint8_t b : v6) {
            if (b != 0) { all_zero = false; break; }
        }
        if (all_zero) return true;  // ::

        static const std::array<uint8_t, 16> kLoop = {0, 0, 0, 0, 0, 0, 0, 0,
                                                     0, 0, 0, 0, 0, 0, 0, 1};
        if (v6 == kLoop) return true;  // ::1

        if ((v6[0] & 0xFE) == 0xFC) return true;                        // fc00::/7 ULA
        if (v6[0] == 0xFE && (v6[1] & 0xC0) == 0x80) return true;       // fe80::/10
        if (v6[0] == 0xFF) return true;                                 // ff00::/8

        // ::ffff:0:0/96 IPv4-mapped → 递归判内嵌 v4
        static const uint8_t kMappedPrefix[12] = {0, 0, 0, 0, 0, 0,
                                                  0, 0, 0, 0, 0xFF, 0xFF};
        if (std::memcmp(v6.data(), kMappedPrefix, 12) == 0) {
            char buf[INET_ADDRSTRLEN] = {0};
            in_addr a{};
            std::memcpy(&a.s_addr, v6.data() + 12, 4);
            if (inet_ntop(AF_INET, &a, buf, sizeof(buf))) {
                return is_private_or_reserved(buf);
            }
            return true;
        }
        // 2002::/16 6to4 与 2001::/32 Teredo 可封装内网地址，保守拒绝
        if (v6[0] == 0x20 && v6[1] == 0x02) return true;
        if (v6[0] == 0x20 && v6[1] == 0x01 && v6[2] == 0x00 && v6[3] == 0x00) {
            return true;
        }
        return false;
    }

    return true;  // ★ 既非合法 v4 也非合法 v6 → 保守拒绝
}

std::optional<std::string> resolve_first_ip(const std::string& host_in) {
    std::string host = trim_copy(host_in);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    if (host.empty()) return std::nullopt;

    // 已是字面量则直接返回，省一次 DNS
    if (is_ipv4_literal(host) || is_ipv6_literal(host)) return host;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        if (res) ::freeaddrinfo(res);
        return std::nullopt;
    }

    std::optional<std::string> out;
    char buf[INET6_ADDRSTRLEN] = {0};
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            auto* sa = reinterpret_cast<sockaddr_in*>(p->ai_addr);
            if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
                out = std::string(buf);
                break;
            }
        } else if (p->ai_family == AF_INET6) {
            auto* sa = reinterpret_cast<sockaddr_in6*>(p->ai_addr);
            if (inet_ntop(AF_INET6, &sa->sin6_addr, buf, sizeof(buf))) {
                out = std::string(buf);
                break;
            }
        }
    }
    ::freeaddrinfo(res);
    return out;
}

}  // namespace remote_a2a
}  // namespace hunterclaw
