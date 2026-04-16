// HunterClaw - Device Pairing System
// ✅ 严格对齐 OpenClaw src/infra/device-pairing.ts
// Purpose: Mobile device/client pairing with multi-role token management,
//          fine-grained scope control, token rotation/revocation.

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
// Device Pairing Pending Request (device-pairing.ts:23-39)
// ============================================================================

struct DevicePairingPendingRequest {
  std::string request_id;
  std::string device_id;
  std::string public_key;
  std::optional<std::string> display_name;
  std::optional<std::string> platform;
  std::optional<std::string> device_family;
  std::optional<std::string> client_id;
  std::optional<std::string> client_mode;
  std::optional<std::string> role;
  std::vector<std::string> roles;
  std::vector<std::string> scopes;
  std::optional<std::string> remote_ip;
  bool silent = false;
  bool is_repair = false;
  int64_t ts = 0;
};

// ============================================================================
// Device Auth Token (device-pairing.ts:41-49)
// ============================================================================

struct DeviceAuthToken {
  std::string token;
  std::string role;
  std::vector<std::string> scopes;
  int64_t created_at_ms = 0;
  std::optional<int64_t> rotated_at_ms;
  std::optional<int64_t> revoked_at_ms;
  std::optional<int64_t> last_used_at_ms;
};

// ============================================================================
// Device Auth Token Summary (device-pairing.ts:51-58)
// ============================================================================

struct DeviceAuthTokenSummary {
  std::string role;
  std::vector<std::string> scopes;
  int64_t created_at_ms = 0;
  std::optional<int64_t> rotated_at_ms;
  std::optional<int64_t> revoked_at_ms;
  std::optional<int64_t> last_used_at_ms;
};

// ============================================================================
// Paired Device (device-pairing.ts:69-85)
// ============================================================================

struct PairedDevice {
  std::string device_id;
  std::string public_key;
  std::optional<std::string> display_name;
  std::optional<std::string> platform;
  std::optional<std::string> device_family;
  std::optional<std::string> client_id;
  std::optional<std::string> client_mode;
  std::optional<std::string> role;
  std::vector<std::string> roles;
  std::vector<std::string> scopes;
  std::vector<std::string> approved_scopes;
  std::optional<std::string> remote_ip;
  std::unordered_map<std::string, DeviceAuthToken> tokens;
  int64_t created_at_ms = 0;
  int64_t approved_at_ms = 0;
};

// ============================================================================
// Paired Device Metadata Patch (device-pairing.ts:87-90)
// ============================================================================

struct PairedDeviceMetadataPatch {
  std::optional<std::string> display_name;
  std::optional<std::string> client_id;
  std::optional<std::string> client_mode;
  std::optional<std::string> remote_ip;
};

// ============================================================================
// Device Pairing List (device-pairing.ts:92-95)
// ============================================================================

struct DevicePairingList {
  std::vector<DevicePairingPendingRequest> pending;
  std::vector<PairedDevice> paired;
};

// ============================================================================
// Forbidden Result (device-pairing.ts:97-114)
// ============================================================================

enum class DevicePairingForbiddenReason {
  CALLER_SCOPES_REQUIRED,
  CALLER_MISSING_SCOPE,
  SCOPE_OUTSIDE_REQUESTED_ROLES,
  BOOTSTRAP_ROLE_NOT_ALLOWED,
  BOOTSTRAP_SCOPE_NOT_ALLOWED
};

struct DevicePairingForbiddenResult {
  DevicePairingForbiddenReason reason;
  std::optional<std::string> scope;
  std::optional<std::string> role;
};

std::string format_device_pairing_forbidden_message(
    const DevicePairingForbiddenResult& result);

// ============================================================================
// Approve Result (device-pairing.ts:111-114)
// ============================================================================

struct ApproveDevicePairingResult {
  enum class Status { APPROVED, FORBIDDEN };
  Status status;
  // APPROVED fields
  std::string request_id;
  PairedDevice device;
  // FORBIDDEN fields
  std::optional<DevicePairingForbiddenResult> forbidden;
};

// ============================================================================
// Rotate Token Result (device-pairing.ts:60-67)
// ============================================================================

enum class RotateDeviceTokenDenyReason {
  UNKNOWN_DEVICE_OR_ROLE,
  MISSING_APPROVED_SCOPE_BASELINE,
  SCOPE_OUTSIDE_APPROVED_BASELINE
};

struct RotateDeviceTokenResult {
  bool ok;
  std::optional<DeviceAuthToken> entry;
  std::optional<RotateDeviceTokenDenyReason> reason;
};

// ============================================================================
// Constants (device-pairing.ts:121-123)
// ============================================================================

constexpr int64_t DEVICE_PAIRING_PENDING_TTL_MS = 5 * 60 * 1000;  // 5 minutes
constexpr const char* OPERATOR_ROLE = "operator";
constexpr const char* OPERATOR_SCOPE_PREFIX = "operator.";

// ============================================================================
// Public API — 严格对齐 OpenClaw device-pairing.ts 全部导出函数
// ============================================================================

// ✅ device-pairing.ts:451 listDevicePairing
DevicePairingList list_device_pairing(const std::string& base_dir = "");

// ✅ device-pairing.ts:460 getPairedDevice
std::optional<PairedDevice> get_paired_device(
    const std::string& device_id,
    const std::string& base_dir = "");

// ✅ device-pairing.ts:468 getPendingDevicePairing
std::optional<DevicePairingPendingRequest> get_pending_device_pairing(
    const std::string& request_id,
    const std::string& base_dir = "");

// ✅ device-pairing.ts:476 requestDevicePairing
struct RequestDevicePairingResult {
  std::string status;  // "pending"
  DevicePairingPendingRequest request;
  bool created;
};

RequestDevicePairingResult request_device_pairing(
    const DevicePairingPendingRequest& req,
    const std::string& base_dir = "");

// ✅ device-pairing.ts:534 approveDevicePairing
std::optional<ApproveDevicePairingResult> approve_device_pairing(
    const std::string& request_id,
    const std::vector<std::string>& caller_scopes = {},
    const std::string& base_dir = "");

// ✅ device-pairing.ts:644 approveBootstrapDevicePairing
struct DeviceBootstrapProfile {
  std::vector<std::string> roles;
  std::vector<std::string> scopes;
};

std::optional<ApproveDevicePairingResult> approve_bootstrap_device_pairing(
    const std::string& request_id,
    const DeviceBootstrapProfile& profile,
    const std::string& base_dir = "");

// ✅ device-pairing.ts:731 rejectDevicePairing
struct RejectDevicePairingResult {
  std::string request_id;
  std::string device_id;
};

std::optional<RejectDevicePairingResult> reject_device_pairing(
    const std::string& request_id,
    const std::string& base_dir = "");

// ✅ device-pairing.ts:750 removePairedDevice
std::optional<std::string> remove_paired_device(
    const std::string& device_id,
    const std::string& base_dir = "");

// ✅ device-pairing.ts:766 updatePairedDeviceMetadata
void update_paired_device_metadata(
    const std::string& device_id,
    const PairedDeviceMetadataPatch& patch,
    const std::string& base_dir = "");

// ✅ device-pairing.ts:796 summarizeDeviceTokens
std::optional<std::vector<DeviceAuthTokenSummary>> summarize_device_tokens(
    const std::unordered_map<std::string, DeviceAuthToken>& tokens);

// ✅ device-pairing.ts:815 verifyDeviceToken
struct VerifyDeviceTokenResult {
  bool ok;
  std::string reason;
};

VerifyDeviceTokenResult verify_device_token(
    const std::string& device_id,
    const std::string& token,
    const std::string& role,
    const std::vector<std::string>& scopes,
    const std::string& base_dir = "");

// ✅ device-pairing.ts:865 ensureDeviceToken
std::optional<DeviceAuthToken> ensure_device_token(
    const std::string& device_id,
    const std::string& role,
    const std::vector<std::string>& scopes,
    const std::string& base_dir = "");

// ✅ device-pairing.ts:950 rotateDeviceToken
RotateDeviceTokenResult rotate_device_token(
    const std::string& device_id,
    const std::string& role,
    const std::vector<std::string>& scopes = {},
    const std::string& base_dir = "");

// ✅ device-pairing.ts:999 revokeDeviceToken
std::optional<DeviceAuthToken> revoke_device_token(
    const std::string& device_id,
    const std::string& role,
    const std::string& base_dir = "");

// ✅ device-pairing.ts:1027 clearDevicePairing
bool clear_device_pairing(
    const std::string& device_id,
    const std::string& base_dir = "");

// ============================================================================
// Role & Scope Helpers (device-pairing.ts:213-245)
// ============================================================================

// ✅ device-pairing.ts:213 listApprovedPairedDeviceRoles
std::vector<std::string> list_approved_paired_device_roles(const PairedDevice& device);

// ✅ device-pairing.ts:221 listEffectivePairedDeviceRoles
std::vector<std::string> list_effective_paired_device_roles(const PairedDevice& device);

// ✅ device-pairing.ts:236 hasEffectivePairedDeviceRole
bool has_effective_paired_device_role(const PairedDevice& device, const std::string& role);

}  // namespace pairing
}  // namespace hunterclaw
