// ============================================================================
// acp_session_mapper.cc
// 🎯 严格对齐 OpenClaw: src/acp/session-mapper.ts
// ============================================================================

#include "hunterclaw/acp/acp_session_mapper.h"
#include "hunterclaw/gateway/client.h"

#include <stdexcept>

namespace hunterclaw {
namespace acp {

namespace {

/// 対齐 JS String.prototype.trim()
std::string trim_copy(const std::string& s) {
    const char* ws = " \t\r\n\f\v";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

/// 対齐 packages/acp-core/src/meta.ts:4-27 readMetaValue + readString
/// 关键：归一化失败时**继续试下一个别名**，只有成功才返回。
std::string read_meta_string(const nlohmann::json& meta,
                             const std::vector<std::string>& keys) {
    for (const auto& k : keys) {
        if (!meta.contains(k) || !meta[k].is_string()) continue;
        const std::string v = trim_copy(meta[k].get<std::string>());
        if (v.empty()) continue;   // normalizeOptionalString：空视为不存在
        return v;
    }
    return std::string();
}

/// 対齐 meta.ts:30-35 readBool —— 仅接受真正的 boolean
std::optional<bool> read_meta_bool(const nlohmann::json& meta,
                                   const std::vector<std::string>& keys) {
    for (const auto& k : keys) {
        if (meta.contains(k) && meta[k].is_boolean()) return meta[k].get<bool>();
    }
    return std::nullopt;
}

/// 対齐 session-mapper.ts:42-48 等四处 `sessions.resolve` 调用：
/// 结果必须含非空 key，否则视为解析失败。
std::string request_resolved_key(const GatewayRequestFn& request_fn,
                                const std::string& param_name,
                                const std::string& param_value) {
    if (!request_fn) return std::string();
    const nlohmann::json result =
        request_fn("sessions.resolve", nlohmann::json{{param_name, param_value}});
    if (!result.is_object()) return std::string();
    if (!result.contains("key") || !result["key"].is_string()) return std::string();
    return trim_copy(result["key"].get<std::string>());
}

}  // namespace

AcpSessionMeta_Parsed parse_session_meta(const nlohmann::json& meta) {
    AcpSessionMeta_Parsed r;
    // 対齐:16-18
    if (meta.is_null() || !meta.is_object()) return r;

    // 対齐:21-25 —— 别名顺序不可调换
    if (auto s = read_meta_string(meta, {"sessionKey", "session", "key"}); !s.empty()) {
        r.session_key = s;
    }
    if (auto s = read_meta_string(meta, {"sessionLabel", "label"}); !s.empty()) {
        r.session_label = s;
    }
    r.reset_session    = read_meta_bool(meta, {"resetSession", "reset"});
    r.require_existing = read_meta_bool(meta, {"requireExistingSession", "requireExisting"});
    r.prefix_cwd       = read_meta_bool(meta, {"prefixCwd"});
    return r;
}

std::string resolve_session_key(const AcpSessionMeta_Parsed& meta,
                                const std::string& fallback_key,
                                const SessionKeyResolutionOptions& opts,
                                const GatewayRequestFn& request_fn) {
    // 対齐:36-39 —— 三个取值链
    const std::string requested_label =
        meta.session_label.has_value() ? *meta.session_label
                                       : opts.default_session_label.value_or("");
    const std::string requested_key =
        meta.session_key.has_value() ? *meta.session_key
                                     : opts.default_session_key.value_or("");
    // 対齐:38-39 —— meta → opts → false
    const bool require_existing =
        meta.require_existing.has_value()
            ? *meta.require_existing
            : opts.require_existing_session.value_or(false);

    // ① 対齐:41-49 —— meta.sessionLabel 优先，且**必须**查表换算
    if (meta.session_label.has_value() && !meta.session_label->empty()) {
        const std::string resolved =
            request_resolved_key(request_fn, "label", *meta.session_label);
        if (resolved.empty()) {
            // 対齐:46 —— 解析失败抛错，不可静默回落到 fallback，
            // 否则消息会被投到完全无关的会话。
            throw std::runtime_error("Unable to resolve session label: " +
                                     *meta.session_label);
        }
        return resolved;
    }

    // ② 対齐:51-62 —— meta.sessionKey：仅 requireExisting 时校验存在性
    if (meta.session_key.has_value() && !meta.session_key->empty()) {
        if (!require_existing) return *meta.session_key;
        const std::string resolved =
            request_resolved_key(request_fn, "key", *meta.session_key);
        if (resolved.empty()) {
            throw std::runtime_error("Session key not found: " + *meta.session_key);
        }
        return resolved;
    }

    // ③ 対齐:64-72 —— opts.defaultSessionLabel
    if (!requested_label.empty()) {
        const std::string resolved =
            request_resolved_key(request_fn, "label", requested_label);
        if (resolved.empty()) {
            throw std::runtime_error("Unable to resolve session label: " + requested_label);
        }
        return resolved;
    }

    // ④ 対齐:74-85 —— opts.defaultSessionKey
    if (!requested_key.empty()) {
        if (!require_existing) return requested_key;
        const std::string resolved =
            request_resolved_key(request_fn, "key", requested_key);
        if (resolved.empty()) {
            throw std::runtime_error("Session key not found: " + requested_key);
        }
        return resolved;
    }

    // ⑤ 対齐:87 —— 兜底
    return fallback_key;
}

bool reset_session_if_needed(const AcpSessionMeta_Parsed& meta,
                             const std::string& session_key,
                             const SessionKeyResolutionOptions& opts,
                             const GatewayRequestFn& request_fn) {
    // 対齐:97 —— meta → opts → false
    const bool reset =
        meta.reset_session.has_value() ? *meta.reset_session
                                       : opts.reset_session.value_or(false);
    // 対齐:98-100
    if (!reset) return false;
    if (!request_fn || session_key.empty()) return false;

    // 対齐:101 —— **sessions.reset**，不是 sessions.init。
    // init 只做初始化，不清历史；用它顶替 reset 会让 resetSession=true
    // 表面成功而实际历史仍在，后续 baseline/上下文全部错位。
    request_fn("sessions.reset", nlohmann::json{{"key", session_key}});
    return true;
}

// ── GatewayClient* 便捷重载 ──────────────────────────────────────────────────

namespace {

GatewayRequestFn make_gateway_request_fn(gateway::GatewayClient* gw) {
    if (!gw) return GatewayRequestFn();
    return [gw](const std::string& method, const nlohmann::json& params) -> nlohmann::json {
        auto result = gw->request<nlohmann::json>(method, params);
        if (!result.is_ok()) {
            // 让上层的 "resolve 失败即抛错" 语义得以生效
            throw std::runtime_error(method + " failed: " + result.error);
        }
        return result.get();
    };
}

}  // namespace

std::string resolve_session_key(const AcpSessionMeta_Parsed& meta,
                                const std::string& fallback_key,
                                const SessionKeyResolutionOptions& opts,
                                gateway::GatewayClient* gw) {
    return resolve_session_key(meta, fallback_key, opts, make_gateway_request_fn(gw));
}

bool reset_session_if_needed(const AcpSessionMeta_Parsed& meta,
                             const std::string& session_key,
                             const SessionKeyResolutionOptions& opts,
                             gateway::GatewayClient* gw) {
    return reset_session_if_needed(meta, session_key, opts, make_gateway_request_fn(gw));
}

} // namespace acp
} // namespace hunterclaw
