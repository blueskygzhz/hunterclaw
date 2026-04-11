// ============================================================================
// talk.cc - Voice/talk mode management
// 🎯 严格对齐 OpenClaw server-methods/talk.ts (463 行)
//
// 核心功能:
//   - talk.config: 获取 TTS/Talk 配置 (provider/voice/aliases)
//   - talk.mode: 设置 talk 模式 (对齐 validateTalkModeParams)
//   - talk.speak: 文本合成语音 (对齐 synthesizeSpeech flow)
//   - talk.enable / talk.disable: 启用/禁用 talk mode
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <algorithm>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static bool s_talk_enabled = false;
static std::string s_talk_mode = "off";

static inline int64_t talk_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// 对齐 talk.ts:42-45 canReadTalkSecrets
static bool can_read_talk_secrets(const ClientContext& ctx) {
    for (const auto& scope : ctx.connect.scopes) {
        if (scope == "operator.admin" || scope == "operator.talk-secrets") return true;
    }
    return false;
}

// 对齐 talk.ts:65-80 resolveTalkVoiceId
static std::string resolve_talk_voice_id(const json& provider_config, const std::string& requested) {
    if (requested.empty()) return "";
    if (!provider_config.contains("voiceAliases") || !provider_config["voiceAliases"].is_object())
        return requested;
    std::string lower_req = requested;
    std::transform(lower_req.begin(), lower_req.end(), lower_req.begin(), ::tolower);
    for (auto& [alias, voice_id] : provider_config["voiceAliases"].items()) {
        std::string lower_alias = alias;
        std::transform(lower_alias.begin(), lower_alias.end(), lower_alias.begin(), ::tolower);
        if (lower_alias == lower_req) return voice_id.get<std::string>();
    }
    return requested;
}

// 对齐 talk.ts:buildTalkConfigResponse — 构建完整 talk 配置响应
static json build_talk_config_response(std::shared_ptr<GatewayServer> /*server*/, bool include_secrets) {
    json result;
    result["enabled"] = s_talk_enabled;
    result["mode"] = s_talk_mode;

    // TTS 配置在 C++ 运行时通过 config 的 tts 字段获取
    // 目前返回基础状态；完整 TTS provider 配置待 TTS 模块对齐
    json tts_section;

    // Provider info
    std::string provider_id = tts_section.value("provider", "");
    result["provider"] = provider_id;

    // Voice config
    json voice_config;
    voice_config["voiceId"] = tts_section.value("voiceId", "");
    if (tts_section.contains("speed")) voice_config["speed"] = tts_section["speed"];
    if (tts_section.contains("pitch")) voice_config["pitch"] = tts_section["pitch"];
    if (tts_section.contains("format")) voice_config["format"] = tts_section["format"];
    result["voice"] = voice_config;

    // Provider config (对齐 talk.ts: resolveActiveTalkProviderConfig)
    json provider_config;
    if (tts_section.contains("providers") && tts_section["providers"].is_object() &&
        tts_section["providers"].contains(provider_id)) {
        provider_config = tts_section["providers"][provider_id];
    }
    // Redact API keys unless secrets scope
    if (!include_secrets && provider_config.contains("apiKey")) {
        std::string key = provider_config["apiKey"].get<std::string>();
        if (key.size() > 8) {
            provider_config["apiKey"] = key.substr(0, 4) + "..." + key.substr(key.size() - 4);
        } else {
            provider_config["apiKey"] = "***";
        }
    }
    result["providerConfig"] = provider_config;

    // Voice aliases
    if (provider_config.contains("voiceAliases"))
        result["voiceAliases"] = provider_config["voiceAliases"];

    return result;
}

// talk.config (对齐 talk.ts talkHandlers["talk.config"])
core::Result<json> method_talk_config(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> /*method_guard*/,
    const ClientContext& client_ctx,
    const json& /*params*/
) {
    return core::Result<json>::Ok(build_talk_config_response(server, can_read_talk_secrets(client_ctx)));
}

// talk.mode (对齐 talk.ts talkHandlers["talk.mode"])
core::Result<json> method_talk_mode(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> /*method_guard*/,
    const ClientContext& /*client_ctx*/,
    const json& params
) {
    std::string mode = params.value("mode", "");
    if (mode.empty()) {
        return core::Result<json>::Err("mode required");
    }
    // Validate mode (对齐 validateTalkModeParams)
    if (mode != "off" && mode != "voice" && mode != "realtime" && mode != "push-to-talk") {
        return core::Result<json>::Err("invalid mode: " + mode);
    }

    s_talk_mode = mode;
    s_talk_enabled = (mode != "off");

    // Broadcast mode change
    json event;
    event["mode"] = mode;
    event["ts"] = talk_now_ms();
    core::EventFrame ef;
    ef.event = "talk.mode";
    ef.payload = core::JsonValue(event);
    server->broadcast(ef);

    json result;
    result["ok"] = true;
    result["mode"] = mode;
    return core::Result<json>::Ok(result);
}

// talk.speak (对齐 talk.ts talkHandlers["talk.speak"])
core::Result<json> method_talk_speak(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> /*method_guard*/,
    const ClientContext& /*client_ctx*/,
    const json& params
) {
    std::string text = params.value("text", "");
    if (text.empty()) {
        return core::Result<json>::Err("text required");
    }
    std::string requested_voice = params.value("voiceId", "");

    // TTS 配置待完整 TTS 模块对齐
    json tts_section;

    std::string provider_id = tts_section.value("provider", "");
    if (provider_id.empty()) {
        json err;
        err["ok"] = false;
        err["error"] = "TTS not configured";
        err["reason"] = "talk_unconfigured";
        err["fallbackEligible"] = false;
        return core::Result<json>::Ok(err);
    }

    // Resolve voice (with aliases)
    json provider_config;
    if (tts_section.contains("providers") && tts_section["providers"].is_object() &&
        tts_section["providers"].contains(provider_id)) {
        provider_config = tts_section["providers"][provider_id];
    }
    std::string voice_id = resolve_talk_voice_id(provider_config, 
        requested_voice.empty() ? tts_section.value("voiceId", "") : requested_voice);

    // Synthesize (对齐 talk.ts: synthesizeSpeech call)
    // In production, would call the TTS provider. For now return stub with structured error.
    json result;
    result["ok"] = false;
    result["error"] = "TTS synthesis not yet implemented in C++ runtime";
    result["reason"] = "method_unavailable";
    result["fallbackEligible"] = true;
    result["provider"] = provider_id;
    result["voiceId"] = voice_id;
    return core::Result<json>::Ok(result);
}

// talk.enable (对齐 talk.ts)
core::Result<json> method_talk_enable(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    json mode_params;
    mode_params["mode"] = "voice";
    return method_talk_mode(server, method_guard, client_ctx, mode_params);
}

// talk.disable
core::Result<json> method_talk_disable(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    json mode_params;
    mode_params["mode"] = "off";
    return method_talk_mode(server, method_guard, client_ctx, mode_params);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
