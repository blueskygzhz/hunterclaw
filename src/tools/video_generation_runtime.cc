// 🎯 Strict alignment: OpenClaw src/video-generation/
// Files aligned:
//   types.ts              (154 lines) → GeneratedVideoAsset, VideoGenerationProvider, etc.
//   runtime.ts            (312 lines) → generateVideo() with fallback + guards
//   runtime-types.ts      (47 lines)  → GenerateVideoParams, GenerateVideoRuntimeResult
//   provider-registry.ts  (78 lines)  → list/get video generation providers
//   normalization.ts      (254 lines) → resolveVideoGenerationOverrides
//   capabilities.ts       (77 lines)  → resolveVideoGenerationMode/ModeCapabilities
//   duration-support.ts   (64 lines)  → normalizeVideoGenerationDuration
//   model-ref.ts          (8 lines)   → parseVideoGenerationModelRef
//   dashscope-compatible.ts (309 lines) → DashScope API integration
//   live-test-helpers.ts  (85 lines)  → DEFAULT_LIVE_VIDEO_MODELS

#include <hunterclaw/tools/tool.h>
#include <hunterclaw/core/types.h>
#include <hunterclaw/core/json.h>
#include <hunterclaw/core/logging.h>
#include <hunterclaw/config/config_system.h>
#include <hunterclaw/plugins/plugin_system.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hunterclaw::video_generation {

namespace {
auto log() { return core::get_logger("video-generation"); }
} // namespace

// ============================================================================
// Types (aligned: video-generation/types.ts — 154 lines)
// ============================================================================

struct GeneratedVideoAsset {
    std::optional<std::vector<uint8_t>> buffer;
    std::optional<std::string> url;
    std::string mime_type;
    std::optional<std::string> file_name;
    nlohmann::json metadata;
};

enum class VideoGenerationResolution { k480P, k720P, k768P, k1080P };

inline std::string resolution_to_string(VideoGenerationResolution r) {
    switch (r) {
        case VideoGenerationResolution::k480P: return "480P";
        case VideoGenerationResolution::k720P: return "720P";
        case VideoGenerationResolution::k768P: return "768P";
        case VideoGenerationResolution::k1080P: return "1080P";
    }
    return "480P";
}

// 🎯 对齐 OC: VideoGenerationAssetRole
using VideoGenerationAssetRole = std::string; // "first_frame"|"last_frame"|"reference_image"|...

struct VideoGenerationSourceAsset {
    std::optional<std::string> url;
    std::optional<std::vector<uint8_t>> buffer;
    std::optional<std::string> mime_type;
    std::optional<std::string> file_name;
    std::optional<std::string> role;
    nlohmann::json metadata;
};

struct VideoGenerationIgnoredOverride {
    std::string key;   // "size"|"aspectRatio"|"resolution"|"audio"|"watermark"
    std::string value;
};

// 🎯 对齐 OC: VideoGenerationMode
enum class VideoGenerationMode { kGenerate, kImageToVideo, kVideoToVideo };

// 🎯 对齐 OC: VideoGenerationProviderOptionType
using ProviderOptionType = std::string; // "number"|"boolean"|"string"

struct VideoGenerationModeCapabilities {
    std::optional<int> max_videos;
    std::optional<int> max_input_images;
    std::optional<int> max_input_videos;
    std::optional<int> max_input_audios;
    std::optional<int> max_duration_seconds;
    std::vector<int> supported_duration_seconds;
    std::map<std::string, std::vector<int>> supported_duration_seconds_by_model;
    std::vector<std::string> sizes;
    std::vector<std::string> aspect_ratios;
    std::vector<VideoGenerationResolution> resolutions;
    bool supports_size = false;
    bool supports_aspect_ratio = false;
    bool supports_resolution = false;
    bool supports_audio = false;
    bool supports_watermark = false;
    std::map<std::string, ProviderOptionType> provider_options;
};

struct VideoGenerationTransformCapabilities : VideoGenerationModeCapabilities {
    bool enabled = false;
};

struct VideoGenerationProviderCapabilities : VideoGenerationModeCapabilities {
    std::optional<VideoGenerationModeCapabilities> generate;
    std::optional<VideoGenerationTransformCapabilities> image_to_video;
    std::optional<VideoGenerationTransformCapabilities> video_to_video;
};

struct VideoGenerationNormalization {
    struct Entry {
        std::optional<std::string> requested;
        std::string applied;
        std::optional<std::string> derived_from;
    };
    struct NumEntry {
        std::optional<int> requested;
        int applied;
        std::vector<int> supported_values;
    };
    std::optional<Entry> size;
    std::optional<Entry> aspect_ratio;
    std::optional<Entry> resolution;
    std::optional<NumEntry> duration_seconds;
};

struct VideoGenerationRequest {
    std::string provider;
    std::string model;
    std::string prompt;
    nlohmann::json cfg;
    std::optional<std::string> agent_dir;
    std::optional<int> timeout_ms;
    std::optional<std::string> size;
    std::optional<std::string> aspect_ratio;
    std::optional<VideoGenerationResolution> resolution;
    std::optional<int> duration_seconds;
    std::optional<bool> audio;
    std::optional<bool> watermark;
    std::vector<VideoGenerationSourceAsset> input_images;
    std::vector<VideoGenerationSourceAsset> input_videos;
    std::vector<VideoGenerationSourceAsset> input_audios;
    nlohmann::json provider_options;
};

struct VideoGenerationResult {
    std::vector<GeneratedVideoAsset> videos;
    std::optional<std::string> model;
    nlohmann::json metadata;
};

struct VideoGenerationProvider {
    std::string id;
    std::vector<std::string> aliases;
    std::optional<std::string> label;
    std::optional<std::string> default_model;
    std::vector<std::string> models;
    VideoGenerationProviderCapabilities capabilities;
    std::function<bool(const nlohmann::json&)> is_configured;
    std::function<VideoGenerationResult(const VideoGenerationRequest&)> generate_video;
};

// ============================================================================
// Capabilities (aligned: video-generation/capabilities.ts — 77 lines)
// ============================================================================

std::optional<VideoGenerationMode> resolve_video_generation_mode(
        int input_image_count, int input_video_count) {
    if (input_image_count > 0 && input_video_count > 0) return std::nullopt;
    if (input_video_count > 0) return VideoGenerationMode::kVideoToVideo;
    if (input_image_count > 0) return VideoGenerationMode::kImageToVideo;
    return VideoGenerationMode::kGenerate;
}

std::vector<VideoGenerationMode> list_supported_modes(const VideoGenerationProvider& p) {
    std::vector<VideoGenerationMode> modes = {VideoGenerationMode::kGenerate};
    if (p.capabilities.image_to_video && p.capabilities.image_to_video->enabled)
        modes.push_back(VideoGenerationMode::kImageToVideo);
    if (p.capabilities.video_to_video && p.capabilities.video_to_video->enabled)
        modes.push_back(VideoGenerationMode::kVideoToVideo);
    return modes;
}

const VideoGenerationModeCapabilities* resolve_mode_capabilities(
        const VideoGenerationProvider& provider, int input_image_count, int input_video_count) {
    auto mode = resolve_video_generation_mode(input_image_count, input_video_count);
    if (!mode) return nullptr;
    switch (*mode) {
        case VideoGenerationMode::kGenerate:
            return provider.capabilities.generate
                ? &*provider.capabilities.generate : &provider.capabilities;
        case VideoGenerationMode::kImageToVideo:
            return provider.capabilities.image_to_video
                ? static_cast<const VideoGenerationModeCapabilities*>(&*provider.capabilities.image_to_video)
                : nullptr;
        case VideoGenerationMode::kVideoToVideo:
            return provider.capabilities.video_to_video
                ? static_cast<const VideoGenerationModeCapabilities*>(&*provider.capabilities.video_to_video)
                : nullptr;
    }
    return nullptr;
}

// ============================================================================
// Duration Support (aligned: video-generation/duration-support.ts — 64 lines)
// ============================================================================

std::vector<int> resolve_supported_durations(
        const VideoGenerationProvider& provider,
        const std::string& model, int input_image_count, int input_video_count) {
    auto* caps = resolve_mode_capabilities(provider, input_image_count, input_video_count);
    if (!caps) return {};
    // Check model-specific durations first
    auto it = caps->supported_duration_seconds_by_model.find(model);
    if (it != caps->supported_duration_seconds_by_model.end() && !it->second.empty()) {
        auto result = it->second;
        std::sort(result.begin(), result.end());
        return result;
    }
    auto result = caps->supported_duration_seconds;
    std::sort(result.begin(), result.end());
    return result;
}

std::optional<int> normalize_duration(
        const VideoGenerationProvider& provider, const std::string& model,
        std::optional<int> duration_seconds, int input_image_count, int input_video_count) {
    if (!duration_seconds) return std::nullopt;
    int rounded = std::max(1, *duration_seconds);
    auto supported = resolve_supported_durations(provider, model, input_image_count, input_video_count);
    if (supported.empty()) return rounded;
    // Snap to closest supported value
    int best = supported.front();
    for (int s : supported) {
        if (std::abs(s - rounded) < std::abs(best - rounded)) best = s;
        else if (std::abs(s - rounded) == std::abs(best - rounded) && s > best) best = s;
    }
    return best;
}

// ============================================================================
// Provider Registry (aligned: video-generation/provider-registry.ts — 78 lines)
// ============================================================================

static std::vector<VideoGenerationProvider>& registered_providers() {
    static std::vector<VideoGenerationProvider> providers;
    return providers;
}

void register_video_generation_provider(const VideoGenerationProvider& provider) {
    registered_providers().push_back(provider);
}

std::vector<const VideoGenerationProvider*> list_video_generation_providers() {
    std::vector<const VideoGenerationProvider*> result;
    for (const auto& p : registered_providers()) result.push_back(&p);
    return result;
}

const VideoGenerationProvider* get_video_generation_provider(const std::string& id) {
    std::string normalized = id;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    for (const auto& p : registered_providers()) {
        std::string pid = p.id;
        std::transform(pid.begin(), pid.end(), pid.begin(), ::tolower);
        if (pid == normalized) return &p;
        for (const auto& a : p.aliases) {
            std::string alias = a;
            std::transform(alias.begin(), alias.end(), alias.begin(), ::tolower);
            if (alias == normalized) return &p;
        }
    }
    return nullptr;
}

// ============================================================================
// Normalization (aligned: video-generation/normalization.ts — 254 lines)
// ============================================================================

struct ResolvedVideoOverrides {
    std::optional<std::string> size;
    std::optional<std::string> aspect_ratio;
    std::optional<VideoGenerationResolution> resolution;
    std::optional<int> duration_seconds;
    std::vector<int> supported_duration_seconds;
    std::optional<bool> audio;
    std::optional<bool> watermark;
    std::vector<VideoGenerationIgnoredOverride> ignored_overrides;
    std::optional<VideoGenerationNormalization> normalization;
};

ResolvedVideoOverrides resolve_video_generation_overrides(
        const VideoGenerationProvider& provider, const std::string& model,
        const std::optional<std::string>& size, const std::optional<std::string>& aspect_ratio,
        const std::optional<VideoGenerationResolution>& resolution,
        const std::optional<int>& duration_seconds, const std::optional<bool>& audio,
        const std::optional<bool>& watermark,
        int input_image_count, int input_video_count) {

    auto* caps = resolve_mode_capabilities(provider, input_image_count, input_video_count);
    ResolvedVideoOverrides result;
    VideoGenerationNormalization normalization;
    auto out_size = size;
    auto out_ar = aspect_ratio;
    auto out_res = resolution;
    auto out_audio = audio;
    auto out_watermark = watermark;

    if (caps) {
        // Size/AspectRatio/Resolution normalization (same pattern as image)
        if (!caps->supports_size && out_size) {
            result.ignored_overrides.push_back({"size", *out_size});
            out_size = std::nullopt;
        }
        if (!caps->supports_aspect_ratio && out_ar) {
            result.ignored_overrides.push_back({"aspectRatio", *out_ar});
            out_ar = std::nullopt;
        }
        if (!caps->supports_resolution && out_res) {
            result.ignored_overrides.push_back({"resolution", resolution_to_string(*out_res)});
            out_res = std::nullopt;
        }
        if (out_audio && !caps->supports_audio) {
            result.ignored_overrides.push_back({"audio", *out_audio ? "true" : "false"});
            out_audio = std::nullopt;
        }
        if (out_watermark && !caps->supports_watermark) {
            result.ignored_overrides.push_back({"watermark", *out_watermark ? "true" : "false"});
            out_watermark = std::nullopt;
        }
    }

    // Duration normalization
    auto normalized_duration = normalize_duration(
        provider, model, duration_seconds, input_image_count, input_video_count);
    auto supported_durations = resolve_supported_durations(
        provider, model, input_image_count, input_video_count);

    if (duration_seconds && normalized_duration && *duration_seconds != *normalized_duration) {
        VideoGenerationNormalization::NumEntry entry;
        entry.requested = *duration_seconds;
        entry.applied = *normalized_duration;
        entry.supported_values = supported_durations;
        normalization.duration_seconds = entry;
    }

    result.size = out_size;
    result.aspect_ratio = out_ar;
    result.resolution = out_res;
    result.duration_seconds = normalized_duration;
    result.supported_duration_seconds = supported_durations;
    result.audio = out_audio;
    result.watermark = out_watermark;

    if (normalization.size || normalization.aspect_ratio ||
        normalization.resolution || normalization.duration_seconds) {
        result.normalization = normalization;
    }

    return result;
}

// ============================================================================
// Runtime (aligned: video-generation/runtime.ts — 312 lines)
// ============================================================================

struct GenerateVideoParams {
    nlohmann::json cfg;
    std::string prompt;
    std::optional<std::string> agent_dir;
    std::optional<std::string> model_override;
    std::optional<std::string> size;
    std::optional<std::string> aspect_ratio;
    std::optional<VideoGenerationResolution> resolution;
    std::optional<int> duration_seconds;
    std::optional<bool> audio;
    std::optional<bool> watermark;
    std::vector<VideoGenerationSourceAsset> input_images;
    std::vector<VideoGenerationSourceAsset> input_videos;
    std::vector<VideoGenerationSourceAsset> input_audios;
    nlohmann::json provider_options;
};

struct FallbackAttempt {
    std::string provider;
    std::string model;
    std::optional<std::string> error;
};

struct GenerateVideoRuntimeResult {
    std::vector<GeneratedVideoAsset> videos;
    std::string provider;
    std::string model;
    std::vector<FallbackAttempt> attempts;
    std::optional<VideoGenerationNormalization> normalization;
    nlohmann::json metadata;
    std::vector<VideoGenerationIgnoredOverride> ignored_overrides;
};

// 🎯 对齐 OC: validateProviderOptionsAgainstDeclaration
std::optional<std::string> validate_provider_options(
        const std::string& provider_id, const std::string& model,
        const nlohmann::json& options,
        const std::map<std::string, ProviderOptionType>& declaration) {
    if (!options.is_object() || options.empty()) return std::nullopt;
    if (declaration.empty()) {
        return provider_id + "/" + model + " does not accept providerOptions; skipping";
    }
    for (auto& [key, _] : options.items()) {
        if (declaration.find(key) == declaration.end()) {
            return provider_id + "/" + model + " does not accept providerOptions key: " + key;
        }
    }
    return std::nullopt;
}

GenerateVideoRuntimeResult generate_video(const GenerateVideoParams& params) {
    struct Candidate { std::string provider; std::string model; };
    std::vector<Candidate> candidates;

    if (params.model_override) {
        auto slash = params.model_override->find('/');
        if (slash != std::string::npos) {
            candidates.push_back({
                params.model_override->substr(0, slash),
                params.model_override->substr(slash + 1)});
        }
    }

    if (candidates.empty()) {
        auto defaults = params.cfg.value("/agents/defaults"_json_pointer, nlohmann::json::object());
        auto model_config = defaults.value("videoGenerationModel", std::string(""));
        if (!model_config.empty()) {
            auto slash = model_config.find('/');
            if (slash != std::string::npos) {
                candidates.push_back({model_config.substr(0, slash), model_config.substr(slash + 1)});
            }
        }
    }

    if (candidates.empty()) {
        for (const auto* p : list_video_generation_providers()) {
            if (p->default_model) candidates.push_back({p->id, *p->default_model});
        }
    }

    if (candidates.empty()) {
        throw std::runtime_error("No video-generation model configured.");
    }

    std::vector<FallbackAttempt> attempts;
    int input_image_count = static_cast<int>(params.input_images.size());
    int input_video_count = static_cast<int>(params.input_videos.size());
    int input_audio_count = static_cast<int>(params.input_audios.size());

    for (const auto& candidate : candidates) {
        auto* provider = get_video_generation_provider(candidate.provider);
        if (!provider) {
            attempts.push_back({candidate.provider, candidate.model,
                "No video-generation provider registered for " + candidate.provider});
            continue;
        }

        // Guard: audio capability
        if (input_audio_count > 0) {
            auto* caps = resolve_mode_capabilities(*provider, input_image_count, input_video_count);
            int max_audio = (caps ? caps->max_input_audios.value_or(0) : 0);
            if (input_audio_count > max_audio) {
                attempts.push_back({candidate.provider, candidate.model,
                    candidate.provider + " does not support " + std::to_string(input_audio_count) + " audio inputs"});
                continue;
            }
        }

        // Guard: duration
        if (params.duration_seconds) {
            auto* caps = resolve_mode_capabilities(*provider, input_image_count, input_video_count);
            auto supported = resolve_supported_durations(*provider, candidate.model,
                input_image_count, input_video_count);
            if (supported.empty() && caps && caps->max_duration_seconds) {
                if (*params.duration_seconds > *caps->max_duration_seconds) {
                    attempts.push_back({candidate.provider, candidate.model,
                        "Duration " + std::to_string(*params.duration_seconds) + "s exceeds max"});
                    continue;
                }
            }
        }

        try {
            auto sanitized = resolve_video_generation_overrides(
                *provider, candidate.model,
                params.size, params.aspect_ratio, params.resolution,
                params.duration_seconds, params.audio, params.watermark,
                input_image_count, input_video_count);

            VideoGenerationRequest req;
            req.provider = candidate.provider;
            req.model = candidate.model;
            req.prompt = params.prompt;
            req.cfg = params.cfg;
            req.agent_dir = params.agent_dir;
            req.size = sanitized.size;
            req.aspect_ratio = sanitized.aspect_ratio;
            req.resolution = sanitized.resolution;
            req.duration_seconds = sanitized.duration_seconds;
            req.audio = sanitized.audio;
            req.watermark = sanitized.watermark;
            req.input_images = params.input_images;
            req.input_videos = params.input_videos;
            req.input_audios = params.input_audios;
            req.provider_options = params.provider_options;

            auto result = provider->generate_video(req);

            if (result.videos.empty()) {
                throw std::runtime_error("Video generation provider returned no videos.");
            }

            for (size_t i = 0; i < result.videos.size(); ++i) {
                const auto& v = result.videos[i];
                if (!v.buffer && !v.url) {
                    throw std::runtime_error(
                        "Video at index " + std::to_string(i) + " has neither buffer nor url.");
                }
            }

            return {
                std::move(result.videos), candidate.provider,
                result.model.value_or(candidate.model),
                std::move(attempts), sanitized.normalization,
                result.metadata, std::move(sanitized.ignored_overrides)
            };

        } catch (const std::exception& e) {
            attempts.push_back({candidate.provider, candidate.model, std::string(e.what())});
            log()->debug("video-generation candidate failed: {}/{}", candidate.provider, candidate.model);
        }
    }

    std::string errors;
    for (const auto& a : attempts) {
        if (a.error) {
            if (!errors.empty()) errors += "; ";
            errors += a.provider + ": " + *a.error;
        }
    }
    throw std::runtime_error("Video generation failed: " + errors);
}

// ============================================================================
// DashScope Compatible (aligned: dashscope-compatible.ts — 309 lines)
// ============================================================================

static const std::string DEFAULT_DASHSCOPE_WAN_VIDEO_MODEL = "wan2.6-t2v";
static const std::vector<std::string> DASHSCOPE_WAN_VIDEO_MODELS = {
    "wan2.6-t2v", "wan2.6-i2v", "wan2.6-r2v", "wan2.6-r2v-flash", "wan2.7-r2v"
};
static constexpr int DEFAULT_VIDEO_GENERATION_DURATION_SECONDS = 5;
static constexpr int DEFAULT_VIDEO_GENERATION_TIMEOUT_MS = 120000;
static constexpr int DEFAULT_VIDEO_GENERATION_POLL_INTERVAL_MS = 2500;
static constexpr int DEFAULT_VIDEO_GENERATION_MAX_POLL_ATTEMPTS = 120;

const std::map<std::string, std::string>& default_resolution_to_size() {
    static const std::map<std::string, std::string> mapping = {
        {"480P", "832*480"}, {"720P", "1280*720"}, {"1080P", "1920*1080"}
    };
    return mapping;
}

nlohmann::json build_dashscope_video_input(const VideoGenerationRequest& req) {
    nlohmann::json input;
    input["prompt"] = req.prompt;
    std::vector<std::string> ref_urls;
    for (const auto& img : req.input_images) {
        if (img.url) ref_urls.push_back(*img.url);
    }
    for (const auto& vid : req.input_videos) {
        if (vid.url) ref_urls.push_back(*vid.url);
    }
    if (ref_urls.size() == 1 && req.input_images.size() == 1 && req.input_videos.empty()) {
        input["img_url"] = ref_urls[0];
    } else if (!ref_urls.empty()) {
        input["reference_urls"] = ref_urls;
    }
    return input;
}

nlohmann::json build_dashscope_video_parameters(const VideoGenerationRequest& req) {
    nlohmann::json params;
    if (req.size) params["size"] = *req.size;
    if (req.aspect_ratio) params["aspect_ratio"] = *req.aspect_ratio;
    if (req.duration_seconds) params["duration"] = *req.duration_seconds;
    if (req.audio.has_value()) params["enable_audio"] = *req.audio;
    if (req.watermark.has_value()) params["watermark"] = *req.watermark;
    return params.empty() ? nlohmann::json() : params;
}

// ============================================================================
// Default Live Models (aligned: live-test-helpers.ts — 85 lines)
// ============================================================================

const std::map<std::string, std::string>& default_live_video_models() {
    static const std::map<std::string, std::string> models = {
        {"alibaba", "alibaba/wan2.6-t2v"},
        {"byteplus", "byteplus/seedance-1-0-lite-t2v-250428"},
        {"fal", "fal/fal-ai/minimax/video-01-live"},
        {"google", "google/veo-3.1-fast-generate-preview"},
        {"minimax", "minimax/MiniMax-Hailuo-2.3"},
        {"openai", "openai/sora-2"},
        {"qwen", "qwen/wan2.6-t2v"},
        {"runway", "runway/gen4.5"},
        {"together", "together/Wan-AI/Wan2.2-T2V-A14B"},
        {"vydra", "vydra/veo3"},
        {"xai", "xai/grok-imagine-video"},
    };
    return models;
}

} // namespace hunterclaw::video_generation
