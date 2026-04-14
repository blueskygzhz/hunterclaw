// 🎯 Strict alignment: OpenClaw src/music-generation/
// Files aligned:
//   types.ts              (92 lines)  → GeneratedMusicAsset, MusicGenerationProvider, etc.
//   runtime.ts            (126 lines) → generateMusic() with fallback
//   runtime-types.ts      (42 lines)  → GenerateMusicParams, GenerateMusicRuntimeResult
//   provider-registry.ts  (78 lines)  → list/get music generation providers
//   normalization.ts      (106 lines) → resolveMusicGenerationOverrides
//   capabilities.ts       (48 lines)  → resolveMusicGenerationMode/ModeCapabilities
//   model-ref.ts          (8 lines)   → parseMusicGenerationModelRef

#include <hunterclaw/tools/tool.h>
#include <hunterclaw/core/types.h>
#include <hunterclaw/core/json.h>
#include <hunterclaw/core/logging.h>
#include <hunterclaw/config/config_system.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw::music_generation {

namespace {
auto log() { return core::get_logger("music-generation"); }
} // namespace

// ============================================================================
// Types (aligned: music-generation/types.ts — 92 lines)
// ============================================================================

struct GeneratedMusicAsset {
    std::vector<uint8_t> buffer;
    std::string mime_type;
    std::optional<std::string> file_name;
    nlohmann::json metadata;
};

// 🎯 对齐 OC: MusicGenerationOutputFormat
enum class MusicOutputFormat { kMp3, kWav };

inline std::string format_to_string(MusicOutputFormat f) {
    return f == MusicOutputFormat::kWav ? "wav" : "mp3";
}

inline std::optional<MusicOutputFormat> parse_format(const std::string& s) {
    if (s == "mp3") return MusicOutputFormat::kMp3;
    if (s == "wav") return MusicOutputFormat::kWav;
    return std::nullopt;
}

struct MusicGenerationSourceImage {
    std::optional<std::string> url;
    std::optional<std::vector<uint8_t>> buffer;
    std::optional<std::string> mime_type;
    std::optional<std::string> file_name;
    nlohmann::json metadata;
};

struct MusicGenerationIgnoredOverride {
    std::string key;   // "lyrics"|"instrumental"|"durationSeconds"|"format"
    std::string value;
};

enum class MusicGenerationMode { kGenerate, kEdit };

struct MusicGenerationModeCapabilities {
    std::optional<int> max_tracks;
    std::optional<int> max_duration_seconds;
    bool supports_lyrics = false;
    bool supports_instrumental = false;
    bool supports_duration = false;
    bool supports_format = false;
    std::vector<MusicOutputFormat> supported_formats;
    std::map<std::string, std::vector<MusicOutputFormat>> supported_formats_by_model;
};

struct MusicGenerationEditCapabilities : MusicGenerationModeCapabilities {
    bool enabled = false;
    std::optional<int> max_input_images;
};

struct MusicGenerationProviderCapabilities : MusicGenerationModeCapabilities {
    std::optional<int> max_input_images;
    std::optional<MusicGenerationModeCapabilities> generate;
    std::optional<MusicGenerationEditCapabilities> edit;
};

struct MusicGenerationNormalization {
    struct NumEntry {
        std::optional<int> requested;
        int applied;
    };
    std::optional<NumEntry> duration_seconds;
};

struct MusicGenerationRequest {
    std::string provider;
    std::string model;
    std::string prompt;
    nlohmann::json cfg;
    std::optional<std::string> agent_dir;
    std::optional<int> timeout_ms;
    std::optional<std::string> lyrics;
    std::optional<bool> instrumental;
    std::optional<int> duration_seconds;
    std::optional<MusicOutputFormat> format;
    std::vector<MusicGenerationSourceImage> input_images;
};

struct MusicGenerationResult {
    std::vector<GeneratedMusicAsset> tracks;
    std::optional<std::string> model;
    std::vector<std::string> lyrics;
    nlohmann::json metadata;
};

struct MusicGenerationProvider {
    std::string id;
    std::vector<std::string> aliases;
    std::optional<std::string> label;
    std::optional<std::string> default_model;
    std::vector<std::string> models;
    MusicGenerationProviderCapabilities capabilities;
    std::function<bool(const nlohmann::json&)> is_configured;
    std::function<MusicGenerationResult(const MusicGenerationRequest&)> generate_music;
};

// ============================================================================
// Capabilities (aligned: music-generation/capabilities.ts — 48 lines)
// ============================================================================

MusicGenerationMode resolve_music_generation_mode(int input_image_count) {
    return input_image_count > 0 ? MusicGenerationMode::kEdit : MusicGenerationMode::kGenerate;
}

const MusicGenerationModeCapabilities* resolve_mode_capabilities(
        const MusicGenerationProvider& provider, int input_image_count) {
    auto mode = resolve_music_generation_mode(input_image_count);
    if (mode == MusicGenerationMode::kGenerate) {
        return provider.capabilities.generate
            ? &*provider.capabilities.generate : &provider.capabilities;
    }
    return provider.capabilities.edit
        ? static_cast<const MusicGenerationModeCapabilities*>(&*provider.capabilities.edit)
        : nullptr;
}

// ============================================================================
// Provider Registry (aligned: music-generation/provider-registry.ts — 78 lines)
// ============================================================================

static std::vector<MusicGenerationProvider>& registered_providers() {
    static std::vector<MusicGenerationProvider> providers;
    return providers;
}

void register_music_generation_provider(const MusicGenerationProvider& provider) {
    registered_providers().push_back(provider);
}

std::vector<const MusicGenerationProvider*> list_music_generation_providers() {
    std::vector<const MusicGenerationProvider*> result;
    for (const auto& p : registered_providers()) result.push_back(&p);
    return result;
}

const MusicGenerationProvider* get_music_generation_provider(const std::string& id) {
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
// Normalization (aligned: music-generation/normalization.ts — 106 lines)
// ============================================================================

struct ResolvedMusicOverrides {
    std::optional<std::string> lyrics;
    std::optional<bool> instrumental;
    std::optional<int> duration_seconds;
    std::optional<MusicOutputFormat> format;
    std::vector<MusicGenerationIgnoredOverride> ignored_overrides;
    std::optional<MusicGenerationNormalization> normalization;
};

ResolvedMusicOverrides resolve_music_generation_overrides(
        const MusicGenerationProvider& provider, const std::string& model,
        const std::optional<std::string>& lyrics, const std::optional<bool>& instrumental,
        const std::optional<int>& duration_seconds, const std::optional<MusicOutputFormat>& format,
        int input_image_count) {

    auto* caps = resolve_mode_capabilities(provider, input_image_count);
    ResolvedMusicOverrides result;
    MusicGenerationNormalization normalization;
    auto out_lyrics = lyrics;
    auto out_instrumental = instrumental;
    auto out_duration = duration_seconds;
    auto out_format = format;

    if (!caps) {
        result.lyrics = out_lyrics;
        result.instrumental = out_instrumental;
        result.duration_seconds = out_duration;
        result.format = out_format;
        return result;
    }

    // Lyrics check
    if (out_lyrics && !out_lyrics->empty() && !caps->supports_lyrics) {
        result.ignored_overrides.push_back({"lyrics", *out_lyrics});
        out_lyrics = std::nullopt;
    }

    // Instrumental check
    if (out_instrumental && !caps->supports_instrumental) {
        result.ignored_overrides.push_back({"instrumental", *out_instrumental ? "true" : "false"});
        out_instrumental = std::nullopt;
    }

    // Duration check
    if (out_duration && !caps->supports_duration) {
        result.ignored_overrides.push_back({"durationSeconds", std::to_string(*out_duration)});
        out_duration = std::nullopt;
    } else if (out_duration && caps->max_duration_seconds) {
        int clamped = std::min(*out_duration, *caps->max_duration_seconds);
        if (clamped != *out_duration) {
            normalization.duration_seconds = MusicGenerationNormalization::NumEntry{
                *out_duration, clamped};
        }
        out_duration = clamped;
    }

    // Format check
    if (out_format) {
        auto model_formats_it = caps->supported_formats_by_model.find(model);
        const auto& supported = (model_formats_it != caps->supported_formats_by_model.end())
            ? model_formats_it->second : caps->supported_formats;
        if (!caps->supports_format ||
            (!supported.empty() && std::find(supported.begin(), supported.end(), *out_format) == supported.end())) {
            result.ignored_overrides.push_back({"format", format_to_string(*out_format)});
            out_format = std::nullopt;
        }
    }

    result.lyrics = out_lyrics;
    result.instrumental = out_instrumental;
    result.duration_seconds = out_duration;
    result.format = out_format;

    if (normalization.duration_seconds) {
        result.normalization = normalization;
    }

    return result;
}

// ============================================================================
// Runtime (aligned: music-generation/runtime.ts — 126 lines)
// ============================================================================

struct GenerateMusicParams {
    nlohmann::json cfg;
    std::string prompt;
    std::optional<std::string> agent_dir;
    std::optional<std::string> model_override;
    std::optional<std::string> lyrics;
    std::optional<bool> instrumental;
    std::optional<int> duration_seconds;
    std::optional<MusicOutputFormat> format;
    std::vector<MusicGenerationSourceImage> input_images;
};

struct FallbackAttempt {
    std::string provider;
    std::string model;
    std::optional<std::string> error;
};

struct GenerateMusicRuntimeResult {
    std::vector<GeneratedMusicAsset> tracks;
    std::string provider;
    std::string model;
    std::vector<FallbackAttempt> attempts;
    std::vector<std::string> lyrics;
    std::optional<MusicGenerationNormalization> normalization;
    nlohmann::json metadata;
    std::vector<MusicGenerationIgnoredOverride> ignored_overrides;
};

GenerateMusicRuntimeResult generate_music(const GenerateMusicParams& params) {
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
        auto model_config = defaults.value("musicGenerationModel", std::string(""));
        if (!model_config.empty()) {
            auto slash = model_config.find('/');
            if (slash != std::string::npos) {
                candidates.push_back({model_config.substr(0, slash), model_config.substr(slash + 1)});
            }
        }
    }

    if (candidates.empty()) {
        for (const auto* p : list_music_generation_providers()) {
            if (p->default_model) candidates.push_back({p->id, *p->default_model});
        }
    }

    if (candidates.empty()) {
        throw std::runtime_error(
            "No music-generation model configured. Set agents.defaults.musicGenerationModel. "
            "Example: google/lyria-3-clip-preview");
    }

    std::vector<FallbackAttempt> attempts;
    int input_image_count = static_cast<int>(params.input_images.size());

    for (const auto& candidate : candidates) {
        auto* provider = get_music_generation_provider(candidate.provider);
        if (!provider) {
            attempts.push_back({candidate.provider, candidate.model,
                "No music-generation provider registered for " + candidate.provider});
            continue;
        }

        try {
            auto sanitized = resolve_music_generation_overrides(
                *provider, candidate.model,
                params.lyrics, params.instrumental,
                params.duration_seconds, params.format, input_image_count);

            MusicGenerationRequest req;
            req.provider = candidate.provider;
            req.model = candidate.model;
            req.prompt = params.prompt;
            req.cfg = params.cfg;
            req.agent_dir = params.agent_dir;
            req.lyrics = sanitized.lyrics;
            req.instrumental = sanitized.instrumental;
            req.duration_seconds = sanitized.duration_seconds;
            req.format = sanitized.format;
            req.input_images = params.input_images;

            auto result = provider->generate_music(req);

            if (result.tracks.empty()) {
                throw std::runtime_error("Music generation provider returned no tracks.");
            }

            return {
                std::move(result.tracks), candidate.provider,
                result.model.value_or(candidate.model),
                std::move(attempts), result.lyrics,
                sanitized.normalization, result.metadata,
                std::move(sanitized.ignored_overrides)
            };

        } catch (const std::exception& e) {
            attempts.push_back({candidate.provider, candidate.model, std::string(e.what())});
            log()->debug("music-generation candidate failed: {}/{}", candidate.provider, candidate.model);
        }
    }

    std::string errors;
    for (const auto& a : attempts) {
        if (a.error) {
            if (!errors.empty()) errors += "; ";
            errors += a.provider + ": " + *a.error;
        }
    }
    throw std::runtime_error("Music generation failed: " + errors);
}

} // namespace hunterclaw::music_generation
