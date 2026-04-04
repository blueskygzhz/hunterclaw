// HunterClaw - Pairing System
// 严格对齐 OpenClaw pairing/
// Purpose: DM pairing challenges, setup codes, pairing store, allow-from management

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
// Pairing Configuration
// ============================================================================

constexpr size_t PAIRING_CODE_LENGTH = 8;
constexpr const char* PAIRING_CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr int64_t PAIRING_PENDING_TTL_MS = 60 * 60 * 1000;  // 1 hour
constexpr size_t PAIRING_PENDING_MAX = 3;

// ============================================================================
// Pairing Request
// ============================================================================

struct PairingRequest {
  std::string id;
  std::string code;
  std::string created_at;      // ISO timestamp
  std::string last_seen_at;    // ISO timestamp
  std::unordered_map<std::string, std::string> meta;
};

// ============================================================================
// Pairing Challenge
// ============================================================================

struct PairingChallengeParams {
  std::string channel;
  std::string sender_id;
  std::string sender_id_line;  // Human-readable sender identifier
  std::unordered_map<std::string, std::string> meta;

  // Callbacks
  std::function<std::pair<std::string, bool>(const std::string& id,
                                             const std::unordered_map<std::string, std::string>&)>
      upsert_pairing_request;  // Returns (code, created)
  std::function<void(const std::string&)> send_pairing_reply;
  std::function<std::string(const std::string&, const std::string&)> build_reply_text;  // Optional
  std::function<void(const std::string&)> on_created;                                    // Optional
  std::function<void(const std::string&)> on_reply_error;                                // Optional
};

struct PairingChallengeResult {
  bool created;
  std::optional<std::string> code;
};

// Issue pairing challenge for DM pairing policy pathways
PairingChallengeResult issue_pairing_challenge(const PairingChallengeParams& params);

// ============================================================================
// Setup Code Generation
// ============================================================================

std::string generate_setup_code();
std::string generate_unique_pairing_code(const std::vector<std::string>& existing_codes);

// ============================================================================
// Pairing Messages
// ============================================================================

struct BuildPairingReplyParams {
  std::string channel;
  std::string id_line;
  std::string code;
};

std::string build_pairing_reply(const BuildPairingReplyParams& params);

// ============================================================================
// Pairing Setup (DEPRECATED - Use setup_code.h)
// ============================================================================
// NOTE: Setup code functionality has moved to hunterclaw/pairing/setup_code.h
// These declarations are kept for backward compatibility.
// For new code, #include "hunterclaw/pairing/setup_code.h" instead.

// ============================================================================
// Pairing Store
// ============================================================================

class PairingStore {
 public:
  PairingStore();
  explicit PairingStore(const std::string& channel);
  ~PairingStore();

  // Request management
  std::pair<std::string, bool> upsert_request(
      const std::string& id, const std::unordered_map<std::string, std::string>& meta);
  std::optional<PairingRequest> get_request(const std::string& code) const;
  std::vector<PairingRequest> list_requests() const;
  bool remove_request(const std::string& code);
  void prune_expired_requests();

  // Allow-from management
  void add_allow_from(const std::string& entry);
  void remove_allow_from(const std::string& entry);
  std::vector<std::string> list_allow_from() const;
  bool is_allowed(const std::string& entry) const;
  void clear_allow_from();

  // Account-scoped allow-from
  void add_allow_from(const std::string& account_id, const std::string& entry);
  void remove_allow_from(const std::string& account_id, const std::string& entry);
  std::vector<std::string> list_allow_from(const std::string& account_id) const;
  bool is_allowed(const std::string& account_id, const std::string& entry) const;

  // Persistence
  void save();
  void load();

  // Channel
  std::string get_channel() const;
  void set_channel(const std::string& channel);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Pairing Adapter Interface
// ============================================================================

class PairingAdapter {
 public:
  virtual ~PairingAdapter() = default;

  // Pairing challenge
  virtual PairingChallengeResult issue_challenge(const PairingChallengeParams& params) = 0;

  // Allow-from management
  virtual bool is_allowed(const std::string& sender_id) const = 0;
  virtual void add_allowed(const std::string& sender_id) = 0;
  virtual void remove_allowed(const std::string& sender_id) = 0;

  // Setup
  virtual PairingSetupResolution resolve_setup(
      const ResolvePairingSetupOptions& options) const = 0;
};

// ============================================================================
// Default Pairing Adapter
// ============================================================================

class DefaultPairingAdapter : public PairingAdapter {
 public:
  explicit DefaultPairingAdapter(const std::string& channel);
  ~DefaultPairingAdapter() override;

  PairingChallengeResult issue_challenge(const PairingChallengeParams& params) override;
  bool is_allowed(const std::string& sender_id) const override;
  void add_allowed(const std::string& sender_id) override;
  void remove_allowed(const std::string& sender_id) override;
  PairingSetupResolution resolve_setup(
      const ResolvePairingSetupOptions& options) const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Pairing Policy
// ============================================================================

// ✅ Aligned with openclaw: DM Policy = {open, pairing, allowlist, disabled}
// HunterClaw maps: OPEN→open, CHALLENGE→pairing, ALLOWLIST→allowlist, CLOSED→disabled
enum class PairingPolicy {
  OPEN,      // No pairing required (dmPolicy=open)
  CHALLENGE, // Challenge-based pairing (dmPolicy=pairing, DEFAULT)
  ALLOWLIST, // Only configured allowlist (dmPolicy=allowlist)
  CLOSED     // No pairing allowed (dmPolicy=disabled)
};

std::string pairing_policy_to_string(PairingPolicy policy);
PairingPolicy parse_pairing_policy(const std::string& str);

}  // namespace pairing
}  // namespace hunterclaw
