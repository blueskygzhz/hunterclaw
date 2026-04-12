// ============================================================================
// acp_persistent_bindings.cc
// 🎯 严格对齐 OpenClaw:
//   - src/acp/persistent-bindings.types.ts (ConfiguredAcpBindingSpec, 类型)
//   - src/acp/persistent-bindings.lifecycle.ts (ensureConfiguredAcpBindingSession)
//   - src/acp/persistent-bindings.resolve.ts (resolveConfiguredAcpBindingSpec)
// ============================================================================

#include "hunterclaw/acp/acp_persistent_bindings.h"
#include "hunterclaw/acp/acp_session_manager.h"
#include "hunterclaw/core/config.h"
#include <algorithm>
#include <functional>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>

namespace hunterclaw {
namespace acp {

// ── normalizeText ────────────────────────────────────────────────────────────
// 🎯 对齐 normalize-text.ts normalizeText(value)

static std::string normalize_text(const std::string& value) {
    std::string trimmed = value;
    // Trim whitespace
    auto start = trimmed.find_first_not_of(" \t\n\r");
    auto end = trimmed.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return trimmed.substr(start, end - start + 1);
}

// ── normalizeMode ────────────────────────────────────────────────────────────
// 🎯 对齐 persistent-bindings.types.ts normalizeMode(value)

AcpRuntimeSessionMode normalize_acp_mode(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto trimmed = normalize_text(lower);
    if (trimmed == "oneshot") return AcpRuntimeSessionMode::Oneshot;
    return AcpRuntimeSessionMode::Persistent;
}

// ── normalizeBindingConfig ───────────────────────────────────────────────────
// 🎯 对齐 persistent-bindings.types.ts normalizeBindingConfig(raw)

AcpBindingConfigShape normalize_binding_config(const nlohmann::json& raw) {
    AcpBindingConfigShape shape;
    if (raw.is_null() || !raw.is_object()) return shape;

    if (raw.contains("mode") && raw["mode"].is_string()) {
        shape.mode = normalize_acp_mode(raw["mode"].get<std::string>());
    }
    if (raw.contains("cwd") && raw["cwd"].is_string()) {
        shape.cwd = normalize_text(raw["cwd"].get<std::string>());
    }
    if (raw.contains("backend") && raw["backend"].is_string()) {
        shape.backend = normalize_text(raw["backend"].get<std::string>());
    }
    if (raw.contains("label") && raw["label"].is_string()) {
        shape.label = normalize_text(raw["label"].get<std::string>());
    }
    return shape;
}

// ── buildBindingHash ─────────────────────────────────────────────────────────
// 🎯 对齐 persistent-bindings.types.ts buildBindingHash(params)

static std::string build_binding_hash(const std::string& channel,
                                        const std::string& account_id,
                                        const std::string& conversation_id) {
    std::string input = channel + ":" + account_id + ":" + conversation_id;

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), hash);

    std::ostringstream ss;
    for (int i = 0; i < 8; i++) {  // 16 hex chars = 8 bytes
        ss << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<int>(hash[i]);
    }
    return ss.str();
}

// ── buildConfiguredAcpSessionKey ─────────────────────────────────────────────
// 🎯 对齐 persistent-bindings.types.ts buildConfiguredAcpSessionKey(spec)

std::string build_configured_acp_session_key(const ConfiguredAcpBindingSpec& spec) {
    std::string hash = build_binding_hash(
        spec.channel, spec.account_id, spec.conversation_id);

    // Sanitize agent ID (lowercase, alphanumeric + hyphens)
    std::string agent = spec.agent_id;
    std::transform(agent.begin(), agent.end(), agent.begin(), ::tolower);

    return "agent:" + agent + ":acp:binding:" + spec.channel +
           ":" + spec.account_id + ":" + hash;
}

// ── toConfiguredAcpBindingRecord ─────────────────────────────────────────────
// 🎯 对齐 persistent-bindings.types.ts toConfiguredAcpBindingRecord(spec)

SessionBindingRecord to_configured_acp_binding_record(const ConfiguredAcpBindingSpec& spec) {
    SessionBindingRecord record;
    record.channel = spec.channel;
    record.account_id = spec.account_id;
    record.conversation_id = spec.conversation_id;
    record.parent_conversation_id = spec.parent_conversation_id;
    record.session_key = build_configured_acp_session_key(spec);
    record.label = spec.label;
    return record;
}

// ── sessionMatchesConfiguredBinding ──────────────────────────────────────────
// 🎯 对齐 persistent-bindings.lifecycle.ts sessionMatchesConfiguredBinding(params)

static bool session_matches_configured_binding(
    const core::Config& cfg,
    const ConfiguredAcpBindingSpec& spec,
    const SessionAcpMeta& meta) {

    if (meta.state == "error") return false;

    // Check agent ID match
    std::string desired_agent = spec.acp_agent_id.empty()
        ? spec.agent_id : spec.acp_agent_id;
    std::string lower_desired = desired_agent;
    std::transform(lower_desired.begin(), lower_desired.end(),
                   lower_desired.begin(), ::tolower);

    std::string lower_current = meta.agent;
    std::transform(lower_current.begin(), lower_current.end(),
                   lower_current.begin(), ::tolower);

    if (lower_current.empty() || lower_current != lower_desired) return false;

    // Check mode match
    auto desired_mode = spec.mode;
    if (meta.mode != desired_mode) return false;

    // Check backend match
    std::string desired_backend = normalize_text(spec.backend);
    if (desired_backend.empty()) {
        desired_backend = normalize_text(cfg.acp_backend());
    }
    if (!desired_backend.empty()) {
        if (normalize_text(meta.backend) != desired_backend) return false;
    }

    // Check cwd match
    std::string desired_cwd = normalize_text(spec.cwd);
    if (!desired_cwd.empty()) {
        std::string current_cwd = normalize_text(meta.cwd);
        if (desired_cwd != current_cwd) return false;
    }

    return true;
}

// ── ensureConfiguredAcpBindingSession ─────────────────────────────────────────
// 🎯 对齐 persistent-bindings.lifecycle.ts ensureConfiguredAcpBindingSession(params)

AcpBindingSessionResult ensure_configured_acp_binding_session(
    const core::Config& cfg,
    const ConfiguredAcpBindingSpec& spec) {

    std::string session_key = build_configured_acp_session_key(spec);
    AcpBindingSessionResult result;
    result.session_key = session_key;

    // Try to read existing session meta
    auto& mgr = get_acp_session_manager();
    auto resolution = mgr.resolve_session(session_key);

    if (resolution.kind == AcpSessionResolution::Kind::Ready) {
        // Check if existing session matches the binding spec
        SessionAcpMeta meta;
        meta.agent = resolution.meta.agent;
        meta.mode = resolution.meta.acp_mode;
        meta.backend = resolution.meta.backend;
        meta.cwd = resolution.meta.cwd;
        meta.state = resolution.meta.state;

        if (session_matches_configured_binding(cfg, spec, meta)) {
            result.ok = true;
            return result;
        }

        // Session exists but doesn't match — needs reset
        mgr.reset_session(session_key);
    }

    // Create new session for binding
    try {
        std::string acp_agent = spec.acp_agent_id.empty()
            ? spec.agent_id : spec.acp_agent_id;

        auto mode_str = (spec.mode == AcpRuntimeSessionMode::Oneshot)
            ? "oneshot" : "persistent";

        mgr.create_acp_session(session_key, acp_agent, mode_str,
                                spec.cwd, spec.backend);
        result.ok = true;
    } catch (const std::exception& e) {
        result.ok = false;
        result.error = e.what();
    }

    return result;
}

// ── resolveConfiguredAcpBindingSpecBySessionKey ──────────────────────────────
// 🎯 对齐 persistent-bindings.resolve.ts resolveConfiguredAcpBindingSpecBySessionKey

std::optional<ConfiguredAcpBindingSpec> resolve_configured_acp_binding_spec_by_session_key(
    const core::Config& cfg,
    const std::string& session_key) {

    // Parse session key format: agent:<agentId>:acp:binding:<channel>:<accountId>:<hash>
    auto parts = split_string(session_key, ':');
    if (parts.size() < 7) return std::nullopt;
    if (parts[0] != "agent" || parts[2] != "acp" || parts[3] != "binding") {
        return std::nullopt;
    }

    // Look up in config bindings
    auto bindings = cfg.acp_bindings();
    for (auto& binding : bindings) {
        auto expected_key = build_configured_acp_session_key(binding);
        if (expected_key == session_key) {
            return binding;
        }
    }

    return std::nullopt;
}

// ── Utility ──────────────────────────────────────────────────────────────────

std::vector<std::string> split_string(const std::string& str, char delimiter) {
    std::vector<std::string> parts;
    std::istringstream stream(str);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(part);
    }
    return parts;
}

} // namespace acp
} // namespace hunterclaw
