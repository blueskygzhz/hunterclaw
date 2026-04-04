// HunterClaw - Pairing Setup Code Implementation
// 严格对齐 OpenClaw src/pairing/setup-code.ts

#include "hunterclaw/pairing/setup_code.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <random>
#include <regex>
#include <sstream>

#include "hunterclaw/infra/utilities.h"

using json = nlohmann::json;

namespace hunterclaw {
namespace pairing {

namespace {

// ============================================================================
// URL Parsing & Normalization (setup-code.ts:65-95)
// ============================================================================

std::optional<std::string> try_parse_url(const std::string& raw,
                                          const std::string& scheme_fallback) {
  std::string trimmed = infra::trim(raw);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  // Try full URL parsing
  std::regex url_regex(R"(^([a-z]+)://([^:/]+)(?::(\d+))?)",
                       std::regex_constants::icase);
  std::smatch match;

  if (std::regex_search(trimmed, match, url_regex)) {
    std::string scheme = match[1].str();
    std::string host = match[2].str();
    std::string port = match[3].str();

    // Convert http/https to ws/wss
    if (scheme == "http") {
      scheme = "ws";
    } else if (scheme == "https") {
      scheme = "wss";
    }

    // Validate scheme
    if (scheme != "ws" && scheme != "wss") {
      return std::nullopt;
    }

    if (host.empty()) {
      return std::nullopt;
    }

    std::string port_suffix = port.empty() ? "" : ":" + port;
    return scheme + "://" + host + port_suffix;
  }

  // Fallback: treat as host:port
  auto slash_pos = trimmed.find('/');
  std::string without_path = slash_pos == std::string::npos
                                 ? trimmed
                                 : trimmed.substr(0, slash_pos);

  if (without_path.empty()) {
    return std::nullopt;
  }

  return scheme_fallback + "://" + without_path;
}

// ============================================================================
// Network Detection (setup-code.ts:109-154)
// ============================================================================

bool is_private_ipv4(const std::string& address) {
  // RFC 1918: 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16
  std::regex rfc1918_regex(
      R"(^(10\.|172\.(1[6-9]|2[0-9]|3[01])\.|192\.168\.))");
  return std::regex_search(address, rfc1918_regex);
}

bool is_tailnet_ipv4(const std::string& address) {
  // Carrier-grade NAT: 100.64.0.0/10 (Tailscale uses this)
  std::regex cgnat_regex(R"(^100\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\.)");
  return std::regex_search(address, cgnat_regex);
}

std::optional<std::string> pick_lan_ipv4(
    const std::function<std::vector<std::string>()>& get_interfaces) {
  if (!get_interfaces) {
    return std::nullopt;
  }

  auto interfaces = get_interfaces();
  for (const auto& addr : interfaces) {
    if (is_private_ipv4(addr)) {
      return addr;
    }
  }
  return std::nullopt;
}

std::optional<std::string> pick_tailnet_ipv4(
    const std::function<std::vector<std::string>()>& get_interfaces) {
  if (!get_interfaces) {
    return std::nullopt;
  }

  auto interfaces = get_interfaces();
  for (const auto& addr : interfaces) {
    if (is_tailnet_ipv4(addr)) {
      return addr;
    }
  }
  return std::nullopt;
}

// ============================================================================
// Auth Resolution (setup-code.ts:156-207)
// ============================================================================

std::optional<std::string> resolve_gateway_token_from_env(
    const std::unordered_map<std::string, std::string>& env) {
  auto it = env.find("OPENCLAW_GATEWAY_TOKEN");
  if (it != env.end() && !it->second.empty()) {
    return it->second;
  }
  it = env.find("CLAWDBOT_GATEWAY_TOKEN");
  if (it != env.end() && !it->second.empty()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<std::string> resolve_gateway_password_from_env(
    const std::unordered_map<std::string, std::string>& env) {
  auto it = env.find("OPENCLAW_GATEWAY_PASSWORD");
  if (it != env.end() && !it->second.empty()) {
    return it->second;
  }
  it = env.find("CLAWDBOT_GATEWAY_PASSWORD");
  if (it != env.end() && !it->second.empty()) {
    return it->second;
  }
  return std::nullopt;
}

struct AuthLabelResult {
  std::optional<std::string> label;  // "token" or "password"
  std::string error;
};

AuthLabelResult resolve_pairing_setup_auth_label(
    const std::unordered_map<std::string, std::string>& env) {
  auto token = resolve_gateway_token_from_env(env);
  auto password = resolve_gateway_password_from_env(env);

  if (token) {
    return {std::optional<std::string>("token"), ""};
  }
  if (password) {
    return {std::optional<std::string>("password"), ""};
  }

  return {std::nullopt,
          "Gateway auth is not configured (no token or password)."};
}

// ============================================================================
// Gateway URL Resolution (setup-code.ts:299-359)
// ============================================================================

struct ResolveUrlResult {
  std::optional<std::string> url;
  std::string source;
  std::string error;
};

ResolveUrlResult resolve_gateway_url_internal(
    const ResolvePairingSetupOptions& options) {
  // Determine scheme (ws vs wss)
  std::string scheme = "ws";
  if (options.force_secure) {
    scheme = "wss";
  }

  // 1. Public URL (highest priority if provided)
  if (options.public_url) {
    auto url = try_parse_url(*options.public_url, scheme);
    if (url) {
      return {url, "plugins.entries.device-pair.config.publicUrl", ""};
    }
    return {std::nullopt, "", "Configured publicUrl is invalid."};
  }

  // 2. Remote URL (if prefer_remote_url)
  auto remote_url_it = options.env.find("OPENCLAW_GATEWAY_REMOTE_URL");
  std::optional<std::string> remote_url;
  if (remote_url_it != options.env.end() && !remote_url_it->second.empty()) {
    remote_url = try_parse_url(remote_url_it->second, scheme);
  }

  if (options.prefer_remote_url && remote_url) {
    return {remote_url, "gateway.remote.url", ""};
  }

  // 3. Tailscale serve/funnel (if enabled)
  auto tailscale_mode_it = options.env.find("OPENCLAW_GATEWAY_TAILSCALE_MODE");
  if (tailscale_mode_it != options.env.end()) {
    std::string mode = tailscale_mode_it->second;
    if (mode == "serve" || mode == "funnel") {
      // Simplified: assume tailnet host is available in env
      auto tailnet_host_it = options.env.find("TAILSCALE_HOSTNAME");
      if (tailnet_host_it != options.env.end()) {
        return {"wss://" + tailnet_host_it->second,
                "gateway.tailscale.mode=" + mode, ""};
      }
      return {std::nullopt, "",
              "Tailscale Serve is enabled, but MagicDNS could not be "
              "resolved."};
    }
  }

  // 4. Remote URL fallback
  if (remote_url) {
    return {remote_url, "gateway.remote.url", ""};
  }

  // 5. LAN/Tailnet bind URL
  std::string gateway_port = "3000";
  auto port_it = options.env.find("OPENCLAW_GATEWAY_PORT");
  if (port_it != options.env.end()) {
    gateway_port = port_it->second;
  }

  // Try Tailnet first
  auto tailnet_ip = pick_tailnet_ipv4(options.get_network_interfaces);
  if (tailnet_ip) {
    return {scheme + "://" + *tailnet_ip + ":" + gateway_port,
            "gateway.bind=tailnet", ""};
  }

  // Try LAN
  auto lan_ip = pick_lan_ipv4(options.get_network_interfaces);
  if (lan_ip) {
    return {scheme + "://" + *lan_ip + ":" + gateway_port,
            "gateway.bind=lan", ""};
  }

  return {
      std::nullopt, "",
      "Gateway is only bound to loopback. Set gateway.bind=lan, enable "
      "tailscale serve, or configure "
      "plugins.entries.device-pair.config.publicUrl."};
}

}  // namespace

// ============================================================================
// Public API
// ============================================================================

std::optional<std::string> normalize_setup_url(const std::string& raw,
                                                const std::string& scheme_fallback) {
  return try_parse_url(raw, scheme_fallback);
}

std::string resolve_gateway_url(const ResolvePairingSetupOptions& options) {
  auto result = resolve_gateway_url_internal(options);
  return result.url.value_or("ws://localhost:3000");
}

std::string encode_pairing_setup_code(const PairingSetupPayload& payload) {
  // Aligned with setup-code.ts:361-365
  json j;
  j["url"] = payload.url;
  j["bootstrapToken"] = payload.bootstrap_token;

  std::string json_str = j.dump();
  auto bytes = std::vector<uint8_t>(json_str.begin(), json_str.end());
  std::string base64 = infra::base64_encode(bytes);

  // URL-safe Base64: + -> -, / -> _, remove trailing =
  std::string url_safe;
  url_safe.reserve(base64.size());
  for (char c : base64) {
    if (c == '+') {
      url_safe += '-';
    } else if (c == '/') {
      url_safe += '_';
    } else if (c != '=') {
      url_safe += c;
    }
  }

  return url_safe;
}

std::optional<PairingSetupPayload> decode_pairing_setup_code(
    const std::string& code) {
  try {
    // Convert URL-safe back to standard Base64
    std::string base64;
    base64.reserve(code.size());
    for (char c : code) {
      if (c == '-') {
        base64 += '+';
      } else if (c == '_') {
        base64 += '/';
      } else {
        base64 += c;
      }
    }

    // Add padding if needed
    while (base64.size() % 4 != 0) {
      base64 += '=';
    }

    auto bytes = infra::base64_decode(base64);
    std::string json_str(bytes.begin(), bytes.end());

    json j = json::parse(json_str);

    PairingSetupPayload payload;
    payload.url = j.value("url", "");
    payload.bootstrap_token = j.value("bootstrapToken", "");

    if (payload.url.empty() || payload.bootstrap_token.empty()) {
      return std::nullopt;
    }

    return payload;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::string issue_device_bootstrap_token(const std::string& pairing_base_dir) {
  // Generate random 32-byte token
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);

  std::vector<uint8_t> token_bytes(32);
  for (auto& byte : token_bytes) {
    byte = static_cast<uint8_t>(dis(gen));
  }

  std::string token = infra::base64_encode(token_bytes);

  // Simplified: store token in pairing_base_dir/bootstrap_tokens.json
  // (In production, this would have TTL, device metadata, etc.)
  std::string store_path = pairing_base_dir.empty()
                               ? "/tmp/hunterclaw_bootstrap_tokens.json"
                               : pairing_base_dir + "/bootstrap_tokens.json";

  try {
    json tokens;
    std::ifstream in(store_path);
    if (in.is_open()) {
      in >> tokens;
    }

    tokens[token] = {{"created_at", infra::current_timestamp_iso()},
                     {"expires_at",
                      infra::current_timestamp_ms() + 3600000}};  // 1 hour

    std::ofstream out(store_path);
    out << tokens.dump(2);
  } catch (const std::exception&) {
    // Ignore errors
  }

  return token;
}

std::optional<std::string> verify_bootstrap_token(
    const std::string& token, const std::string& pairing_base_dir) {
  std::string store_path = pairing_base_dir.empty()
                               ? "/tmp/hunterclaw_bootstrap_tokens.json"
                               : pairing_base_dir + "/bootstrap_tokens.json";

  try {
    std::ifstream in(store_path);
    if (!in.is_open()) {
      return std::nullopt;
    }

    json tokens;
    in >> tokens;

    if (!tokens.contains(token)) {
      return std::nullopt;
    }

    auto token_data = tokens[token];
    int64_t expires_at = token_data.value("expires_at", 0);
    if (expires_at < infra::current_timestamp_ms()) {
      // Token expired
      tokens.erase(token);
      std::ofstream out(store_path);
      out << tokens.dump(2);
      return std::nullopt;
    }

    // Generate device ID
    std::string device_id = infra::generate_uuid();

    // Remove token (single-use)
    tokens.erase(token);
    std::ofstream out(store_path);
    out << tokens.dump(2);

    return device_id;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

PairingSetupResolution resolve_pairing_setup_from_config(
    const ResolvePairingSetupOptions& options) {
  // Aligned with setup-code.ts:367-409

  // 1. Resolve auth label
  auto auth_result = resolve_pairing_setup_auth_label(options.env);
  if (!auth_result.label) {
    return {false, std::nullopt, std::nullopt, "", auth_result.error};
  }

  // 2. Resolve gateway URL
  auto url_result = resolve_gateway_url_internal(options);
  if (!url_result.url) {
    return {false, std::nullopt, std::nullopt, "",
            url_result.error.empty() ? "Gateway URL unavailable."
                                     : url_result.error};
  }

  // 3. Issue bootstrap token
  std::string bootstrap_token =
      issue_device_bootstrap_token(options.pairing_base_dir.value_or(""));

  // 4. Create payload
  PairingSetupPayload payload;
  payload.url = *url_result.url;
  payload.bootstrap_token = bootstrap_token;

  return {true, payload, auth_result.label, url_result.source, ""};
}

}  // namespace pairing
}  // namespace hunterclaw
