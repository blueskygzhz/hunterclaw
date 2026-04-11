#pragma once
// ============================================================================
// model_fallback.h
// 严格对齐 OpenClaw: model-fallback.ts (962行)
//                   + model-fallback.types.ts (16行)
//                   + model-fallback-observation.ts (96行)
//                   + failover-error.ts
//                   + failover-policy.ts
// ============================================================================

#include "hunterclaw/llm/model_selection.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>
#include <map>
#include <set>
#include <mutex>
#include <stdexcept>

namespace hunterclaw {
namespace llm {

// ============================================================================
// ModelCandidate + FallbackAttempt (对齐 model-fallback.types.ts)
// ============================================================================

struct ModelCandidate {
    std::string provider;
    std::string model;
};

/// 对齐: FailoverReason
enum class FailoverReason {
    Unknown,
    RateLimit,
    Overloaded,
    Auth,
    AuthPermanent,
    Billing,
    ModelNotFound,
    ContextOverflow,
    Timeout,
    NetworkError,
    ServerError
};

std::string failover_reason_to_string(FailoverReason reason);
FailoverReason failover_reason_from_string(const std::string& s);

struct FallbackAttempt {
    std::string provider;
    std::string model;
    std::string error;
    FailoverReason reason = FailoverReason::Unknown;
    std::optional<int> status;
    std::optional<std::string> code;
};

// ============================================================================
// FailoverError (对齐 failover-error.ts)
// ============================================================================

class FailoverError : public std::runtime_error {
public:
    FailoverError(const std::string& message, FailoverReason reason,
                  const std::string& provider = "", const std::string& model = "",
                  int status = 0, const std::string& code = "");
    
    FailoverReason reason() const { return reason_; }
    const std::string& provider() const { return provider_; }
    const std::string& model() const { return model_; }
    int status() const { return status_; }
    const std::string& code() const { return code_; }
    
private:
    FailoverReason reason_;
    std::string provider_;
    std::string model_;
    int status_;
    std::string code_;
};

/// 对齐: isFailoverError
bool is_failover_error(const std::exception& e);

/// 对齐: coerceToFailoverError — detect rate-limit/overloaded from HTTP status
std::optional<FailoverError> coerce_to_failover_error(
    const std::exception& e,
    const std::string& provider,
    const std::string& model);

/// 对齐: describeFailoverError
struct DescribedFailoverError {
    std::string message;
    FailoverReason reason;
    std::optional<int> status;
    std::optional<std::string> code;
};

DescribedFailoverError describe_failover_error(const std::exception& e);

// ============================================================================
// FallbackSummaryError (对齐 model-fallback.ts:51-66)
// ============================================================================

class FallbackSummaryError : public std::runtime_error {
public:
    FallbackSummaryError(const std::string& message,
                         std::vector<FallbackAttempt> attempts,
                         std::optional<int64_t> soonest_cooldown_expiry);
    
    const std::vector<FallbackAttempt>& attempts() const { return attempts_; }
    std::optional<int64_t> soonest_cooldown_expiry() const { return soonest_cooldown_expiry_; }
    
private:
    std::vector<FallbackAttempt> attempts_;
    std::optional<int64_t> soonest_cooldown_expiry_;
};

bool is_fallback_summary_error(const std::exception& e);

// ============================================================================
// Fallback Policy (对齐 failover-policy.ts)
// ============================================================================

/// 对齐: shouldAllowCooldownProbeForReason
bool should_allow_cooldown_probe_for_reason(FailoverReason reason);

/// 对齐: shouldUseTransientCooldownProbeSlot
bool should_use_transient_cooldown_probe_slot(FailoverReason reason);

/// 对齐: shouldPreserveTransientCooldownProbeSlot
bool should_preserve_transient_cooldown_probe_slot(FailoverReason reason);

// ============================================================================
// Model Fallback Run (对齐 model-fallback.ts:632-908)
// ============================================================================

struct ModelFallbackRunOptions {
    bool allow_transient_cooldown_probe = false;
};

template<typename T>
using ModelFallbackRunFn = std::function<T(
    const std::string& provider,
    const std::string& model,
    const ModelFallbackRunOptions& options)>;

template<typename T>
struct ModelFallbackRunResult {
    T result;
    std::string provider;
    std::string model;
    std::vector<FallbackAttempt> attempts;
};

/// 对齐: resolveFallbackCandidates
std::vector<ModelCandidate> resolve_fallback_candidates(
    const core::Config& cfg,
    const std::string& provider,
    const std::string& model,
    const std::vector<std::string>& fallbacks_override = {});

/// 对齐: runWithModelFallback<T> (simplified sync version for C++)
/// In C++ we use a sync callback instead of async/await.
/// The run function is called for each candidate until one succeeds.
template<typename T>
ModelFallbackRunResult<T> run_with_model_fallback(
    const core::Config& cfg,
    const std::string& provider,
    const std::string& model,
    ModelFallbackRunFn<T> run,
    std::function<void(const std::string&, const std::string&, const std::exception&, int, int)> on_error = nullptr)
{
    auto candidates = resolve_fallback_candidates(cfg, provider, model);
    if (candidates.empty()) {
        candidates.push_back({provider, model});
    }
    
    std::vector<FallbackAttempt> attempts;
    std::exception_ptr last_error;
    
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        ModelFallbackRunOptions options;
        
        try {
            T result = run(candidate.provider, candidate.model, options);
            return ModelFallbackRunResult<T>{
                std::move(result),
                candidate.provider,
                candidate.model,
                std::move(attempts)
            };
        } catch (const std::exception& e) {
            last_error = std::current_exception();
            
            auto described = describe_failover_error(e);
            attempts.push_back(FallbackAttempt{
                candidate.provider,
                candidate.model,
                described.message,
                described.reason,
                described.status,
                described.code
            });
            
            if (on_error) {
                on_error(candidate.provider, candidate.model, e,
                        static_cast<int>(i + 1),
                        static_cast<int>(candidates.size()));
            }
        }
    }
    
    // All candidates failed
    if (attempts.size() <= 1 && last_error) {
        std::rethrow_exception(last_error);
    }
    
    std::string summary;
    for (const auto& a : attempts) {
        if (!summary.empty()) summary += " | ";
        summary += a.provider + "/" + a.model + ": " + a.error;
        if (a.reason != FailoverReason::Unknown) {
            summary += " (" + failover_reason_to_string(a.reason) + ")";
        }
    }
    
    throw FallbackSummaryError(
        "All models failed (" + std::to_string(attempts.size()) + "): " + summary,
        attempts,
        std::nullopt);
}

// ============================================================================
// Cooldown Probe State (对齐 model-fallback.ts:451-498)
// ============================================================================

class CooldownProbeState {
public:
    static CooldownProbeState& instance();
    
    bool is_probe_throttle_open(const std::string& key, int64_t now_ms) const;
    void mark_probe_attempt(const std::string& key, int64_t now_ms);
    void prune(int64_t now_ms);
    void reset_for_test();
    
    static constexpr int MIN_PROBE_INTERVAL_MS = 30000;
    static constexpr int PROBE_MARGIN_MS = 2 * 60 * 1000;
    static constexpr int64_t PROBE_STATE_TTL_MS = 24LL * 60 * 60 * 1000;
    static constexpr int MAX_PROBE_KEYS = 256;
    
private:
    CooldownProbeState() = default;
    mutable std::mutex mutex_;
    std::map<std::string, int64_t> last_probe_attempt_;
};

// ============================================================================
// Fallback Observation (对齐 model-fallback-observation.ts)
// ============================================================================

/// 对齐: logModelFallbackDecision — fire-and-forget observation log
void log_model_fallback_decision(
    const std::string& decision,
    const ModelCandidate& candidate,
    int attempt,
    int total,
    FailoverReason reason = FailoverReason::Unknown,
    const std::string& error = "",
    const ModelCandidate* next_candidate = nullptr);

} // namespace llm
} // namespace hunterclaw
