// ============================================================================
// acp_session_mapper.cc
// 🎯 严格对齐 OpenClaw: src/acp/session-mapper.ts
// ============================================================================

#include "hunterclaw/acp/acp_session_mapper.h"
#include "hunterclaw/gateway/client.h"
#include "hunterclaw/core/types.h"

namespace hunterclaw {
namespace acp {

// ── parse_session_meta ────────────────────────────────────────────────────────
// 🎯 对齐 session-mapper.ts: parseSessionMeta(meta)
AcpSessionMeta_Parsed parse_session_meta(const nlohmann::json& meta) {
    AcpSessionMeta_Parsed r;
    if (meta.is_null() || !meta.is_object()) return r;

    // sessionKey: 三种别名风格
    for (auto& k : {"sessionKey", "session", "key"}) {
        if (meta.contains(k) && meta[k].is_string()) {
            r.session_key = meta[k].get<std::string>();
            break;
        }
    }

    // sessionLabel
    for (auto& k : {"sessionLabel", "label"}) {
        if (meta.contains(k) && meta[k].is_string()) {
            r.session_label = meta[k].get<std::string>();
            break;
        }
    }

    // resetSession
    for (auto& k : {"resetSession", "reset"}) {
        if (meta.contains(k) && meta[k].is_boolean()) {
            r.reset_session = meta[k].get<bool>();
            break;
        }
    }

    // requireExistingSession
    for (auto& k : {"requireExistingSession", "requireExisting"}) {
        if (meta.contains(k) && meta[k].is_boolean()) {
            r.require_existing = meta[k].get<bool>();
            break;
        }
    }

    // conversationId
    for (auto& k : {"conversationId", "conversation_id", "conversationID"}) {
        if (meta.contains(k) && meta[k].is_string()) {
            r.conversation_id = meta[k].get<std::string>();
            break;
        }
    }

    return r;
}

// ── resolve_session_key ─────────────────────────────────────────────────────
// 🎯 对齐 session-mapper.ts: resolveSessionKey(parsed, defaults, opts)
SessionKeyResolution resolve_session_key(
    const AcpSessionMeta_Parsed& parsed,
    const SessionKeyResolutionOptions& opts,
    std::function<std::optional<std::string>(const std::string&)> conversation_lookup) {

    SessionKeyResolution result;

    // 1. 请求显式 sessionKey
    if (parsed.session_key && !parsed.session_key->empty()) {
        result.session_key = *parsed.session_key;
        result.is_new = false;
        result.source = "explicit";
        return result;
    }

    // 2. conversationId → session_key 映射
    if (parsed.conversation_id && !parsed.conversation_id->empty() && conversation_lookup) {
        auto mapped = conversation_lookup(*parsed.conversation_id);
        if (mapped && !mapped->empty()) {
            result.session_key = *mapped;
            result.is_new = false;
            result.source = "conversation";
            return result;
        }
    }

    // 3. 服务端默认 session_key
    if (opts.default_session_key && !opts.default_session_key->empty()) {
        result.session_key = *opts.default_session_key;
        result.is_new = false;
        result.source = "default";
        return result;
    }

    // 4. 新建
    if (opts.allow_create) {
        result.session_key = "";
        result.is_new = true;
        result.source = "new";
        return result;
    }

    result.session_key = "";
    result.is_new = false;
    result.source = "none";
    return result;
}

// ── Core: reset_session_if_needed (callback-based) ───────────────────────────
bool reset_session_if_needed(
    const AcpSessionMeta_Parsed& parsed,
    const std::optional<GatewayRequestFn>& request_fn,
    const std::string& session_key,
    const std::optional<std::string>& session_label) {

    if (!parsed.reset_session.value_or(false)) return false;
    if (!request_fn) return false;
    if (session_key.empty()) return false;

    try {
        // 更新 label（如果有）
        if (session_label && !session_label->empty()) {
            auto& fn = *request_fn;
            nlohmann::json patch = {{"key", session_key}, {"label", *session_label}};
            try { fn("sessions.patch", patch); } catch (...) {}
        }

        // sessions.reset 语义：通过 sessions.init 重新初始化
        auto& fn = *request_fn;
        nlohmann::json init_params = {{"key", session_key}};
        if (session_label && !session_label->empty()) {
            init_params["label"] = *session_label;
        }
        fn("sessions.init", init_params);
        return true;
    } catch (...) {
        return false;
    }
}

// 🔧 gateway::GatewayClient* 简化版
bool reset_session_if_needed(
    const AcpSessionMeta_Parsed& parsed,
    gateway::GatewayClient* gw,
    const std::string& session_key,
    const std::optional<std::string>& session_label) {

    if (!parsed.reset_session.value_or(false)) return false;
    if (!gw || session_key.empty()) return false;

    try {
        if (session_label && !session_label->empty()) {
            nlohmann::json patch = {{"key", session_key}, {"label", *session_label}};
            try { gw->request("sessions.patch", patch); } catch (...) {}
        }
        nlohmann::json init_params = {{"key", session_key}};
        if (session_label && !session_label->empty()) {
            init_params["label"] = *session_label;
        }
        gw->request("sessions.init", init_params);
        return true;
    } catch (...) {
        return false;
    }
}

// ── Core: mark_session_with_acp_id (callback-based) ─────────────────────────
void mark_session_with_acp_id(
    const std::optional<GatewayRequestFn>& request_fn,
    const std::string& session_key,
    const std::string& acp_session_id) {

    if (!request_fn) return;
    if (session_key.empty() || acp_session_id.empty()) return;

    try {
        auto& fn = *request_fn;
        nlohmann::json patch_params = {
            {"key", session_key},
            {"meta", nlohmann::json{{"acp_session_id", acp_session_id}}}
        };
        fn("sessions.patch", patch_params);
    } catch (...) {}
}

// 🔧 gateway::GatewayClient* 简化版
void mark_session_with_acp_id(
    gateway::GatewayClient* gw,
    const std::string& session_key,
    const std::string& acp_session_id) {

    if (!gw || session_key.empty() || acp_session_id.empty()) return;

    try {
        nlohmann::json patch_params = {
            {"key", session_key},
            {"meta", nlohmann::json{{"acp_session_id", acp_session_id}}}
        };
        gw->request("sessions.patch", patch_params);
    } catch (...) {}
}

// ── Core: resolve_session_key_from_conversation_id (callback-based) ─────────
std::optional<std::string> resolve_session_key_from_conversation_id(
    const std::optional<GatewayRequestFn>& request_fn,
    const std::string& conversation_id) {

    if (!request_fn || conversation_id.empty()) return std::nullopt;

    try {
        auto& fn = *request_fn;
        auto result = fn("sessions.list", nlohmann::json{
            {"search", "acp:" + conversation_id}
        });

        if (!result.is_ok()) return std::nullopt;

        const auto& sessions = result.get().value("sessions", nlohmann::json::array());
        for (const auto& session : sessions) {
            if (!session.is_object()) continue;
            auto meta = session.value("meta", nlohmann::json::object());
            auto acp_id = meta.value("acp_session_id", "");
            if (acp_id == "acp:" + conversation_id) {
                auto key = session.value("key", "");
                if (!key.empty()) return key;
                auto sk = session.value("sessionKey", "");
                if (!sk.empty()) return sk;
            }
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

// 🔧 gateway::GatewayClient* 简化版
std::optional<std::string> resolve_session_key_from_conversation_id(
    gateway::GatewayClient* gw,
    const std::string& conversation_id) {

    if (!gw || conversation_id.empty()) return std::nullopt;

    try {
        auto result = gw->request("sessions.list", nlohmann::json{
            {"search", "acp:" + conversation_id}
        });

        if (!result.is_ok()) return std::nullopt;

        const auto& sessions = result.get().value("sessions", nlohmann::json::array());
        for (const auto& session : sessions) {
            if (!session.is_object()) continue;
            auto meta = session.value("meta", nlohmann::json::object());
            auto acp_id = meta.value("acp_session_id", "");
            if (acp_id == "acp:" + conversation_id) {
                auto key = session.value("key", "");
                if (!key.empty()) return key;
                auto sk = session.value("sessionKey", "");
                if (!sk.empty()) return sk;
            }
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace acp
} // namespace hunterclaw
