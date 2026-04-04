// HunterClaw - Node Pairing System
// 严格对齐 OpenClaw src/infra/node-pairing.ts
// Purpose: Remote OpenClaw node authentication and authorization

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
// Node Pairing Metadata (node-pairing.ts:13-26)
// ============================================================================

struct NodePairingNodeMetadata {
  std::string node_id;
  std::optional<std::string> display_name;
  std::optional<std::string> platform;
  std::optional<std::string> version;
  std::optional<std::string> core_version;
  std::optional<std::string> ui_version;
  std::optional<std::string> device_family;
  std::optional<std::string> model_identifier;
  std::vector<std::string> caps;
  std::vector<std::string> commands;
  std::unordered_map<std::string, bool> permissions;
  std::optional<std::string> remote_ip;
};

// ============================================================================
// Pending Request (node-pairing.ts:28-33)
// ============================================================================

struct NodePairingPendingRequest : NodePairingNodeMetadata {
  std::string request_id;
  bool silent = false;
  bool is_repair = false;
  int64_t ts;  // Timestamp in milliseconds
};

// ============================================================================
// Paired Node (node-pairing.ts:35-41)
// ============================================================================

struct NodePairingPairedNode : NodePairingNodeMetadata {
  std::string token;  // Single token per node
  std::vector<std::string> bins;
  int64_t created_at_ms;
  int64_t approved_at_ms;
  std::optional<int64_t> last_connected_at_ms;
};

// ============================================================================
// List Result (node-pairing.ts:43-46)
// ============================================================================

struct NodePairingList {
  std::vector<NodePairingPendingRequest> pending;
  std::vector<NodePairingPairedNode> paired;
};

// ============================================================================
// Request Result (node-pairing.ts:104-111)
// ============================================================================

struct NodePairingRequestResult {
  enum class Status { PENDING };
  Status status;
  NodePairingPendingRequest request;
  bool created;
};

// ============================================================================
// Approval Result (node-pairing.ts:146-149)
// ============================================================================

struct NodePairingApprovalResult {
  std::string request_id;
  NodePairingPairedNode node;
};

// ============================================================================
// Rejection Result (node-pairing.ts:184-187)
// ============================================================================

struct NodePairingRejectionResult {
  std::string request_id;
  std::string node_id;
};

// ============================================================================
// Verification Result (node-pairing.ts:203-207)
// ============================================================================

struct NodePairingVerifyResult {
  bool ok;
  std::optional<NodePairingPairedNode> node;
};

// ============================================================================
// Constants (node-pairing.ts:53)
// ============================================================================

constexpr int64_t NODE_PAIRING_PENDING_TTL_MS = 5 * 60 * 1000;  // 5 minutes

// ============================================================================
// Public API
// ============================================================================

// List pending and paired nodes (node-pairing.ts:87-94)
NodePairingList list_node_pairing(const std::string& base_dir = "");

// Get paired node by ID (node-pairing.ts:96-102)
std::optional<NodePairingPairedNode> get_paired_node(
    const std::string& node_id,
    const std::string& base_dir = "");

// Request node pairing (node-pairing.ts:104-144)
NodePairingRequestResult request_node_pairing(
    const NodePairingNodeMetadata& metadata,
    bool silent = false,
    const std::string& base_dir = "");

// Approve node pairing request (node-pairing.ts:146-182)
std::optional<NodePairingApprovalResult> approve_node_pairing(
    const std::string& request_id,
    const std::string& base_dir = "");

// Reject node pairing request (node-pairing.ts:184-201)
std::optional<NodePairingRejectionResult> reject_node_pairing(
    const std::string& request_id,
    const std::string& base_dir = "");

// Verify node token (node-pairing.ts:203-215)
NodePairingVerifyResult verify_node_token(
    const std::string& node_id,
    const std::string& token,
    const std::string& base_dir = "");

// Update paired node metadata (node-pairing.ts:217-250)
void update_paired_node_metadata(
    const std::string& node_id,
    const NodePairingNodeMetadata& patch,
    const std::string& base_dir = "");

// Rename paired node (node-pairing.ts:252-273)
std::optional<NodePairingPairedNode> rename_paired_node(
    const std::string& node_id,
    const std::string& display_name,
    const std::string& base_dir = "");

// Unpair node (remove from paired list)
bool unpair_node(const std::string& node_id, const std::string& base_dir = "");

// ============================================================================
// Pairing Token Management
// ============================================================================

// Generate pairing token (32-byte random, Base64-encoded)
std::string generate_pairing_token();

// Verify pairing token (constant-time comparison)
bool verify_pairing_token(const std::string& provided,
                           const std::string& expected);

}  // namespace pairing
}  // namespace hunterclaw
