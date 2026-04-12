// ============================================================================
// net_utils.cc
// 🎯 严格对齐 OpenClaw: src/gateway/net.ts (562 行)
//
// 网络地址工具集：
// - IP 地址分类（回环、私有、公网）
// - Host 头解析（IPv6 方括号、端口剥离）
// - WebSocket URL 安全检查
// - LAN IP 探测
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include <string>
#include <algorithm>
#include <cstring>
#include <vector>
#include <array>

#ifdef __linux__
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace hunterclaw {
namespace gateway {
namespace net {

// ── IP Address Classification ───────────────────────────────────────────────
// 🎯 对齐: isLoopbackIpAddress, isPrivateOrLoopbackIpAddress

static std::string normalize_lowercase(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

// 🎯 对齐: isLoopbackIpAddress(ip)
bool is_loopback_address(const std::string& ip) {
    std::string trimmed = trim(ip);
    if (trimmed.empty()) return false;

    // IPv4 loopback: 127.0.0.0/8
    if (trimmed.rfind("127.", 0) == 0) return true;

    // IPv6 loopback: ::1
    if (trimmed == "::1" || trimmed == "0000:0000:0000:0000:0000:0000:0000:0001") return true;

    // localhost
    std::string lower = normalize_lowercase(trimmed);
    if (lower == "localhost") return true;

    return false;
}

// 🎯 对齐: isPrivateOrLoopbackIpAddress(ip)
// Private ranges: RFC1918, link-local, ULA IPv6, CGNAT (100.64/10), plus loopback
bool is_private_or_loopback_address(const std::string& ip) {
    std::string trimmed = trim(ip);
    if (trimmed.empty()) return false;

    if (is_loopback_address(trimmed)) return true;

    // RFC1918 private ranges
    if (trimmed.rfind("10.", 0) == 0) return true;
    if (trimmed.rfind("192.168.", 0) == 0) return true;

    // 172.16.0.0/12
    if (trimmed.rfind("172.", 0) == 0) {
        // Extract second octet
        size_t dot1 = trimmed.find('.');
        if (dot1 != std::string::npos) {
            size_t dot2 = trimmed.find('.', dot1 + 1);
            if (dot2 != std::string::npos) {
                int second = std::atoi(trimmed.substr(dot1 + 1, dot2 - dot1 - 1).c_str());
                if (second >= 16 && second <= 31) return true;
            }
        }
    }

    // CGNAT: 100.64.0.0/10
    if (trimmed.rfind("100.", 0) == 0) {
        size_t dot1 = trimmed.find('.');
        if (dot1 != std::string::npos) {
            size_t dot2 = trimmed.find('.', dot1 + 1);
            if (dot2 != std::string::npos) {
                int second = std::atoi(trimmed.substr(dot1 + 1, dot2 - dot1 - 1).c_str());
                if (second >= 64 && second <= 127) return true;
            }
        }
    }

    // Link-local: 169.254.0.0/16
    if (trimmed.rfind("169.254.", 0) == 0) return true;

    // IPv6 private ranges
    std::string lower = normalize_lowercase(trimmed);
    if (lower.rfind("fc", 0) == 0 || lower.rfind("fd", 0) == 0) return true;  // ULA
    if (lower.rfind("fe80:", 0) == 0) return true;  // Link-local

    return false;
}

// ── Host Header Parsing ─────────────────────────────────────────────────────
// 🎯 对齐: normalizeHostHeader, resolveHostName

std::string normalize_host_header(const std::string& host_header) {
    return normalize_lowercase(trim(host_header));
}

// 🎯 对齐: resolveHostName(hostHeader)
// Extract hostname from Host header, handling IPv6 brackets and ports
std::string resolve_host_name(const std::string& host_header) {
    std::string host = normalize_host_header(host_header);
    if (host.empty()) return "";

    // IPv6 in brackets: [::1]:8080 → ::1
    if (host[0] == '[') {
        size_t end = host.find(']');
        if (end != std::string::npos) {
            return host.substr(1, end - 1);
        }
    }

    // Simple IPv4 or hostname with port: host:port
    size_t last_colon = host.rfind(':');
    if (last_colon != std::string::npos) {
        // Only strip port if there's exactly one colon (IPv4:port, not IPv6)
        size_t first_colon = host.find(':');
        if (first_colon == last_colon) {
            return host.substr(0, last_colon);
        }
    }

    return host;
}

// ── stripOptionalPort ───────────────────────────────────────────────────────
// 🎯 对齐: stripOptionalPort(ip)

static std::string strip_optional_port(const std::string& ip) {
    if (ip.empty()) return ip;

    // IPv6 in brackets
    if (ip[0] == '[') {
        size_t end = ip.find(']');
        if (end != std::string::npos) {
            return ip.substr(1, end - 1);
        }
    }

    // IPv4 with port
    size_t last_colon = ip.rfind(':');
    if (last_colon != std::string::npos && ip.find('.') != std::string::npos) {
        size_t first_colon = ip.find(':');
        if (first_colon == last_colon) {
            return ip.substr(0, last_colon);
        }
    }

    return ip;
}

// 🎯 对齐: parseIpLiteral(raw)
static std::string parse_ip_literal(const std::string& raw) {
    std::string trimmed = trim(raw);
    if (trimmed.empty()) return "";

    std::string stripped = strip_optional_port(trimmed);
    // Basic validation: must look like an IP
    if (stripped.empty()) return "";

    return stripped;
}

// ── isLoopbackHost ──────────────────────────────────────────────────────────
// 🎯 对齐: isLoopbackHost(host) — used by client.ts

bool is_loopback_host(const std::string& host) {
    std::string parsed = parse_ip_literal(host);
    if (!parsed.empty()) {
        return is_loopback_address(parsed);
    }
    std::string lower = normalize_lowercase(trim(host));
    return lower == "localhost";
}

// ── isSecureWebSocketUrl ────────────────────────────────────────────────────
// 🎯 对齐: isSecureWebSocketUrl(url, opts)
// Returns true if wss://, ws://loopback, or allowPrivateWs

bool is_secure_websocket_url(const std::string& url, bool allow_private_ws) {
    std::string lower = normalize_lowercase(url);

    // wss:// is always secure
    if (lower.rfind("wss://", 0) == 0) return true;

    // Must be ws:// to continue
    if (lower.rfind("ws://", 0) != 0) return false;

    // Extract host from ws://host:port/path
    std::string after_scheme = url.substr(5);  // skip "ws://"
    size_t slash = after_scheme.find('/');
    std::string host_port = (slash != std::string::npos)
        ? after_scheme.substr(0, slash) : after_scheme;

    std::string host = resolve_host_name(host_port);
    if (is_loopback_host(host)) return true;

    // Private network escape hatch
    if (allow_private_ws) return true;

    return false;
}

// ── pickPrimaryLanIPv4 ──────────────────────────────────────────────────────
// 🎯 对齐: pickPrimaryLanIPv4()
// Pick the primary non-internal IPv4 address (LAN IP)

std::string pick_primary_lan_ipv4() {
#ifdef __linux__
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) return "";

    std::string result;
    // Prefer en0/eth0
    std::vector<std::string> preferred = {"en0", "eth0", "wlan0"};

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;

        char addr_buf[INET_ADDRSTRLEN];
        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        inet_ntop(AF_INET, &sin->sin_addr, addr_buf, sizeof(addr_buf));

        std::string addr(addr_buf);
        if (is_loopback_address(addr)) continue;

        std::string name(ifa->ifa_name ? ifa->ifa_name : "");
        for (const auto& pref : preferred) {
            if (name == pref) {
                result = addr;
                freeifaddrs(ifaddr);
                return result;
            }
        }
        if (result.empty()) result = addr;
    }

    freeifaddrs(ifaddr);
    return result;
#else
    return "";
#endif
}

// ── resolveGatewayPort ──────────────────────────────────────────────────────
// 🎯 对齐: resolveGatewayPort(config, env)

int resolve_gateway_port(const nlohmann::json& config, int default_port) {
    if (config.contains("gateway") && config["gateway"].contains("port")) {
        return config["gateway"]["port"].get<int>();
    }
    // Check env
    const char* env_port = std::getenv("HUNTERCLAW_GATEWAY_PORT");
    if (env_port) {
        int p = std::atoi(env_port);
        if (p > 0 && p < 65536) return p;
    }
    return default_port;
}

}  // namespace net
}  // namespace gateway
}  // namespace hunterclaw
