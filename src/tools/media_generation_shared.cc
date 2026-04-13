// 🎯 Strict alignment: OpenClaw media-generation/runtime-shared.ts
// Shared infrastructure for image/video/music generation — capability candidate resolution,
// failover error handling, provider mapping.

#include <hunterclaw/tools/tool.h>
#include <hunterclaw/core/types.h>
#include <hunterclaw/core/json.h>
#include <hunterclaw/core/logging.h>
#include <hunterclaw/config/config_system.h>
#include <hunterclaw/llm/model_selection.h>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hunterclaw::tools {

namespace {
auto log() { return core::get_logger("media-generation"); }
} // namespace

// ─── Types (aligned: media-generation/runtime-shared.ts) ─────────────────

enum class MediaGenerationType {
    kImage,
    kVideo,
    kMusic
};

struct MediaGenerationModelCandidate {
    std::string provider_id;
    std::string model_id;
    std::string display_name;
    int priority = 0;  // Lower = higher priority
    bool has_auth = false;
};

struct MediaGenerationNormalizationMetadata {
    std::string capability_label;
    std::string model_config_key;
    std::string resolved_provider;
    std::string resolved_model;
    bool is_fallback = false;
};

struct MediaGenerationResult {
    bool ok = false;
    std::optional<std::string> error;
    std::optional<std::string> url;
    std::optional<std::string> file_path;
    std::optional<nlohmann::json> metadata;
};

// ─── Failover error detection (aligned: isFailoverError / describeFailoverError) ─

bool is_failover_error(const std::exception& e) {
    // 🎯 对齐 OC agents/failover-error.ts: isFailoverError
    std::string msg = e.what();

    // Transient errors that should trigger failover
    static const std::vector<std::string> failover_patterns = {
        "rate limit",
        "429",
        "503",
        "502",
        "timeout",
        "ECONNRESET",
        "ECONNREFUSED",
        "ETIMEDOUT",
        "overloaded",
        "capacity",
        "quota exceeded",
        "model_not_available",
    };

    for (const auto& pattern : failover_patterns) {
        if (msg.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string describe_failover_error(const std::exception& e) {
    // 🎯 对齐 OC: describeFailoverError
    std::string msg = e.what();
    if (msg.find("429") != std::string::npos || msg.find("rate limit") != std::string::npos) {
        return "rate-limited";
    }
    if (msg.find("503") != std::string::npos || msg.find("overloaded") != std::string::npos) {
        return "provider-overloaded";
    }
    if (msg.find("timeout") != std::string::npos || msg.find("ETIMEDOUT") != std::string::npos) {
        return "timeout";
    }
    return "transient-error";
}

// ─── Capability candidate resolution (aligned: resolveCapabilityModelCandidates) ─

struct ResolveCapabilityCandidatesParams {
    nlohmann::json cfg;
    std::string capability_label;      // e.g. "image-generation"
    std::string model_config_key;      // e.g. "imageGenerationModel"
    std::string preferred_provider;
    std::string preferred_model;
};

std::vector<MediaGenerationModelCandidate> resolve_capability_model_candidates(
        const ResolveCapabilityCandidatesParams& params) {
    // 🎯 对齐 OC: resolveCapabilityModelCandidates

    std::vector<MediaGenerationModelCandidate> candidates;

    // 1. Check explicit model config
    auto agents_defaults = params.cfg.value("/agents/defaults"_json_pointer, nlohmann::json::object());
    auto model_value = agents_defaults.value(params.model_config_key, std::string(""));

    if (!model_value.empty()) {
        // Parse provider/model format
        auto slash = model_value.find('/');
        std::string provider, model;
        if (slash != std::string::npos) {
            provider = model_value.substr(0, slash);
            model = model_value.substr(slash + 1);
        } else {
            model = model_value;
        }

        if (!provider.empty() && !model.empty()) {
            candidates.push_back({provider, model, model_value, 0, true});
        }
    }

    // 2. Add preferred provider/model if set
    if (!params.preferred_provider.empty() && !params.preferred_model.empty()) {
        bool already_present = false;
        for (const auto& c : candidates) {
            if (c.provider_id == params.preferred_provider && c.model_id == params.preferred_model) {
                already_present = true;
                break;
            }
        }
        if (!already_present) {
            candidates.push_back({
                params.preferred_provider,
                params.preferred_model,
                params.preferred_provider + "/" + params.preferred_model,
                1,
                true
            });
        }
    }

    log()->debug("resolve_capability_model_candidates: {} candidates for {}",
        candidates.size(), params.capability_label);
    return candidates;
}

// ─── No capability model configured message (aligned: buildNoCapabilityModelConfiguredMessage) ─

std::string build_no_capability_model_configured_message(
        const std::string& capability_label,
        const std::string& model_config_key,
        const std::vector<std::string>& available_providers) {
    // 🎯 对齐 OC: buildNoCapabilityModelConfiguredMessage

    std::string msg = "No " + capability_label + " model configured. ";
    msg += "Set agents.defaults." + model_config_key + " in your config. ";

    if (!available_providers.empty()) {
        msg += "Available providers: ";
        for (size_t i = 0; i < available_providers.size(); ++i) {
            if (i > 0) msg += ", ";
            msg += available_providers[i];
        }
        msg += ".";
    }

    return msg;
}

// ─── Generation normalization metadata (aligned: buildMediaGenerationNormalizationMetadata) ─

MediaGenerationNormalizationMetadata build_media_generation_normalization_metadata(
        const std::string& capability_label,
        const std::string& model_config_key,
        const std::string& resolved_provider,
        const std::string& resolved_model,
        bool is_fallback) {
    // 🎯 对齐 OC: buildMediaGenerationNormalizationMetadata
    return {
        capability_label,
        model_config_key,
        resolved_provider,
        resolved_model,
        is_fallback
    };
}

// ─── Throw capability generation failure (aligned: throwCapabilityGenerationFailure) ─

void throw_capability_generation_failure(
        const std::string& capability_label,
        const std::string& error_message,
        const std::vector<std::string>& attempted_providers) {
    // 🎯 对齐 OC: throwCapabilityGenerationFailure

    std::string msg = capability_label + " generation failed";
    if (!attempted_providers.empty()) {
        msg += " (tried: ";
        for (size_t i = 0; i < attempted_providers.size(); ++i) {
            if (i > 0) msg += ", ";
            msg += attempted_providers[i];
        }
        msg += ")";
    }
    msg += ": " + error_message;

    log()->error("{}", msg);
    throw std::runtime_error(msg);
}

// ─── Fallback attempt tracking (aligned: FallbackAttempt type) ───────────

struct FallbackAttempt {
    std::string provider;
    std::string model;
    bool ok = false;
    std::optional<std::string> error;
    int duration_ms = 0;
};

// ─── Generate with fallback (aligned: common pattern in image/video/music runtime) ─

template<typename GenerateFn>
MediaGenerationResult generate_with_fallback(
        const std::vector<MediaGenerationModelCandidate>& candidates,
        const std::string& capability_label,
        GenerateFn&& generate_fn) {
    // 🎯 对齐 OC: Common pattern across image-generation/runtime.ts, video-generation/runtime.ts

    std::vector<FallbackAttempt> attempts;
    std::vector<std::string> attempted_providers;

    for (const auto& candidate : candidates) {
        auto start = std::chrono::steady_clock::now();
        FallbackAttempt attempt;
        attempt.provider = candidate.provider_id;
        attempt.model = candidate.model_id;

        try {
            log()->info("{}: trying provider={} model={}",
                capability_label, candidate.provider_id, candidate.model_id);

            auto result = generate_fn(candidate);

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            attempt.ok = true;
            attempt.duration_ms = static_cast<int>(elapsed);
            attempts.push_back(attempt);

            log()->info("{}: success with {} in {}ms",
                capability_label, candidate.display_name, elapsed);
            return result;

        } catch (const std::exception& e) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            attempt.ok = false;
            attempt.error = e.what();
            attempt.duration_ms = static_cast<int>(elapsed);
            attempts.push_back(attempt);
            attempted_providers.push_back(candidate.provider_id);

            if (is_failover_error(e)) {
                log()->warn("{}: failover from {} ({}), trying next candidate...",
                    capability_label, candidate.display_name, describe_failover_error(e));
                continue;
            }

            // Non-failover error — stop trying
            log()->error("{}: fatal error from {}: {}",
                capability_label, candidate.display_name, e.what());
            return {false, e.what(), std::nullopt, std::nullopt, std::nullopt};
        }
    }

    // All candidates exhausted
    std::string all_errors;
    for (const auto& a : attempts) {
        if (!a.ok && a.error.has_value()) {
            if (!all_errors.empty()) all_errors += "; ";
            all_errors += a.provider + ": " + *a.error;
        }
    }
    return {false, "All candidates exhausted: " + all_errors,
            std::nullopt, std::nullopt, std::nullopt};
}

} // namespace hunterclaw::tools
