// ============================================================================
// auth_profiles.cc
// 严格对齐 OpenClaw: auth-profiles/ (32 files, ~120KB total)
// 关键对齐: usage.ts (cooldown) + order.ts + profiles.ts + store.ts + display.ts
// ============================================================================

#include "hunterclaw/llm/auth_profiles.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <set>
#include <cmath>

using json = nlohmann::json;

namespace hunterclaw {
namespace llm {

// ============================================================================
// Cooldown Constants (对齐 OC: auth-profiles/usage.ts)
// ============================================================================

static constexpr int64_t BASE_COOLDOWN_MS = 30000;       // 30s
static constexpr int64_t MAX_COOLDOWN_MS = 600000;       // 10 minutes
static constexpr double COOLDOWN_BACKOFF_FACTOR = 1.5;
static constexpr int MAX_COOLDOWN_COUNT = 10;

// ============================================================================
// Cooldown Policy Implementation
// ============================================================================

int64_t calculate_auth_profile_cooldown_ms(int failure_count) {
    if (failure_count <= 0) return 0;
    int capped = std::min(failure_count, MAX_COOLDOWN_COUNT);
    auto ms = static_cast<int64_t>(
        BASE_COOLDOWN_MS * std::pow(COOLDOWN_BACKOFF_FACTOR, capped - 1));
    return std::min(ms, MAX_COOLDOWN_MS);
}

bool is_profile_in_cooldown(const AuthProfileState& profile, int64_t now_ms) {
    return profile.in_cooldown && profile.cooldown_until_ms > now_ms;
}

void mark_auth_profile_failure(
    AuthProfileState& profile,
    AuthProfileFailureReason reason,
    const std::string& message,
    int64_t now_ms
) {
    profile.cooldown_count++;
    profile.last_failure_reason = reason;
    profile.last_failure_message = message;
    profile.usage.total_failures++;

    auto cooldown_ms = calculate_auth_profile_cooldown_ms(profile.cooldown_count);
    profile.in_cooldown = true;
    profile.cooldown_until_ms = now_ms + cooldown_ms;
}

void mark_auth_profile_used(AuthProfileState& profile, int64_t now_ms) {
    profile.usage.last_used_at_ms = now_ms;
    profile.usage.total_requests++;
}

void mark_auth_profile_good(AuthProfileState& profile, int64_t now_ms) {
    profile.usage.last_good_at_ms = now_ms;
    profile.in_cooldown = false;
    profile.cooldown_until_ms = 0;
    profile.cooldown_count = 0;
    profile.last_failure_reason = std::nullopt;
    profile.last_failure_message = std::nullopt;
}

void mark_auth_profile_cooldown(
    AuthProfileState& profile,
    int64_t cooldown_ms,
    int64_t now_ms
) {
    profile.in_cooldown = true;
    profile.cooldown_until_ms = now_ms + cooldown_ms;
}

void clear_auth_profile_cooldown(AuthProfileState& profile) {
    profile.in_cooldown = false;
    profile.cooldown_until_ms = 0;
    profile.cooldown_count = 0;
}

void clear_expired_cooldowns(AuthProfileStore& store, int64_t now_ms) {
    for (auto& [_, profile] : store.profiles) {
        if (profile.in_cooldown && profile.cooldown_until_ms <= now_ms) {
            clear_auth_profile_cooldown(profile);
        }
    }
}

std::optional<int64_t> get_soonest_cooldown_expiry(
    const AuthProfileStore& store,
    const std::string& provider,
    int64_t now_ms
) {
    std::optional<int64_t> soonest;
    for (const auto& [_, profile] : store.profiles) {
        if (profile.provider != provider) continue;
        if (!profile.in_cooldown || profile.cooldown_until_ms <= now_ms) continue;
        if (!soonest || profile.cooldown_until_ms < *soonest) {
            soonest = profile.cooldown_until_ms;
        }
    }
    return soonest;
}

// ============================================================================
// Profile Order (对齐 OC: auth-profiles/order.ts)
// ============================================================================

AuthProfileEligibility resolve_auth_profile_eligibility(
    const AuthProfileState& profile,
    int64_t now_ms
) {
    if (is_profile_in_cooldown(profile, now_ms)) {
        return {false, AuthProfileEligibilityReasonCode::IN_COOLDOWN,
                profile.cooldown_until_ms};
    }
    // Check credential exists
    bool has_cred = false;
    switch (profile.credential.type) {
        case AuthCredentialType::API_KEY:
            has_cred = profile.credential.api_key.has_value() &&
                       !profile.credential.api_key->api_key.empty();
            break;
        case AuthCredentialType::OAUTH:
            has_cred = profile.credential.oauth.has_value() &&
                       !profile.credential.oauth->access_token.empty();
            break;
        case AuthCredentialType::TOKEN:
            has_cred = profile.credential.token.has_value() &&
                       !profile.credential.token->token.empty();
            break;
    }
    if (!has_cred) {
        return {false, AuthProfileEligibilityReasonCode::NO_CREDENTIAL, std::nullopt};
    }
    // Check OAuth expiry
    if (profile.credential.type == AuthCredentialType::OAUTH &&
        profile.credential.oauth) {
        if (profile.credential.oauth->expires_at_ms > 0 &&
            profile.credential.oauth->expires_at_ms <= now_ms) {
            return {false, AuthProfileEligibilityReasonCode::EXPIRED_TOKEN, std::nullopt};
        }
    }
    return {true, AuthProfileEligibilityReasonCode::ELIGIBLE, std::nullopt};
}

std::vector<std::string> resolve_auth_profile_order(
    const AuthProfileStore& store,
    const std::string& provider,
    int64_t now_ms
) {
    // Collect profiles for this provider
    std::vector<const AuthProfileState*> candidates;
    for (const auto& [_, profile] : store.profiles) {
        if (profile.provider == provider) {
            candidates.push_back(&profile);
        }
    }

    // Apply configured order if available
    if (store.profile_order) {
        std::vector<const AuthProfileState*> ordered;
        for (const auto& id : *store.profile_order) {
            for (auto* c : candidates) {
                if (c->profile_id == id) {
                    ordered.push_back(c);
                    break;
                }
            }
        }
        // Append any not in order
        for (auto* c : candidates) {
            bool found = false;
            for (auto* o : ordered) {
                if (o->profile_id == c->profile_id) { found = true; break; }
            }
            if (!found) ordered.push_back(c);
        }
        candidates = ordered;
    } else {
        // Sort by last_used_at descending (most recently used first)
        std::sort(candidates.begin(), candidates.end(),
            [](const AuthProfileState* a, const AuthProfileState* b) {
                return a->usage.last_used_at_ms > b->usage.last_used_at_ms;
            });
    }

    // Filter eligible, put cooldown profiles at end
    std::vector<std::string> eligible;
    std::vector<std::string> in_cooldown;
    for (auto* c : candidates) {
        auto elig = resolve_auth_profile_eligibility(*c, now_ms);
        if (elig.eligible) {
            eligible.push_back(c->profile_id);
        } else if (elig.reason == AuthProfileEligibilityReasonCode::IN_COOLDOWN) {
            in_cooldown.push_back(c->profile_id);
        }
    }
    // Eligible first, then cooldown (for probe slots)
    eligible.insert(eligible.end(), in_cooldown.begin(), in_cooldown.end());
    return eligible;
}

// ============================================================================
// Profile CRUD (对齐 OC: auth-profiles/profiles.ts)
// ============================================================================

std::vector<std::string> dedupe_profile_ids(const std::vector<std::string>& ids) {
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto& id : ids) {
        if (seen.insert(id).second) {
            result.push_back(id);
        }
    }
    return result;
}

std::vector<AuthProfileState> list_profiles_for_provider(
    const AuthProfileStore& store,
    const std::string& provider
) {
    std::vector<AuthProfileState> result;
    for (const auto& [_, profile] : store.profiles) {
        if (profile.provider == provider) {
            result.push_back(profile);
        }
    }
    return result;
}

void upsert_auth_profile(AuthProfileStore& store, const AuthProfileState& profile) {
    store.profiles[profile.profile_id] = profile;
    store.updated_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void set_auth_profile_order(
    AuthProfileStore& store,
    const std::string& provider,
    const std::vector<std::string>& order
) {
    (void)provider; // order applies globally in this simplified implementation
    store.profile_order = dedupe_profile_ids(order);
}

// ============================================================================
// Display (对齐 OC: auth-profiles/display.ts)
// ============================================================================

std::string resolve_auth_profile_display_label(const AuthProfileState& profile) {
    if (!profile.profile_id.empty()) {
        return profile.profile_id;
    }
    return profile.provider + " (default)";
}

std::optional<std::string> resolve_profile_unusable_until_for_display(
    const AuthProfileState& profile,
    int64_t now_ms
) {
    if (!is_profile_in_cooldown(profile, now_ms)) return std::nullopt;
    auto remaining_ms = profile.cooldown_until_ms - now_ms;
    auto remaining_s = remaining_ms / 1000;
    if (remaining_s < 60) {
        return std::to_string(remaining_s) + "s";
    }
    return std::to_string(remaining_s / 60) + "m " + std::to_string(remaining_s % 60) + "s";
}

// ============================================================================
// Doctor (对齐 OC: auth-profiles/doctor.ts)
// ============================================================================

std::string format_auth_doctor_hint(
    const AuthProfileStore& store,
    const std::string& provider
) {
    auto profiles = list_profiles_for_provider(store, provider);
    if (profiles.empty()) {
        return "No auth profiles for " + provider;
    }
    std::ostringstream ss;
    ss << provider << ": " << profiles.size() << " profile(s)";
    int eligible = 0, cooldown = 0;
    for (const auto& p : profiles) {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (is_profile_in_cooldown(p, now_ms)) cooldown++;
        else eligible++;
    }
    ss << " (" << eligible << " eligible, " << cooldown << " in cooldown)";
    return ss.str();
}

// ============================================================================
// AuthProfileManager (thread-safe wrapper)
// ============================================================================

AuthProfileManager::AuthProfileManager() = default;
AuthProfileManager::~AuthProfileManager() = default;

void AuthProfileManager::load_from_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        std::ifstream f(path);
        if (!f.is_open()) return;
        json j;
        f >> j;
        // Parse profiles
        if (j.contains("profiles") && j["profiles"].is_object()) {
            for (auto it = j["profiles"].begin(); it != j["profiles"].end(); ++it) {
                AuthProfileState profile;
                profile.profile_id = it.key();
                profile.provider = it.value().value("provider", "");
                profile.in_cooldown = it.value().value("inCooldown", false);
                profile.cooldown_until_ms = it.value().value("cooldownUntilMs", 0);
                profile.cooldown_count = it.value().value("cooldownCount", 0);
                profile.order_index = it.value().value("orderIndex", 0);
                profile.is_default = it.value().value("isDefault", false);
                // Parse credential
                if (it.value().contains("apiKey")) {
                    profile.credential.type = AuthCredentialType::API_KEY;
                    profile.credential.api_key = ApiKeyCredential{
                        it.value().value("apiKey", ""),
                        it.value().value("source", "")
                    };
                } else {
                    profile.credential.type = AuthCredentialType::API_KEY;
                }
                // Usage stats
                if (it.value().contains("usage")) {
                    const auto& u = it.value()["usage"];
                    profile.usage.last_used_at_ms = u.value("lastUsedAtMs", 0);
                    profile.usage.last_good_at_ms = u.value("lastGoodAtMs", 0);
                    profile.usage.total_requests = u.value("totalRequests", 0);
                    profile.usage.total_failures = u.value("totalFailures", 0);
                }
                store_.profiles[profile.profile_id] = profile;
            }
        }
        if (j.contains("profileOrder") && j["profileOrder"].is_array()) {
            std::vector<std::string> order;
            for (const auto& item : j["profileOrder"]) {
                if (item.is_string()) order.push_back(item.get<std::string>());
            }
            store_.profile_order = order;
        }
    } catch (...) {
        // Load failure is non-fatal
    }
}

void AuthProfileManager::save_to_file(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        json j;
        json profiles_json = json::object();
        for (const auto& [id, profile] : store_.profiles) {
            json p;
            p["provider"] = profile.provider;
            p["inCooldown"] = profile.in_cooldown;
            p["cooldownUntilMs"] = profile.cooldown_until_ms;
            p["cooldownCount"] = profile.cooldown_count;
            p["orderIndex"] = profile.order_index;
            p["isDefault"] = profile.is_default;
            if (profile.credential.api_key) {
                p["apiKey"] = profile.credential.api_key->api_key;
                p["source"] = profile.credential.api_key->source;
            }
            json u;
            u["lastUsedAtMs"] = profile.usage.last_used_at_ms;
            u["lastGoodAtMs"] = profile.usage.last_good_at_ms;
            u["totalRequests"] = profile.usage.total_requests;
            u["totalFailures"] = profile.usage.total_failures;
            p["usage"] = u;
            profiles_json[id] = p;
        }
        j["profiles"] = profiles_json;
        if (store_.profile_order) {
            j["profileOrder"] = *store_.profile_order;
        }
        std::ofstream f(path);
        if (f.is_open()) f << j.dump(2);
    } catch (...) {}
}

AuthProfileStore AuthProfileManager::get_store() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_;
}

void AuthProfileManager::set_store(const AuthProfileStore& store) {
    std::lock_guard<std::mutex> lock(mutex_);
    store_ = store;
}

std::optional<AuthProfileState> AuthProfileManager::get_next_eligible_profile(
    const std::string& provider,
    int64_t now_ms
) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto order = resolve_auth_profile_order(store_, provider, now_ms);
    for (const auto& id : order) {
        auto it = store_.profiles.find(id);
        if (it != store_.profiles.end()) {
            auto elig = resolve_auth_profile_eligibility(it->second, now_ms);
            if (elig.eligible) {
                return it->second;
            }
        }
    }
    return std::nullopt;
}

void AuthProfileManager::mark_failure(
    const std::string& profile_id,
    AuthProfileFailureReason reason,
    const std::string& message,
    int64_t now_ms
) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.profiles.find(profile_id);
    if (it != store_.profiles.end()) {
        mark_auth_profile_failure(it->second, reason, message, now_ms);
    }
}

void AuthProfileManager::mark_good(const std::string& profile_id, int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.profiles.find(profile_id);
    if (it != store_.profiles.end()) {
        mark_auth_profile_good(it->second, now_ms);
    }
}

void AuthProfileManager::upsert(const AuthProfileState& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    upsert_auth_profile(store_, profile);
}

void AuthProfileManager::clear_expired(int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_expired_cooldowns(store_, now_ms);
}

std::optional<int64_t> AuthProfileManager::soonest_cooldown(
    const std::string& provider, int64_t now_ms
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return get_soonest_cooldown_expiry(store_, provider, now_ms);
}

} // namespace llm
} // namespace hunterclaw
