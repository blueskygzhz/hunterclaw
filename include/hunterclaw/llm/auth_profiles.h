#pragma once
// ============================================================================
// auth_profiles.h
// 严格对齐 OpenClaw: auth-profiles/ (32 files, ~120KB total)
// 对齐文件: auth-profiles.ts (入口) + types.ts + constants.ts + usage.ts +
//          store.ts + order.ts + profiles.ts + display.ts + doctor.ts +
//          paths.ts + policy.ts + state.ts + oauth.ts + repair.ts + ...
// ============================================================================

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <chrono>
#include <mutex>
#include <memory>
#include <functional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace llm {

// ============================================================================
// Constants (对齐 OC: auth-profiles/constants.ts)
// ============================================================================

constexpr const char* CLAUDE_CLI_PROFILE_ID = "claude-cli";
constexpr const char* CODEX_CLI_PROFILE_ID = "codex-cli";

// ============================================================================
// Types (对齐 OC: auth-profiles/types.ts)
// ============================================================================

enum class AuthCredentialType { API_KEY, OAUTH, TOKEN };

struct ApiKeyCredential {
    std::string api_key;
    std::string source;  // e.g., "env: ANTHROPIC_API_KEY"
};

struct OAuthCredential {
    std::string access_token;
    std::string refresh_token;
    int64_t expires_at_ms = 0;
    std::string token_type = "bearer";
};

struct TokenCredential {
    std::string token;
    std::string source;
};

struct AuthProfileCredential {
    AuthCredentialType type;
    std::optional<ApiKeyCredential> api_key;
    std::optional<OAuthCredential> oauth;
    std::optional<TokenCredential> token;
};

enum class AuthProfileFailureReason {
    RATE_LIMIT, OVERLOADED, AUTH_ERROR, AUTH_PERMANENT,
    BILLING, NETWORK_ERROR, SERVER_ERROR, UNKNOWN
};

struct ProfileUsageStats {
    int64_t last_used_at_ms = 0;
    int64_t last_good_at_ms = 0;
    int64_t total_requests = 0;
    int64_t total_failures = 0;
};

struct AuthProfileState {
    std::string profile_id;
    std::string provider;
    AuthProfileCredential credential;
    ProfileUsageStats usage;

    // Cooldown state
    bool in_cooldown = false;
    int64_t cooldown_until_ms = 0;
    int cooldown_count = 0;
    std::optional<AuthProfileFailureReason> last_failure_reason;
    std::optional<std::string> last_failure_message;

    // Order preference
    int order_index = 0;
    bool is_default = false;
};

struct AuthProfileStore {
    std::map<std::string, AuthProfileState> profiles;  // key: profile_id
    std::optional<std::vector<std::string>> profile_order;
    int64_t updated_at_ms = 0;
};

// ============================================================================
// Cooldown Policy (对齐 OC: auth-profiles/usage.ts)
// ============================================================================

/// Calculate cooldown duration for a given failure count
/// 对齐 OC: calculateAuthProfileCooldownMs
int64_t calculate_auth_profile_cooldown_ms(int failure_count);

/// Check if a profile is currently in cooldown
/// 对齐 OC: isProfileInCooldown
bool is_profile_in_cooldown(const AuthProfileState& profile, int64_t now_ms);

/// Mark a profile as having failed, applying cooldown
/// 对齐 OC: markAuthProfileFailure
void mark_auth_profile_failure(
    AuthProfileState& profile,
    AuthProfileFailureReason reason,
    const std::string& message,
    int64_t now_ms);

/// Mark a profile as successfully used
/// 对齐 OC: markAuthProfileUsed
void mark_auth_profile_used(AuthProfileState& profile, int64_t now_ms);

/// Mark a profile as good (reset cooldown)
/// 对齐 OC: markAuthProfileGood
void mark_auth_profile_good(AuthProfileState& profile, int64_t now_ms);

/// Mark a profile with explicit cooldown
/// 对齐 OC: markAuthProfileCooldown
void mark_auth_profile_cooldown(
    AuthProfileState& profile,
    int64_t cooldown_ms,
    int64_t now_ms);

/// Clear cooldown for a profile
/// 对齐 OC: clearAuthProfileCooldown
void clear_auth_profile_cooldown(AuthProfileState& profile);

/// Clear expired cooldowns in the store
/// 对齐 OC: clearExpiredCooldowns
void clear_expired_cooldowns(AuthProfileStore& store, int64_t now_ms);

/// Get the soonest cooldown expiry across all profiles
/// 对齐 OC: getSoonestCooldownExpiry
std::optional<int64_t> get_soonest_cooldown_expiry(
    const AuthProfileStore& store,
    const std::string& provider,
    int64_t now_ms);

// ============================================================================
// Profile Order (对齐 OC: auth-profiles/order.ts)
// ============================================================================

enum class AuthProfileEligibilityReasonCode {
    ELIGIBLE, IN_COOLDOWN, NO_CREDENTIAL, EXPIRED_TOKEN, REVOKED
};

struct AuthProfileEligibility {
    bool eligible = false;
    AuthProfileEligibilityReasonCode reason = AuthProfileEligibilityReasonCode::ELIGIBLE;
    std::optional<int64_t> cooldown_until_ms;
};

/// Check if a profile is eligible for use
/// 对齐 OC: resolveAuthProfileEligibility
AuthProfileEligibility resolve_auth_profile_eligibility(
    const AuthProfileState& profile,
    int64_t now_ms);

/// Resolve ordered list of profile IDs for a provider
/// 对齐 OC: resolveAuthProfileOrder
std::vector<std::string> resolve_auth_profile_order(
    const AuthProfileStore& store,
    const std::string& provider,
    int64_t now_ms);

// ============================================================================
// Profile CRUD (对齐 OC: auth-profiles/profiles.ts)
// ============================================================================

/// Deduplicate profile IDs
std::vector<std::string> dedupe_profile_ids(const std::vector<std::string>& ids);

/// List profiles for a specific provider
std::vector<AuthProfileState> list_profiles_for_provider(
    const AuthProfileStore& store,
    const std::string& provider);

/// Upsert (insert or update) a profile
void upsert_auth_profile(AuthProfileStore& store, const AuthProfileState& profile);

/// Set profile ordering for a provider
void set_auth_profile_order(
    AuthProfileStore& store,
    const std::string& provider,
    const std::vector<std::string>& order);

// ============================================================================
// Display (对齐 OC: auth-profiles/display.ts)
// ============================================================================

/// Resolve a human-readable display label for a profile
std::string resolve_auth_profile_display_label(const AuthProfileState& profile);

/// Resolve display text for when a profile is unusable
std::optional<std::string> resolve_profile_unusable_until_for_display(
    const AuthProfileState& profile,
    int64_t now_ms);

// ============================================================================
// Doctor (对齐 OC: auth-profiles/doctor.ts)
// ============================================================================

/// Format a diagnostic hint for auth profiles
std::string format_auth_doctor_hint(
    const AuthProfileStore& store,
    const std::string& provider);

// ============================================================================
// AuthProfileManager — thread-safe wrapper
// ============================================================================

class AuthProfileManager {
public:
    AuthProfileManager();
    ~AuthProfileManager();

    /// Load store from disk
    void load_from_file(const std::string& path);

    /// Save store to disk
    void save_to_file(const std::string& path) const;

    /// Get a copy of the current store
    AuthProfileStore get_store() const;

    /// Replace the entire store
    void set_store(const AuthProfileStore& store);

    /// Get the next eligible profile for a provider (round-robin with cooldown)
    /// 对齐 OC: resolveApiKeyForProfile + resolveAuthProfileOrder
    std::optional<AuthProfileState> get_next_eligible_profile(
        const std::string& provider,
        int64_t now_ms);

    /// Mark a profile as failed
    void mark_failure(
        const std::string& profile_id,
        AuthProfileFailureReason reason,
        const std::string& message,
        int64_t now_ms);

    /// Mark a profile as good
    void mark_good(const std::string& profile_id, int64_t now_ms);

    /// Upsert a profile
    void upsert(const AuthProfileState& profile);

    /// Clear expired cooldowns
    void clear_expired(int64_t now_ms);

    /// Get soonest cooldown expiry for a provider
    std::optional<int64_t> soonest_cooldown(
        const std::string& provider, int64_t now_ms) const;

private:
    mutable std::mutex mutex_;
    AuthProfileStore store_;
};

} // namespace llm
} // namespace hunterclaw
