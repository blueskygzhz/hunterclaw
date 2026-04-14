// 🎯 Strict alignment: OpenClaw src/image-generation/
// Files aligned:
//   types.ts           (95 lines)  → GeneratedImageAsset, ImageGenerationProvider, etc.
//   runtime.ts          (125 lines) → generateImage() with fallback chain
//   runtime-types.ts    (41 lines)  → GenerateImageParams, GenerateImageRuntimeResult
//   provider-registry.ts (78 lines) → list/get image generation providers
//   normalization.ts    (205 lines) → resolveImageGenerationOverrides (size/aspectRatio/resolution)
//   model-ref.ts        (8 lines)   → parseImageGenerationModelRef
//   live-test-helpers.ts (47 lines) → DEFAULT_LIVE_IMAGE_MODELS

#include <hunterclaw/tools/tool.h>
#include <hunterclaw/core/types.h>
#include <hunterclaw/core/json.h>
#include <hunterclaw/core/logging.h>
#include <hunterclaw/config/config_system.h>
#include <hunterclaw/llm/model_selection.h>
#include <hunterclaw/plugins/plugin_system.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hunterclaw::image_generation {

namespace {
auto log() { return core::get_logger("image-generation"); }
} // namespace

// ============================================================================
// Types (aligned: image-generation/types.ts — 95 lines)
// ============================================================================

struct GeneratedImageAsset {
    std::vector<uint8_t> buffer;
    std::string mime_type;
    std::optional<std::string> file_name;
    std::optional<std::string> revised_prompt;
    nlohmann::json metadata;
};

enum class ImageGenerationResolution { k1K, k2K, k4K };

inline std::string resolution_to_string(ImageGenerationResolution r) {
    switch (r) {
        case ImageGenerationResolution::k1K: return "1K";
        case ImageGenerationResolution::k2K: return "2K";
        case ImageGenerationResolution::k4K: return "4K";
    }
    return "1K";
}

inline std::optional<ImageGenerationResolution> parse_resolution(const std::string& s) {
    if (s == "1K") return ImageGenerationResolution::k1K;
    if (s == "2K") return ImageGenerationResolution::k2K;
    if (s == "4K") return ImageGenerationResolution::k4K;
    return std::nullopt;
}

struct ImageGenerationSourceImage {
    std::vector<uint8_t> buffer;
    std::string mime_type;
    std::optional<std::string> file_name;
    nlohmann::json metadata;
};

struct ImageGenerationIgnoredOverride {
    std::string key;   // "size" | "aspectRatio" | "resolution"
    std::string value;
};

struct ImageGenerationModeCapabilities {
    std::optional<int> max_count;
    bool supports_size = false;
    bool supports_aspect_ratio = false;
    bool supports_resolution = false;
};

struct ImageGenerationEditCapabilities : ImageGenerationModeCapabilities {
    bool enabled = false;
    std::optional<int> max_input_images;
};

struct ImageGenerationGeometryCapabilities {
    std::vector<std::string> sizes;
    std::vector<std::string> aspect_ratios;
    std::vector<ImageGenerationResolution> resolutions;
};

struct ImageGenerationProviderCapabilities {
    ImageGenerationModeCapabilities generate;
    ImageGenerationEditCapabilities edit;
    std::optional<ImageGenerationGeometryCapabilities> geometry;
};

// 🎯 对齐 OC: ImageGenerationNormalization
struct ImageGenerationNormalization {
    struct Entry {
        std::optional<std::string> requested;
        std::string applied;
        std::optional<std::string> derived_from;
    };
    std::optional<Entry> size;
    std::optional<Entry> aspect_ratio;
    std::optional<Entry> resolution;
};

struct ImageGenerationRequest {
    std::string provider;
    std::string model;
    std::string prompt;
    nlohmann::json cfg;
    std::optional<std::string> agent_dir;
    std::optional<int> timeout_ms;
    std::optional<int> count;
    std::optional<std::string> size;
    std::optional<std::string> aspect_ratio;
    std::optional<ImageGenerationResolution> resolution;
    std::vector<ImageGenerationSourceImage> input_images;
};

struct ImageGenerationResult {
    std::vector<GeneratedImageAsset> images;
    std::optional<std::string> model;
    nlohmann::json metadata;
};

// 🎯 对齐 OC: ImageGenerationProvider interface
struct ImageGenerationProvider {
    std::string id;
    std::vector<std::string> aliases;
    std::optional<std::string> label;
    std::optional<std::string> default_model;
    std::vector<std::string> models;
    ImageGenerationProviderCapabilities capabilities;
    std::function<bool(const nlohmann::json&)> is_configured;
    std::function<ImageGenerationResult(const ImageGenerationRequest&)> generate_image;
};

// ============================================================================
// Model Ref (aligned: image-generation/model-ref.ts — 8 lines)
// ============================================================================

struct ParsedModelRef {
    std::string provider;
    std::string model;
};

std::optional<ParsedModelRef> parse_image_generation_model_ref(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    auto slash = raw.find('/');
    if (slash == std::string::npos || slash == 0 || slash == raw.size() - 1) {
        return std::nullopt;
    }
    return ParsedModelRef{raw.substr(0, slash), raw.substr(slash + 1)};
}

// ============================================================================
// Provider Registry (aligned: image-generation/provider-registry.ts — 78 lines)
// ============================================================================

static const std::set<std::string> UNSAFE_PROVIDER_IDS = {"__proto__", "constructor", "prototype"};

std::optional<std::string> normalize_provider_id(const std::string& id) {
    std::string normalized = id;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    // trim
    auto start = normalized.find_first_not_of(" \t");
    if (start == std::string::npos) return std::nullopt;
    auto end = normalized.find_last_not_of(" \t");
    normalized = normalized.substr(start, end - start + 1);
    if (normalized.empty() || UNSAFE_PROVIDER_IDS.count(normalized)) return std::nullopt;
    return normalized;
}

// Global provider storage (populated by plugin registration)
static std::vector<ImageGenerationProvider>& registered_providers() {
    static std::vector<ImageGenerationProvider> providers;
    return providers;
}

void register_image_generation_provider(const ImageGenerationProvider& provider) {
    registered_providers().push_back(provider);
}

struct ProviderMaps {
    std::map<std::string, const ImageGenerationProvider*> canonical;
    std::map<std::string, const ImageGenerationProvider*> aliases;
};

ProviderMaps build_provider_maps() {
    ProviderMaps maps;
    for (const auto& provider : registered_providers()) {
        auto id = normalize_provider_id(provider.id);
        if (!id || UNSAFE_PROVIDER_IDS.count(*id)) continue;
        maps.canonical[*id] = &provider;
        maps.aliases[*id] = &provider;
        for (const auto& alias : provider.aliases) {
            auto norm_alias = normalize_provider_id(alias);
            if (norm_alias && !UNSAFE_PROVIDER_IDS.count(*norm_alias)) {
                maps.aliases[*norm_alias] = &provider;
            }
        }
    }
    return maps;
}

std::vector<const ImageGenerationProvider*> list_image_generation_providers() {
    auto maps = build_provider_maps();
    std::vector<const ImageGenerationProvider*> result;
    for (const auto& [_, p] : maps.canonical) result.push_back(p);
    return result;
}

const ImageGenerationProvider* get_image_generation_provider(const std::string& provider_id) {
    auto normalized = normalize_provider_id(provider_id);
    if (!normalized) return nullptr;
    auto maps = build_provider_maps();
    auto it = maps.aliases.find(*normalized);
    return it != maps.aliases.end() ? it->second : nullptr;
}

// ============================================================================
// Normalization (aligned: image-generation/normalization.ts — 205 lines)
// ============================================================================

// 🎯 对齐 OC: resolveClosestSize / resolveClosestAspectRatio / resolveClosestResolution
// Simplified implementations — full OC logic handles complex geometric matching

std::optional<std::string> resolve_closest_size(
        const std::string& requested,
        const std::vector<std::string>& supported) {
    if (supported.empty()) return requested;
    // Exact match first
    for (const auto& s : supported) {
        if (s == requested) return s;
    }
    // If no match, return first supported (OC does geometric distance)
    return supported.front();
}

std::optional<std::string> resolve_closest_aspect_ratio(
        const std::string& requested,
        const std::optional<std::string>& requested_size,
        const std::vector<std::string>& supported) {
    if (supported.empty()) return std::nullopt;
    for (const auto& ar : supported) {
        if (ar == requested) return ar;
    }
    return supported.front();
}

struct ResolvedImageGenerationOverrides {
    std::optional<std::string> size;
    std::optional<std::string> aspect_ratio;
    std::optional<ImageGenerationResolution> resolution;
    std::vector<ImageGenerationIgnoredOverride> ignored_overrides;
    std::optional<ImageGenerationNormalization> normalization;
};

ResolvedImageGenerationOverrides resolve_image_generation_overrides(
        const ImageGenerationProvider& provider,
        const std::optional<std::string>& size,
        const std::optional<std::string>& aspect_ratio,
        const std::optional<ImageGenerationResolution>& resolution,
        const std::vector<ImageGenerationSourceImage>& input_images) {

    bool has_input_images = !input_images.empty();
    const auto& mode_caps = has_input_images
        ? static_cast<const ImageGenerationModeCapabilities&>(provider.capabilities.edit)
        : provider.capabilities.generate;
    const auto& geometry = provider.capabilities.geometry;

    ResolvedImageGenerationOverrides result;
    ImageGenerationNormalization normalization;
    auto out_size = size;
    auto out_ar = aspect_ratio;
    auto out_res = resolution;

    // Size normalization
    if (out_size && geometry && !geometry->sizes.empty() && mode_caps.supports_size) {
        auto normalized = resolve_closest_size(*out_size, geometry->sizes);
        if (normalized && *normalized != *out_size) {
            normalization.size = ImageGenerationNormalization::Entry{*out_size, *normalized, std::nullopt};
        }
        out_size = normalized;
    }

    // Size → AspectRatio translation (when size not supported)
    if (!mode_caps.supports_size && out_size) {
        bool translated = false;
        if (mode_caps.supports_aspect_ratio && geometry) {
            auto norm_ar = resolve_closest_aspect_ratio(
                out_ar.value_or(""), out_size, geometry->aspect_ratios);
            if (norm_ar) {
                out_ar = norm_ar;
                normalization.aspect_ratio = ImageGenerationNormalization::Entry{
                    std::nullopt, *norm_ar, "size"};
                translated = true;
            }
        }
        if (!translated) {
            result.ignored_overrides.push_back({"size", *out_size});
        }
        out_size = std::nullopt;
    }

    // AspectRatio normalization
    if (out_ar && geometry && !geometry->aspect_ratios.empty() && mode_caps.supports_aspect_ratio) {
        auto normalized = resolve_closest_aspect_ratio(*out_ar, out_size, geometry->aspect_ratios);
        if (normalized && *normalized != *out_ar) {
            normalization.aspect_ratio = ImageGenerationNormalization::Entry{*out_ar, *normalized, std::nullopt};
        }
        out_ar = normalized;
    } else if (!mode_caps.supports_aspect_ratio && out_ar) {
        result.ignored_overrides.push_back({"aspectRatio", *out_ar});
        out_ar = std::nullopt;
    }

    // Resolution normalization
    if (out_res && !mode_caps.supports_resolution) {
        result.ignored_overrides.push_back({"resolution", resolution_to_string(*out_res)});
        out_res = std::nullopt;
    }

    // Final safety guards
    if (out_size && !mode_caps.supports_size) {
        result.ignored_overrides.push_back({"size", *out_size});
        out_size = std::nullopt;
    }
    if (out_ar && !mode_caps.supports_aspect_ratio) {
        result.ignored_overrides.push_back({"aspectRatio", *out_ar});
        out_ar = std::nullopt;
    }

    result.size = out_size;
    result.aspect_ratio = out_ar;
    result.resolution = out_res;

    // Check if any normalization entry was set
    if (normalization.size || normalization.aspect_ratio || normalization.resolution) {
        result.normalization = normalization;
    }

    return result;
}

// ============================================================================
// Runtime (aligned: image-generation/runtime.ts — 125 lines)
// ============================================================================

// 🎯 对齐 OC: GenerateImageParams
struct GenerateImageParams {
    nlohmann::json cfg;
    std::string prompt;
    std::optional<std::string> agent_dir;
    std::optional<std::string> model_override;
    std::optional<int> count;
    std::optional<std::string> size;
    std::optional<std::string> aspect_ratio;
    std::optional<ImageGenerationResolution> resolution;
    std::vector<ImageGenerationSourceImage> input_images;
};

struct FallbackAttempt {
    std::string provider;
    std::string model;
    std::optional<std::string> error;
    std::optional<std::string> reason;
    std::optional<int> status;
};

// 🎯 对齐 OC: GenerateImageRuntimeResult
struct GenerateImageRuntimeResult {
    std::vector<GeneratedImageAsset> images;
    std::string provider;
    std::string model;
    std::vector<FallbackAttempt> attempts;
    std::optional<ImageGenerationNormalization> normalization;
    nlohmann::json metadata;
    std::vector<ImageGenerationIgnoredOverride> ignored_overrides;
};

std::vector<const ImageGenerationProvider*> list_runtime_image_generation_providers() {
    return list_image_generation_providers();
}

// 🎯 对齐 OC: generateImage — main runtime entry point
GenerateImageRuntimeResult generate_image(const GenerateImageParams& params) {
    // 1. Resolve model candidates
    struct Candidate { std::string provider; std::string model; };
    std::vector<Candidate> candidates;

    // Check model override
    if (params.model_override) {
        auto ref = parse_image_generation_model_ref(*params.model_override);
        if (ref) candidates.push_back({ref->provider, ref->model});
    }

    // Check config model
    auto agents_defaults = params.cfg.value("/agents/defaults"_json_pointer, nlohmann::json::object());
    auto model_config = agents_defaults.value("imageGenerationModel", std::string(""));
    if (!model_config.empty() && candidates.empty()) {
        auto ref = parse_image_generation_model_ref(model_config);
        if (ref) candidates.push_back({ref->provider, ref->model});
    }

    // Auto-discover from registered providers
    if (candidates.empty()) {
        for (const auto* p : list_image_generation_providers()) {
            if (p->default_model) {
                candidates.push_back({p->id, *p->default_model});
            }
        }
    }

    if (candidates.empty()) {
        throw std::runtime_error(
            "No image-generation model configured. Set agents.defaults.imageGenerationModel in your config.");
    }

    // 2. Try each candidate with fallback
    std::vector<FallbackAttempt> attempts;

    for (const auto& candidate : candidates) {
        auto* provider = get_image_generation_provider(candidate.provider);
        if (!provider) {
            std::string error = "No image-generation provider registered for " + candidate.provider;
            attempts.push_back({candidate.provider, candidate.model, error, std::nullopt, std::nullopt});
            continue;
        }

        try {
            auto sanitized = resolve_image_generation_overrides(
                *provider, params.size, params.aspect_ratio,
                params.resolution, params.input_images);

            ImageGenerationRequest req;
            req.provider = candidate.provider;
            req.model = candidate.model;
            req.prompt = params.prompt;
            req.cfg = params.cfg;
            req.agent_dir = params.agent_dir;
            req.count = params.count;
            req.size = sanitized.size;
            req.aspect_ratio = sanitized.aspect_ratio;
            req.resolution = sanitized.resolution;
            req.input_images = params.input_images;

            auto result = provider->generate_image(req);

            if (result.images.empty()) {
                throw std::runtime_error("Image generation provider returned no images.");
            }

            return {
                std::move(result.images),
                candidate.provider,
                result.model.value_or(candidate.model),
                std::move(attempts),
                sanitized.normalization,
                result.metadata,
                std::move(sanitized.ignored_overrides)
            };

        } catch (const std::exception& e) {
            attempts.push_back({
                candidate.provider, candidate.model,
                std::string(e.what()), std::nullopt, std::nullopt});
            log()->debug("image-generation candidate failed: {}/{}", candidate.provider, candidate.model);
        }
    }

    // All candidates exhausted
    std::string errors;
    for (const auto& a : attempts) {
        if (a.error) {
            if (!errors.empty()) errors += "; ";
            errors += a.provider + ": " + *a.error;
        }
    }
    throw std::runtime_error("Image generation failed after trying all candidates: " + errors);
}

// ============================================================================
// Default Live Models (aligned: live-test-helpers.ts — 47 lines)
// ============================================================================

const std::map<std::string, std::string>& default_live_image_models() {
    static const std::map<std::string, std::string> models = {
        {"fal", "fal/fal-ai/flux/dev"},
        {"google", "google/gemini-3.1-flash-image-preview"},
        {"minimax", "minimax/image-01"},
        {"openai", "openai/gpt-image-1"},
        {"vydra", "vydra/grok-imagine"},
    };
    return models;
}

} // namespace hunterclaw::image_generation
