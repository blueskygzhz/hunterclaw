// ============================================================================
// model_fallback.cc
// 严格对齐 OpenClaw: model-fallback.ts (962行) + failover-error.ts + failover-policy.ts
// ============================================================================

#include "hunterclaw/llm/model_fallback.h"
#include <algorithm>
#include <sstream>
#include <regex>

namespace hunterclaw {
namespace llm {

namespace {

std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

} // namespace

// ============================================================================
// FailoverReason
// ============================================================================

std::string failover_reason_to_string(FailoverReason reason) {
    switch (reason) {
        case FailoverReason::Unknown:        return "unknown";
        case FailoverReason::RateLimit:      return "rate_limit";
        case FailoverReason::Overloaded:     return "overloaded";
        case FailoverReason::Auth:           return "auth";
        case FailoverReason::AuthPermanent:  return "auth_permanent";
        case FailoverReason::Billing:        return "billing";
        case FailoverReason::ModelNotFound:  return "model_not_found";
        case FailoverReason::ContextOverflow:return "context_overflow";
        case FailoverReason::Timeout:        return "timeout";
        case FailoverReason::NetworkError:   return "network_error";
        case FailoverReason::ServerError:    return "server_error";
    }
    return "unknown";
}

FailoverReason failover_reason_from_string(const std::string& s) {
    auto lower = to_lower(s);
    if (lower == "rate_limit" || lower == "rate-limit") return FailoverReason::RateLimit;
    if (lower == "overloaded") return FailoverReason::Overloaded;
    if (lower == "auth")           return FailoverReason::Auth;
    if (lower == "auth_permanent") return FailoverReason::AuthPermanent;
    if (lower == "billing")        return FailoverReason::Billing;
    if (lower == "model_not_found") return FailoverReason::ModelNotFound;
    if (lower == "context_overflow") return FailoverReason::ContextOverflow;
    if (lower == "timeout")        return FailoverReason::Timeout;
    if (lower == "network_error")  return FailoverReason::NetworkError;
    if (lower == "server_error")   return FailoverReason::ServerError;
    return FailoverReason::Unknown;
}

// ============================================================================
// FailoverError
// ============================================================================

FailoverError::FailoverError(
    const std::string& message,
    FailoverReason reason,
    const std::string& provider,
    const std::string& model,
    int status,
    const std::string& code
) : std::runtime_error(message),
    reason_(reason), provider_(provider), model_(model),
    status_(status), code_(code) {}

bool is_failover_error(const std::exception& e) {
    return dynamic_cast<const FailoverError*>(&e) != nullptr;
}

std::optional<FailoverError> coerce_to_failover_error(
    const std::exception& e,
    const std::string& provider,
    const std::string& model
) {
    // 对齐 OC: coerceToFailoverError — detect rate-limit/overloaded from HTTP status/message
    std::string msg = e.what();
    auto lower_msg = to_lower(msg);
    
    // HTTP 429 → rate_limit
    if (lower_msg.find("429") != std::string::npos ||
        lower_msg.find("rate limit") != std::string::npos ||
        lower_msg.find("rate_limit") != std::string::npos ||
        lower_msg.find("too many requests") != std::string::npos) {
        return FailoverError(msg, FailoverReason::RateLimit, provider, model, 429);
    }
    // HTTP 529/503 → overloaded
    if (lower_msg.find("529") != std::string::npos ||
        lower_msg.find("overloaded") != std::string::npos ||
        lower_msg.find("503") != std::string::npos ||
        lower_msg.find("service unavailable") != std::string::npos) {
        return FailoverError(msg, FailoverReason::Overloaded, provider, model, 503);
    }
    // HTTP 401/403 → auth
    if (lower_msg.find("401") != std::string::npos ||
        lower_msg.find("unauthorized") != std::string::npos ||
        lower_msg.find("403") != std::string::npos ||
        lower_msg.find("forbidden") != std::string::npos) {
        return FailoverError(msg, FailoverReason::Auth, provider, model, 401);
    }
    // HTTP 402 → billing
    if (lower_msg.find("402") != std::string::npos ||
        lower_msg.find("payment") != std::string::npos ||
        lower_msg.find("billing") != std::string::npos ||
        lower_msg.find("insufficient") != std::string::npos) {
        return FailoverError(msg, FailoverReason::Billing, provider, model, 402);
    }
    // HTTP 404 → model_not_found
    if (lower_msg.find("404") != std::string::npos ||
        lower_msg.find("not found") != std::string::npos ||
        lower_msg.find("model not found") != std::string::npos) {
        return FailoverError(msg, FailoverReason::ModelNotFound, provider, model, 404);
    }
    // HTTP 500 → server_error
    if (lower_msg.find("500") != std::string::npos ||
        lower_msg.find("internal server error") != std::string::npos) {
        return FailoverError(msg, FailoverReason::ServerError, provider, model, 500);
    }
    // Timeout
    if (lower_msg.find("timeout") != std::string::npos ||
        lower_msg.find("timed out") != std::string::npos) {
        return FailoverError(msg, FailoverReason::Timeout, provider, model);
    }
    // Network
    if (lower_msg.find("econnrefused") != std::string::npos ||
        lower_msg.find("enotfound") != std::string::npos ||
        lower_msg.find("connection") != std::string::npos) {
        return FailoverError(msg, FailoverReason::NetworkError, provider, model);
    }
    
    return std::nullopt;
}

DescribedFailoverError describe_failover_error(const std::exception& e) {
    const auto* fe = dynamic_cast<const FailoverError*>(&e);
    if (fe) {
        return DescribedFailoverError{
            fe->what(),
            fe->reason(),
            fe->status() != 0 ? std::optional(fe->status()) : std::nullopt,
            fe->code().empty() ? std::nullopt : std::optional(fe->code())
        };
    }
    return DescribedFailoverError{e.what(), FailoverReason::Unknown, std::nullopt, std::nullopt};
}

// ============================================================================
// FallbackSummaryError
// ============================================================================

FallbackSummaryError::FallbackSummaryError(
    const std::string& message,
    std::vector<FallbackAttempt> attempts,
    std::optional<int64_t> soonest_cooldown_expiry
) : std::runtime_error(message),
    attempts_(std::move(attempts)),
    soonest_cooldown_expiry_(soonest_cooldown_expiry) {}

bool is_fallback_summary_error(const std::exception& e) {
    return dynamic_cast<const FallbackSummaryError*>(&e) != nullptr;
}

// ============================================================================
// Fallback Policy (对齐 failover-policy.ts)
// ============================================================================

bool should_allow_cooldown_probe_for_reason(FailoverReason reason) {
    return reason == FailoverReason::RateLimit ||
           reason == FailoverReason::Overloaded ||
           reason == FailoverReason::Billing ||
           reason == FailoverReason::Unknown;
}

bool should_use_transient_cooldown_probe_slot(FailoverReason reason) {
    return reason == FailoverReason::RateLimit ||
           reason == FailoverReason::Overloaded ||
           reason == FailoverReason::Unknown;
}

bool should_preserve_transient_cooldown_probe_slot(FailoverReason reason) {
    return reason == FailoverReason::Auth ||
           reason == FailoverReason::AuthPermanent ||
           reason == FailoverReason::Billing;
}

// ============================================================================
// Fallback Candidates (对齐 model-fallback.ts:374-449)
// ============================================================================

std::vector<ModelCandidate> resolve_fallback_candidates(
    const core::Config& cfg,
    const std::string& provider,
    const std::string& model,
    const std::vector<std::string>& fallbacks_override
) {
    // 对齐 OC: resolveFallbackCandidates
    auto primary = resolve_configured_model_ref(cfg, DEFAULT_PROVIDER, DEFAULT_MODEL);
    auto default_provider = primary.provider.empty() ? DEFAULT_PROVIDER : primary.provider;
    auto default_model = primary.model.empty() ? DEFAULT_MODEL : primary.model;
    
    auto provider_raw = provider.empty() ? default_provider : provider;
    auto model_raw = model.empty() ? default_model : model;
    auto normalized_primary = normalize_model_ref(provider_raw, model_raw);
    
    std::set<std::string> seen;
    std::vector<ModelCandidate> candidates;
    
    auto add_candidate = [&](const ModelCandidate& c) {
        if (c.provider.empty() || c.model.empty()) return;
        auto key = model_key(c.provider, c.model);
        if (seen.count(key)) return;
        seen.insert(key);
        candidates.push_back(c);
    };
    
    // Add primary candidate
    add_candidate({normalized_primary.provider, normalized_primary.model});
    
    // Add configured fallbacks
    std::vector<std::string> fallbacks;
    if (!fallbacks_override.empty()) {
        fallbacks = fallbacks_override;
    } else {
        // Read agents.defaults.model.fallbacks from config
        if (cfg.data.contains("agents")) {
            const auto& agents = cfg.data["agents"];
            if (agents.contains("defaults")) {
                const auto& defaults = agents["defaults"];
                if (defaults.contains("model")) {
                    const auto& model_cfg = defaults["model"];
                    if (model_cfg.is_object() && model_cfg.contains("fallbacks")) {
                        const auto& fb = model_cfg["fallbacks"];
                        if (fb.is_array()) {
                            for (const auto& item : fb) {
                                if (item.is_string()) {
                                    fallbacks.push_back(item.get<std::string>());
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    for (const auto& raw : fallbacks) {
        auto resolved = resolve_model_ref_from_string(raw, default_provider);
        if (resolved) {
            add_candidate({resolved->ref.provider, resolved->ref.model});
        }
    }
    
    // Add configured primary as last resort
    if (!primary.provider.empty() && !primary.model.empty()) {
        add_candidate({primary.provider, primary.model});
    }
    
    return candidates;
}

// ============================================================================
// Cooldown Probe State (对齐 model-fallback.ts:451-498)
// ============================================================================

CooldownProbeState& CooldownProbeState::instance() {
    static CooldownProbeState instance;
    return instance;
}

bool CooldownProbeState::is_probe_throttle_open(const std::string& key, int64_t now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = last_probe_attempt_.find(key);
    if (it == last_probe_attempt_.end()) return true;
    return (now_ms - it->second) >= MIN_PROBE_INTERVAL_MS;
}

void CooldownProbeState::mark_probe_attempt(const std::string& key, int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_probe_attempt_[key] = now_ms;
    // Enforce cap
    while (static_cast<int>(last_probe_attempt_.size()) > MAX_PROBE_KEYS) {
        auto oldest = last_probe_attempt_.begin();
        for (auto it = last_probe_attempt_.begin(); it != last_probe_attempt_.end(); ++it) {
            if (it->second < oldest->second) oldest = it;
        }
        last_probe_attempt_.erase(oldest);
    }
}

void CooldownProbeState::prune(int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = last_probe_attempt_.begin(); it != last_probe_attempt_.end(); ) {
        if (now_ms - it->second > PROBE_STATE_TTL_MS) {
            it = last_probe_attempt_.erase(it);
        } else {
            ++it;
        }
    }
}

void CooldownProbeState::reset_for_test() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_probe_attempt_.clear();
}

// ============================================================================
// Fallback Observation (对齐 model-fallback-observation.ts)
// ============================================================================

void log_model_fallback_decision(
    const std::string& decision,
    const ModelCandidate& candidate,
    int attempt,
    int total,
    FailoverReason reason,
    const std::string& error,
    const ModelCandidate* next_candidate
) {
    // Fire-and-forget observation — in production, send to observability system
    // For now, log via diagnostic events (swallowed if no observer registered)
    (void)decision;
    (void)candidate;
    (void)attempt;
    (void)total;
    (void)reason;
    (void)error;
    (void)next_candidate;
}

} // namespace llm
} // namespace hunterclaw
