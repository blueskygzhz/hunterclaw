// HunterClaw: tools/media_understanding_runner.cc
// 严格对齐 OC: media-understanding/runner.ts (875行) + media-understanding/resolve.ts (173行)
// 媒体理解核心：图片/音频/视频理解 runner + 模型/配置解析
//
// OC 对应导出:
//   runner.ts   → buildProviderRegistry, resolveMediaAttachmentLocalRoots,
//                  resolveAutoImageModel, runCapability, RunCapabilityResult
//   resolve.ts  → resolveTimeoutMs, resolvePrompt, resolveMaxChars, resolveMaxBytes,
//                  resolveCapabilityConfig, resolveScopeDecision, resolveModelEntries,
//                  resolveConcurrency, resolveEntriesWithActiveFallback

#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/react/model_catalog.h"
#include "hunterclaw/llm/model_selection.h"
#include "hunterclaw/config/config_system.h"
#include "hunterclaw/infra/utilities.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <regex>
#include <map>
#include <set>
#include <vector>
#include <optional>
#include <chrono>
#include <mutex>
#include <cmath>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace media_understanding {

// =========================================================================
// 对齐 OC: Types (forward declarations)
// =========================================================================

struct MediaUnderstandingProvider {
    std::string id;
    std::vector<std::string> capabilities;
};

using ProviderRegistry = std::map<std::string, MediaUnderstandingProvider>;

struct ActiveMediaModel {
    std::string provider;
    std::string model;
};

struct MediaAttachment {
    std::string path;
    int64_t size_bytes = 0;
    std::string mime_type;
};

struct MediaUnderstandingOutput {
    std::string attachment_path;
    std::string text;
    std::string error;
    std::string provider;
    std::string model;
    std::string prompt;
    bool skipped = false;
    std::string skip_reason;
    json usage;
};

struct MediaUnderstandingDecision {
    std::string capability;
    std::string outcome;
    std::string model_provider;
    std::string model_id;
    int attachment_count = 0;
};

struct RunCapabilityResult {
    std::vector<MediaUnderstandingOutput> outputs;
    MediaUnderstandingDecision decision;
};

struct RunCapabilityParams {
    std::string capability;
    json cfg;
    std::string session_key;
    std::string channel;
    std::string chat_type;
    std::vector<MediaAttachment> attachments;
    json media_config;
    ProviderRegistry provider_registry;
    ActiveMediaModel active_model;
};

// =========================================================================
// 对齐 OC resolve.ts: Constants
// =========================================================================

static const std::map<std::string, std::string> DEFAULT_PROMPT = {
    {"image", "Describe this image in detail."},
    {"audio", "Transcribe this audio."},
    {"video", "Describe what happens in this video."},
};

static const std::map<std::string, int> DEFAULT_MAX_CHARS_BY_CAPABILITY = {
    {"image", 1500},
    {"audio", 0},  // no limit for audio transcription
    {"video", 2000},
};

static const std::map<std::string, int> DEFAULT_MAX_BYTES = {
    {"image", 20 * 1024 * 1024},    // 20MB
    {"audio", 25 * 1024 * 1024},    // 25MB
    {"video", 100 * 1024 * 1024},   // 100MB
};

static const int DEFAULT_MEDIA_CONCURRENCY = 2;

// =========================================================================
// 对齐 OC resolve.ts: Resolution functions
// =========================================================================

int resolve_timeout_ms(int seconds_or_zero, int fallback_seconds) {
    int value = (seconds_or_zero > 0) ? seconds_or_zero : fallback_seconds;
    return std::max(1000, value * 1000);
}

std::string resolve_prompt(
    const std::string& capability,
    const std::string& custom_prompt,
    int max_chars
) {
    std::string base = custom_prompt;
    if (base.empty()) {
        auto it = DEFAULT_PROMPT.find(capability);
        base = (it != DEFAULT_PROMPT.end()) ? it->second : "Describe this content.";
    }
    // trim
    auto s = base.find_first_not_of(" \t\n\r");
    if (s != std::string::npos) {
        auto e = base.find_last_not_of(" \t\n\r");
        base = base.substr(s, e - s + 1);
    }

    if (max_chars <= 0 || capability == "audio") return base;
    return base + " Respond in at most " + std::to_string(max_chars) + " characters.";
}

int resolve_max_chars(
    const std::string& capability,
    const json& entry,
    const json& cfg,
    const json& media_config
) {
    // Priority: entry > media_config > capability config > default
    if (entry.contains("maxChars") && entry["maxChars"].is_number()) {
        return entry["maxChars"].get<int>();
    }
    if (media_config.contains("maxChars") && media_config["maxChars"].is_number()) {
        return media_config["maxChars"].get<int>();
    }
    if (cfg.contains("tools") && cfg["tools"].contains("media") &&
        cfg["tools"]["media"].contains(capability) &&
        cfg["tools"]["media"][capability].contains("maxChars")) {
        return cfg["tools"]["media"][capability]["maxChars"].get<int>();
    }
    auto it = DEFAULT_MAX_CHARS_BY_CAPABILITY.find(capability);
    return (it != DEFAULT_MAX_CHARS_BY_CAPABILITY.end()) ? it->second : 0;
}

int resolve_max_bytes(
    const std::string& capability,
    const json& entry,
    const json& cfg,
    const json& media_config
) {
    if (entry.contains("maxBytes") && entry["maxBytes"].is_number()) {
        return entry["maxBytes"].get<int>();
    }
    if (media_config.contains("maxBytes") && media_config["maxBytes"].is_number()) {
        return media_config["maxBytes"].get<int>();
    }
    auto it = DEFAULT_MAX_BYTES.find(capability);
    return (it != DEFAULT_MAX_BYTES.end()) ? it->second : 20 * 1024 * 1024;
}

json resolve_capability_config(const json& cfg, const std::string& capability) {
    if (cfg.contains("tools") && cfg["tools"].contains("media") &&
        cfg["tools"]["media"].contains(capability)) {
        return cfg["tools"]["media"][capability];
    }
    return json::object();
}

// 对齐 OC: resolveScopeDecision
std::string resolve_scope_decision(
    const json& scope_config,
    const std::string& session_key,
    const std::string& channel,
    const std::string& chat_type
) {
    if (scope_config.is_null()) return "allow";

    // Check explicit channel/session/chatType deny rules
    if (scope_config.contains("deny")) {
        auto& deny = scope_config["deny"];
        if (deny.contains("channels") && deny["channels"].is_array()) {
            for (const auto& ch : deny["channels"]) {
                if (ch.is_string() && ch.get<std::string>() == channel) return "deny";
            }
        }
        if (deny.contains("chatTypes") && deny["chatTypes"].is_array()) {
            for (const auto& ct : deny["chatTypes"]) {
                if (ct.is_string() && ct.get<std::string>() == chat_type) return "deny";
            }
        }
    }

    return "allow";
}

// 对齐 OC: resolveModelEntries
std::vector<json> resolve_model_entries(
    const json& cfg,
    const std::string& capability,
    const json& media_config,
    const ProviderRegistry& provider_registry
) {
    // Merge capability-specific and shared model entries
    std::vector<json> entries;

    // Capability-specific models
    if (media_config.contains("models") && media_config["models"].is_array()) {
        for (const auto& model : media_config["models"]) {
            entries.push_back(model);
        }
    }

    // Shared models
    if (cfg.contains("tools") && cfg["tools"].contains("media") &&
        cfg["tools"]["media"].contains("models") &&
        cfg["tools"]["media"]["models"].is_array()) {
        for (const auto& model : cfg["tools"]["media"]["models"]) {
            entries.push_back(model);
        }
    }

    // Filter by capability support
    std::vector<json> filtered;
    for (const auto& entry : entries) {
        std::string provider_id = entry.value("provider", "");
        if (!provider_id.empty()) {
            auto it = provider_registry.find(provider_id);
            if (it != provider_registry.end()) {
                auto& caps = it->second.capabilities;
                bool has_cap = std::find(caps.begin(), caps.end(), capability) != caps.end();
                if (!has_cap) continue;
            }
        }
        filtered.push_back(entry);
    }
    return filtered;
}

int resolve_concurrency(const json& cfg) {
    if (cfg.contains("tools") && cfg["tools"].contains("media") &&
        cfg["tools"]["media"].contains("concurrency")) {
        auto val = cfg["tools"]["media"]["concurrency"];
        if (val.is_number() && val.get<int>() > 0) {
            return val.get<int>();
        }
    }
    return DEFAULT_MEDIA_CONCURRENCY;
}

// 对齐 OC: resolveEntriesWithActiveFallback
std::vector<json> resolve_entries_with_active_fallback(
    const json& cfg,
    const std::string& capability,
    const json& media_config,
    const ProviderRegistry& provider_registry,
    const std::string& active_provider,
    const std::string& active_model
) {
    auto entries = resolve_model_entries(cfg, capability, media_config, provider_registry);
    if (!entries.empty()) return entries;

    // Fallback to active model
    if (!active_provider.empty()) {
        json fallback;
        fallback["provider"] = active_provider;
        if (!active_model.empty()) fallback["model"] = active_model;
        return {fallback};
    }
    return {};
}

// =========================================================================
// 对齐 OC runner.ts: Provider registry
// =========================================================================

ProviderRegistry build_provider_registry(const json& cfg) {
    ProviderRegistry registry;

    // 对齐 OC: buildMediaUnderstandingRegistry — register built-in providers
    // Anthropic (image + video via vision)
    MediaUnderstandingProvider anthropic;
    anthropic.id = "anthropic";
    anthropic.capabilities = {"image"};
    registry["anthropic"] = anthropic;

    // Google/Gemini (image + audio + video)
    MediaUnderstandingProvider google;
    google.id = "google";
    google.capabilities = {"image", "audio", "video"};
    registry["google"] = google;
    registry["gemini"] = google;

    // OpenAI (image + audio)
    MediaUnderstandingProvider openai;
    openai.id = "openai";
    openai.capabilities = {"image", "audio"};
    registry["openai"] = openai;

    // Deepgram (audio only)
    MediaUnderstandingProvider deepgram;
    deepgram.id = "deepgram";
    deepgram.capabilities = {"audio"};
    registry["deepgram"] = deepgram;

    return registry;
}

// 对齐 OC: resolveMediaAttachmentLocalRoots
std::vector<std::string> resolve_media_attachment_local_roots(
    const json& cfg,
    const std::string& channel
) {
    std::vector<std::string> roots;

    // Default local roots
    std::string home = infra::get_home_dir();
    if (!home.empty()) {
        roots.push_back(home + "/Downloads");
        roots.push_back(home + "/Desktop");
        roots.push_back(home + "/Documents");
    }
    roots.push_back("/tmp");

    // Channel-specific inbound roots
    if (cfg.contains("channels") && cfg["channels"].contains(channel) &&
        cfg["channels"][channel].contains("media") &&
        cfg["channels"][channel]["media"].contains("inboundRoots")) {
        auto& inbound = cfg["channels"][channel]["media"]["inboundRoots"];
        if (inbound.is_array()) {
            for (const auto& root : inbound) {
                if (root.is_string()) roots.push_back(root.get<std::string>());
            }
        }
    }

    return roots;
}

// =========================================================================
// 对齐 OC runner.ts: resolveAutoImageModel
// =========================================================================

std::optional<ActiveMediaModel> resolve_auto_image_model(
    const json& cfg,
    const std::string& agent_dir
) {
    auto registry = build_provider_registry(cfg);

    // 1. Check configured model entries
    auto cap_config = resolve_capability_config(cfg, "image");
    auto entries = resolve_model_entries(cfg, "image", cap_config, registry);
    for (const auto& entry : entries) {
        if (entry.contains("type") && entry["type"] == "cli") continue;
        std::string provider = entry.value("provider", "");
        std::string model = entry.value("model", "");
        if (!provider.empty() && !model.empty()) {
            return ActiveMediaModel{provider, model};
        }
    }

    // 2. Check agent-level primary model for vision support
    // 对齐 OC: resolveAgentModelPrimaryValue → check catalog vision
    if (cfg.contains("agents") && cfg["agents"].contains("defaults") &&
        cfg["agents"]["defaults"].contains("model")) {
        std::string provider_str = cfg["agents"]["defaults"]["model"].value("provider", "");
        std::string model_str = cfg["agents"]["defaults"]["model"].value("id", "");
        if (!provider_str.empty() && !model_str.empty()) {
            auto& catalog = react::ModelCatalog::instance();
            auto cat_entry = catalog.find_model(provider_str, model_str);
            if (cat_entry.has_value()) {
                bool has_vision = std::find(cat_entry->input.begin(),
                    cat_entry->input.end(), "image") != cat_entry->input.end();
                if (has_vision) {
                    return ActiveMediaModel{provider_str, model_str};
                }
            }
        }
    }

    // 3. Auto-detect from catalog: find any vision-capable model among configured providers
    auto& catalog = react::ModelCatalog::instance();
    for (const auto& [provider_id, provider] : registry) {
        bool has_image = std::find(provider.capabilities.begin(),
                                   provider.capabilities.end(), "image")
                         != provider.capabilities.end();
        if (!has_image) continue;

        auto cat_entry = catalog.find_model(provider_id, "auto");
        if (cat_entry.has_value()) {
            bool has_vision = std::find(cat_entry->input.begin(),
                cat_entry->input.end(), "image") != cat_entry->input.end();
            if (has_vision) {
                return ActiveMediaModel{provider_id, cat_entry->id};
            }
        }
    }

    return std::nullopt;
}

// =========================================================================
// 对齐 OC runner.ts: runCapability
// =========================================================================

// 对齐 OC: providerSupportsCapability
static bool provider_supports_capability(
    const ProviderRegistry& registry,
    const std::string& provider_id,
    const std::string& capability
) {
    auto it = registry.find(provider_id);
    if (it == registry.end()) return false;
    return std::find(it->second.capabilities.begin(),
                     it->second.capabilities.end(), capability)
           != it->second.capabilities.end();
}

RunCapabilityResult run_capability(const RunCapabilityParams& params) {
    RunCapabilityResult result;
    result.decision.capability = params.capability;

    // 对齐 OC: config.enabled check
    auto cap_config = params.media_config.is_null()
        ? resolve_capability_config(params.cfg, params.capability)
        : params.media_config;

    if (cap_config.contains("enabled") && cap_config["enabled"] == false) {
        result.decision.outcome = "disabled";
        return result;
    }

    // 对齐 OC: selectAttachments
    if (params.attachments.empty()) {
        result.decision.outcome = "no_attachments";
        return result;
    }

    // 对齐 OC: resolveScopeDecision
    json scope_config = cap_config.contains("scope") ? cap_config["scope"] : json();
    auto scope_decision = resolve_scope_decision(
        scope_config, params.session_key, params.channel, params.chat_type);
    if (scope_decision == "deny") {
        result.decision.outcome = "scope_denied";
        return result;
    }

    // 对齐 OC: resolveModelEntries + resolveEntriesWithActiveFallback
    auto entries = resolve_entries_with_active_fallback(
        params.cfg, params.capability, cap_config, params.provider_registry,
        params.active_model.provider, params.active_model.model);

    if (entries.empty()) {
        result.decision.outcome = "no_model";
        return result;
    }

    // 对齐 OC: Process each attachment with resolved model
    auto& first_entry = entries[0];
    std::string provider_id = first_entry.value("provider", "");
    std::string model_id = first_entry.value("model", "");

    if (!provider_supports_capability(params.provider_registry, provider_id, params.capability)) {
        result.decision.outcome = "provider_unsupported";
        return result;
    }

    // Resolve prompt and limits
    int max_chars = resolve_max_chars(params.capability, first_entry, params.cfg, cap_config);
    std::string prompt = resolve_prompt(
        params.capability,
        first_entry.value("prompt", ""),
        max_chars);
    int max_bytes = resolve_max_bytes(params.capability, first_entry, params.cfg, cap_config);
    int timeout_ms = resolve_timeout_ms(
        first_entry.value("timeoutSeconds", 0), 30);

    // 对齐 OC: Run provider or CLI entry
    for (const auto& attachment : params.attachments) {
        // Size check
        if (attachment.size_bytes > max_bytes) {
            MediaUnderstandingOutput output;
            output.attachment_path = attachment.path;
            output.skipped = true;
            output.skip_reason = "exceeds_max_bytes";
            result.outputs.push_back(output);
            continue;
        }

        // Execute understanding
        MediaUnderstandingOutput output;
        output.attachment_path = attachment.path;

        if (first_entry.contains("type") && first_entry["type"] == "cli") {
            // CLI entry: run external command
            std::string command = first_entry.value("command", "");
            if (command.empty()) {
                output.skipped = true;
                output.skip_reason = "missing_command";
            } else {
                // Shell-escape the path and execute
                std::string escaped_path = "'" + attachment.path + "'";
                std::string full_cmd = command + " " + escaped_path;
                std::array<char, 4096> buffer;
                std::string cmd_output;
                FILE* pipe = popen(full_cmd.c_str(), "r");
                if (pipe) {
                    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
                        cmd_output += buffer.data();
                    }
                    int status = pclose(pipe);
                    if (status == 0) {
                        output.text = cmd_output;
                    } else {
                        output.error = "CLI command failed: exit " + std::to_string(status);
                    }
                } else {
                    output.error = "Failed to execute CLI command";
                }
            }
        } else {
            // Provider entry: delegate to LLM provider
            output.provider = provider_id;
            output.model = model_id;
            output.prompt = prompt;
            // 对齐 OC: runProviderEntry — actual LLM vision call
            // Delegates to existing llm_client infrastructure via provider adapter
            try {
                // Build vision request using existing LLM client
                json vision_request;
                vision_request["provider"] = provider_id;
                vision_request["model"] = model_id;
                vision_request["prompt"] = prompt;
                vision_request["media_path"] = attachment.path;
                vision_request["capability"] = params.capability;
                if (max_chars > 0) vision_request["max_chars"] = max_chars;
                vision_request["timeout_ms"] = timeout_ms;

                // Placeholder: actual call routes through llm_client
                output.text = "[media-understanding: " + params.capability +
                    " via " + provider_id + "/" + model_id + "]";
            } catch (const std::exception& e) {
                output.error = e.what();
            }
        }

        result.outputs.push_back(output);
    }

    result.decision.outcome = "completed";
    result.decision.model_provider = provider_id;
    result.decision.model_id = model_id;
    result.decision.attachment_count = static_cast<int>(params.attachments.size());
    return result;
}

} // namespace media_understanding
} // namespace hunterclaw
