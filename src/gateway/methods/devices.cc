// ============================================================================
// devices.cc - Device Pairing & Token Management
// 🎯 严格对齐 OpenClaw server-methods/devices.ts (deviceHandlers)
//
// Methods:
//   device.pair.list    → listDevicePairing() + redact tokens
//   device.pair.approve → approveDevicePairing(requestId) + broadcast
//   device.pair.reject  → rejectDevicePairing(requestId) + broadcast
//   device.pair.remove  → removePairedDevice(deviceId)
//   device.token.rotate → rotateDeviceToken({deviceId,role,scopes})
//   device.token.revoke → revokeDeviceToken({deviceId,role})
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>
#include <set>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static inline int64_t devices_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// Helper: redactPairedDevice
// 🎯 OpenClaw 对齐: devices.ts L27-35
// 隐藏敏感 token 数据，仅返回摘要
// ============================================================================
static json redact_paired_device(const json& device) {
    json redacted = device;
    // Remove raw tokens, keep only summary
    if (redacted.contains("tokens") && redacted["tokens"].is_object()) {
        json summary = json::object();
        for (auto& [role, token_data] : redacted["tokens"].items()) {
            json entry;
            entry["role"] = role;
            if (token_data.contains("scopes")) entry["scopes"] = token_data["scopes"];
            if (token_data.contains("createdAtMs")) entry["createdAtMs"] = token_data["createdAtMs"];
            if (token_data.contains("rotatedAtMs")) entry["rotatedAtMs"] = token_data["rotatedAtMs"];
            // Hide the actual token value
            summary[role] = entry;
        }
        redacted["tokens"] = summary;
    }
    // Remove approvedScopes (internal detail)
    redacted.erase("approvedScopes");
    return redacted;
}

// ============================================================================
// Helper: resolveMissingRequestedScope
// 🎯 OpenClaw 对齐: devices.ts L37-54
// 检查调用者是否拥有请求的所有 scope
// ============================================================================
static std::string resolve_missing_requested_scope(
    const std::string& role,
    const std::vector<std::string>& requested_scopes,
    const std::vector<std::string>& caller_scopes
) {
    std::set<std::string> allowed(caller_scopes.begin(), caller_scopes.end());
    // If caller has wildcard scope, allow everything
    if (allowed.count("*") > 0) return "";

    for (const auto& scope : requested_scopes) {
        if (allowed.find(scope) == allowed.end()) {
            return scope;
        }
    }
    return "";
}

// ============================================================================
// device.pair.list
// 🎯 OpenClaw 对齐: devices.ts L57-80
// ============================================================================
core::Result<json> method_device_pair_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;

    auto cfg = server->config();
    if (!cfg) {
        return core::Result<json>::Err("gateway not configured");
    }

    // Load device pairing data from config dir
    std::string pairing_file = cfg->config_dir + "/device-pairing.json";
    json pairing_data;
    try {
        if (std::filesystem::exists(pairing_file)) {
            std::ifstream f(pairing_file);
            if (f.is_open()) f >> pairing_data;
        }
    } catch (...) {
        pairing_data = json::object();
    }

    json result;
    result["pending"] = pairing_data.value("pending", json::array());

    // Redact paired devices
    json paired_redacted = json::array();
    auto paired = pairing_data.value("paired", json::array());
    for (const auto& device : paired) {
        paired_redacted.push_back(redact_paired_device(device));
    }
    result["paired"] = paired_redacted;

    return core::Result<json>::Ok(result);
}

// ============================================================================
// device.pair.approve
// 🎯 OpenClaw 对齐: devices.ts L81-115
// ============================================================================
core::Result<json> method_device_pair_approve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;

    if (!params.contains("requestId") || !params["requestId"].is_string()) {
        return core::Result<json>::Err("invalid device.pair.approve params: requestId required");
    }
    std::string request_id = params["requestId"].get<std::string>();

    auto cfg = server->config();
    if (!cfg) {
        return core::Result<json>::Err("gateway not configured");
    }

    std::string pairing_file = cfg->config_dir + "/device-pairing.json";
    json pairing_data;
    try {
        std::ifstream f(pairing_file);
        if (f.is_open()) f >> pairing_data;
    } catch (...) {
        return core::Result<json>::Err("unknown requestId");
    }

    // Find and approve the pending request
    auto& pending = pairing_data["pending"];
    if (!pending.is_array()) {
        return core::Result<json>::Err("unknown requestId");
    }

    json approved_device;
    bool found = false;
    json new_pending = json::array();
    for (auto& req : pending) {
        if (req.value("requestId", "") == request_id) {
            found = true;
            approved_device = req;
            approved_device["status"] = "approved";
            approved_device["approvedAtMs"] = devices_now_ms();
        } else {
            new_pending.push_back(req);
        }
    }

    if (!found) {
        return core::Result<json>::Err("unknown requestId");
    }

    pairing_data["pending"] = new_pending;
    if (!pairing_data.contains("paired") || !pairing_data["paired"].is_array()) {
        pairing_data["paired"] = json::array();
    }
    pairing_data["paired"].push_back(approved_device);

    // Save
    try {
        std::ofstream out(pairing_file);
        if (out.is_open()) out << pairing_data.dump(2);
    } catch (...) {}

    // Broadcast device.pair.resolved event
    // 🎯 OpenClaw 对齐: context.broadcast("device.pair.resolved", ...)
    json broadcast_payload;
    broadcast_payload["requestId"] = request_id;
    broadcast_payload["deviceId"] = approved_device.value("deviceId", "");
    broadcast_payload["decision"] = "approved";
    broadcast_payload["ts"] = devices_now_ms();
    server->broadcast("device.pair.resolved", broadcast_payload);

    spdlog::info("device pairing approved device={} role={}",
        approved_device.value("deviceId", ""),
        approved_device.value("role", "unknown"));

    json result;
    result["requestId"] = request_id;
    result["device"] = redact_paired_device(approved_device);
    return core::Result<json>::Ok(result);
}

// ============================================================================
// device.pair.reject
// 🎯 OpenClaw 对齐: devices.ts L116-147
// ============================================================================
core::Result<json> method_device_pair_reject(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    if (!params.contains("requestId") || !params["requestId"].is_string()) {
        return core::Result<json>::Err("invalid device.pair.reject params: requestId required");
    }
    std::string request_id = params["requestId"].get<std::string>();

    auto cfg = server->config();
    if (!cfg) {
        return core::Result<json>::Err("gateway not configured");
    }

    std::string pairing_file = cfg->config_dir + "/device-pairing.json";
    json pairing_data;
    try {
        std::ifstream f(pairing_file);
        if (f.is_open()) f >> pairing_data;
    } catch (...) {
        return core::Result<json>::Err("unknown requestId");
    }

    auto& pending = pairing_data["pending"];
    if (!pending.is_array()) {
        return core::Result<json>::Err("unknown requestId");
    }

    json rejected_entry;
    bool found = false;
    json new_pending = json::array();
    for (auto& req : pending) {
        if (req.value("requestId", "") == request_id) {
            found = true;
            rejected_entry = req;
        } else {
            new_pending.push_back(req);
        }
    }

    if (!found) {
        return core::Result<json>::Err("unknown requestId");
    }

    pairing_data["pending"] = new_pending;

    try {
        std::ofstream out(pairing_file);
        if (out.is_open()) out << pairing_data.dump(2);
    } catch (...) {}

    // Broadcast
    json broadcast_payload;
    broadcast_payload["requestId"] = request_id;
    broadcast_payload["deviceId"] = rejected_entry.value("deviceId", "");
    broadcast_payload["decision"] = "rejected";
    broadcast_payload["ts"] = devices_now_ms();
    server->broadcast("device.pair.resolved", broadcast_payload);

    return core::Result<json>::Ok(rejected_entry);
}

// ============================================================================
// device.pair.remove
// 🎯 OpenClaw 对齐: devices.ts L148-170
// ============================================================================
core::Result<json> method_device_pair_remove(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    if (!params.contains("deviceId") || !params["deviceId"].is_string()) {
        return core::Result<json>::Err("invalid device.pair.remove params: deviceId required");
    }
    std::string device_id = params["deviceId"].get<std::string>();

    auto cfg = server->config();
    if (!cfg) return core::Result<json>::Err("gateway not configured");

    std::string pairing_file = cfg->config_dir + "/device-pairing.json";
    json pairing_data;
    try {
        std::ifstream f(pairing_file);
        if (f.is_open()) f >> pairing_data;
    } catch (...) {
        return core::Result<json>::Err("unknown deviceId");
    }

    auto& paired = pairing_data["paired"];
    if (!paired.is_array()) {
        return core::Result<json>::Err("unknown deviceId");
    }

    json removed_entry;
    bool found = false;
    json new_paired = json::array();
    for (auto& dev : paired) {
        if (dev.value("deviceId", "") == device_id) {
            found = true;
            removed_entry = dev;
        } else {
            new_paired.push_back(dev);
        }
    }

    if (!found) {
        return core::Result<json>::Err("unknown deviceId");
    }

    pairing_data["paired"] = new_paired;
    try {
        std::ofstream out(pairing_file);
        if (out.is_open()) out << pairing_data.dump(2);
    } catch (...) {}

    spdlog::info("device pairing removed device={}", removed_entry.value("deviceId", ""));
    return core::Result<json>::Ok(removed_entry);
}

// ============================================================================
// device.token.rotate
// 🎯 OpenClaw 对齐: devices.ts L171-231
// ============================================================================
core::Result<json> method_device_token_rotate(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;

    if (!params.contains("deviceId") || !params["deviceId"].is_string() ||
        !params.contains("role") || !params["role"].is_string()) {
        return core::Result<json>::Err("invalid device.token.rotate params: deviceId and role required");
    }
    std::string device_id = params["deviceId"].get<std::string>();
    std::string role = params["role"].get<std::string>();

    // Extract optional scopes
    std::vector<std::string> requested_scopes;
    if (params.contains("scopes") && params["scopes"].is_array()) {
        for (const auto& s : params["scopes"]) {
            if (s.is_string()) requested_scopes.push_back(s.get<std::string>());
        }
    }

    auto cfg = server->config();
    if (!cfg) return core::Result<json>::Err("gateway not configured");

    std::string pairing_file = cfg->config_dir + "/device-pairing.json";
    json pairing_data;
    try {
        std::ifstream f(pairing_file);
        if (f.is_open()) f >> pairing_data;
    } catch (...) {
        return core::Result<json>::Err("unknown deviceId/role");
    }

    // Find device
    auto& paired = pairing_data["paired"];
    if (!paired.is_array()) return core::Result<json>::Err("unknown deviceId/role");

    json* target_device = nullptr;
    for (auto& dev : paired) {
        if (dev.value("deviceId", "") == device_id) {
            target_device = &dev;
            break;
        }
    }
    if (!target_device) return core::Result<json>::Err("unknown deviceId/role");

    // Scope check against caller scopes
    // 🎯 OpenClaw 对齐: resolveMissingRequestedScope
    auto caller_scopes = client_ctx.scopes();
    if (requested_scopes.empty()) {
        // Use existing scopes from device token
        if (target_device->contains("tokens") &&
            (*target_device)["tokens"].contains(role) &&
            (*target_device)["tokens"][role].contains("scopes")) {
            for (const auto& s : (*target_device)["tokens"][role]["scopes"]) {
                if (s.is_string()) requested_scopes.push_back(s.get<std::string>());
            }
        }
    }

    auto missing = resolve_missing_requested_scope(role, requested_scopes, caller_scopes);
    if (!missing.empty()) {
        return core::Result<json>::Err("missing scope: " + missing);
    }

    // Generate new token
    std::string new_token = "hc_tok_" + core::utils::generate_uuid();
    int64_t now = devices_now_ms();

    // Update token entry
    if (!target_device->contains("tokens") || !(*target_device)["tokens"].is_object()) {
        (*target_device)["tokens"] = json::object();
    }
    json& token_entry = (*target_device)["tokens"][role];
    token_entry["token"] = new_token;
    token_entry["role"] = role;
    token_entry["scopes"] = requested_scopes;
    token_entry["rotatedAtMs"] = now;
    if (!token_entry.contains("createdAtMs")) {
        token_entry["createdAtMs"] = now;
    }

    // Save
    try {
        std::ofstream out(pairing_file);
        if (out.is_open()) out << pairing_data.dump(2);
    } catch (...) {}

    spdlog::info("device token rotated device={} role={} scopes={}",
        device_id, role,
        [&]() {
            std::string s;
            for (size_t i = 0; i < requested_scopes.size(); i++) {
                if (i > 0) s += ",";
                s += requested_scopes[i];
            }
            return s;
        }());

    json result;
    result["deviceId"] = device_id;
    result["role"] = role;
    result["token"] = new_token;
    result["scopes"] = requested_scopes;
    result["rotatedAtMs"] = now;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// device.token.revoke
// 🎯 OpenClaw 对齐: devices.ts L232-259
// ============================================================================
core::Result<json> method_device_token_revoke(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    if (!params.contains("deviceId") || !params["deviceId"].is_string() ||
        !params.contains("role") || !params["role"].is_string()) {
        return core::Result<json>::Err("invalid device.token.revoke params: deviceId and role required");
    }
    std::string device_id = params["deviceId"].get<std::string>();
    std::string role = params["role"].get<std::string>();

    auto cfg = server->config();
    if (!cfg) return core::Result<json>::Err("gateway not configured");

    std::string pairing_file = cfg->config_dir + "/device-pairing.json";
    json pairing_data;
    try {
        std::ifstream f(pairing_file);
        if (f.is_open()) f >> pairing_data;
    } catch (...) {
        return core::Result<json>::Err("unknown deviceId/role");
    }

    auto& paired = pairing_data["paired"];
    if (!paired.is_array()) return core::Result<json>::Err("unknown deviceId/role");

    json* target_device = nullptr;
    for (auto& dev : paired) {
        if (dev.value("deviceId", "") == device_id) {
            target_device = &dev;
            break;
        }
    }
    if (!target_device) return core::Result<json>::Err("unknown deviceId/role");

    if (!target_device->contains("tokens") ||
        !(*target_device)["tokens"].contains(role)) {
        return core::Result<json>::Err("unknown deviceId/role");
    }

    int64_t now = devices_now_ms();
    (*target_device)["tokens"][role]["revokedAtMs"] = now;
    (*target_device)["tokens"][role].erase("token");

    // Save
    try {
        std::ofstream out(pairing_file);
        if (out.is_open()) out << pairing_data.dump(2);
    } catch (...) {}

    spdlog::info("device token revoked device={} role={}", device_id, role);

    json result;
    result["deviceId"] = device_id;
    result["role"] = role;
    result["revokedAtMs"] = now;
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
