// HunterClaw - Pairing Setup Code
// 严格对齐 OpenClaw src/pairing/setup-code.ts
// Purpose: Generate setup codes for mobile device pairing with Base64 encoding

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hunterclaw {
namespace pairing {

// ============================================================================
// Setup Code Payload
// ============================================================================

// Aligned with OpenClaw PairingSetupPayload (setup-code.ts:16-19)
struct PairingSetupPayload {
  std::string url;
  std::string bootstrap_token;
};

// ============================================================================
// Setup Code Resolution Options
// ============================================================================

// Aligned with OpenClaw ResolvePairingSetupOptions (setup-code.ts:32-40)
struct ResolvePairingSetupOptions {
  std::unordered_map<std::string, std::string> env;
  std::optional<std::string> public_url;
  bool prefer_remote_url = false;
  bool force_secure = false;
  std::optional<std::string> pairing_base_dir;
  
  // Network interface lookup (optional, for LAN/Tailnet detection)
  std::function<std::vector<std::string>()> get_network_interfaces;
};

// ============================================================================
// Setup Code Resolution Result
// ============================================================================

// Aligned with OpenClaw PairingSetupResolution (setup-code.ts:42-52)
struct PairingSetupResolution {
  bool ok;
  std::optional<PairingSetupPayload> payload;
  std::optional<std::string> auth_label;  // "token" or "password"
  std::string url_source;
  std::string error;
};

// ============================================================================
// Setup Code Encoding
// ============================================================================

// Encode pairing setup payload to URL-safe Base64 code
// Aligned with OpenClaw encodePairingSetupCode (setup-code.ts:361-365)
// 
// Algorithm:
// 1. JSON.stringify(payload)
// 2. Base64 encode
// 3. URL-safe: + -> -, / -> _, remove trailing =
std::string encode_pairing_setup_code(const PairingSetupPayload& payload);

// Decode Base64 setup code to payload
std::optional<PairingSetupPayload> decode_pairing_setup_code(const std::string& code);

// ============================================================================
// Gateway URL Resolution
// ============================================================================

// Normalize URL string (handle ws/wss, http/https conversion)
// Aligned with OpenClaw normalizeUrl (setup-code.ts:65-95)
std::optional<std::string> normalize_setup_url(
    const std::string& raw,
    const std::string& scheme_fallback = "ws");

// Resolve gateway URL from config & environment
// Priority (aligned with OpenClaw resolveGatewayUrl logic):
// 1. publicUrl (if provided)
// 2. gateway.remote.url (if preferRemoteUrl)
// 3. Tailscale serve/funnel (if enabled)
// 4. gateway.remote.url (fallback)
// 5. LAN/Tailnet bind URL
std::string resolve_gateway_url(const ResolvePairingSetupOptions& options);

// ============================================================================
// Bootstrap Token Management
// ============================================================================

// Issue device bootstrap token (temporary token for first-time pairing)
// Aligned with OpenClaw issueDeviceBootstrapToken
std::string issue_device_bootstrap_token(const std::string& pairing_base_dir = "");

// Verify and consume bootstrap token (returns device ID if valid)
std::optional<std::string> verify_bootstrap_token(
    const std::string& token,
    const std::string& pairing_base_dir = "");

// ============================================================================
// Setup Code Resolution (Main Entry Point)
// ============================================================================

// Resolve pairing setup from config and generate setup code
// Aligned with OpenClaw resolvePairingSetupFromConfig (setup-code.ts:367-409)
//
// Steps:
// 1. Resolve gateway URL (public/remote/tailscale/LAN)
// 2. Resolve auth method (token vs password)
// 3. Issue bootstrap token
// 4. Create payload {url, bootstrapToken}
// 5. Return resolution result
PairingSetupResolution resolve_pairing_setup_from_config(
    const ResolvePairingSetupOptions& options);

}  // namespace pairing
}  // namespace hunterclaw
