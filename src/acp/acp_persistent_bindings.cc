// ============================================================================
// acp_persistent_bindings.cc
// 🎯 严格对齐 OpenClaw:
//   - src/acp/persistent-bindings.types.ts (ConfiguredAcpBindingSpec, 类型)
//   - src/acp/persistent-bindings.lifecycle.ts (ensureConfiguredAcpBindingSession)
//   - src/acp/persistent-bindings.resolve.ts (resolveConfiguredAcpBindingSpec)
//   - src/routing/session-key.ts (normalizeAgentId / sanitizeAgentId)
// ============================================================================

#include "hunterclaw/acp/acp_persistent_bindings.h"
#include <openssl/sha.h>     // ✅ 对齐 OC: SHA-256 (node:crypto createHash)
#include <algorithm>
#include <functional>
#include <sstream>
#include <iomanip>
#include <cstring>

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

// ── sanitizeAgentId ──────────────────────────────────────────────────────────
// ✅ 严格对齐 OC src/routing/session-key.ts:91-109 normalizeAgentId
//
// OC 规则:
//   DEFAULT_AGENT_ID   = "main"
//   VALID_ID_RE        = /^[a-z0-9][a-z0-9_-]{0,63}$/i
//   INVALID_CHARS_RE   = /[^a-z0-9_-]+/g
//   1. trim + lowercase
//   2. 若为空 → "main"
//   3. 若匹配 VALID_ID_RE → 直接返回 lowercase 版本
//   4. 否则 collapse 非法字符为 "-", 剥离首尾 "-", 截断 64，若为空则 "main"
//
// 旧 HC 实现只 tolower，违反 OC 语义（会让 session_key 被非法字符污染）
static constexpr const char* DEFAULT_AGENT_ID = "main";

static bool is_valid_agent_id_char(char c) {
    // [a-z0-9_-]
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static std::string sanitize_agent_id(const std::string& value) {
    // 1. trim
    std::string trimmed = normalize_text(value);
    if (trimmed.empty()) return DEFAULT_AGENT_ID;

    // 2. lowercase
    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // 3. Check VALID_ID_RE: ^[a-z0-9][a-z0-9_-]{0,63}$ (applied to lowercase)
    auto is_valid = [](const std::string& s) -> bool {
        if (s.empty() || s.size() > 64) return false;
        char first = s[0];
        if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9'))) return false;
        for (size_t i = 1; i < s.size(); ++i) {
            if (!is_valid_agent_id_char(s[i])) return false;
        }
        return true;
    };
    if (is_valid(lower)) return lower;

    // 4. Fallback: collapse invalid chars to "-", strip leading/trailing "-", slice(0,64)
    std::string collapsed;
    collapsed.reserve(lower.size());
    bool prev_dash = false;
    for (char c : lower) {
        if (is_valid_agent_id_char(c)) {
            collapsed += c;
            prev_dash = (c == '-');
        } else {
            if (!prev_dash) {
                collapsed += '-';
                prev_dash = true;
            }
        }
    }
    // strip leading "-"
    size_t start = collapsed.find_first_not_of('-');
    if (start == std::string::npos) return DEFAULT_AGENT_ID;
    // strip trailing "-"
    size_t end = collapsed.find_last_not_of('-');
    collapsed = collapsed.substr(start, end - start + 1);
    if (collapsed.size() > 64) collapsed = collapsed.substr(0, 64);
    return collapsed.empty() ? DEFAULT_AGENT_ID : collapsed;
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
// ✅ 严格对齐 OC persistent-bindings.types.ts:60-69 buildBindingHash
//
// OC 实现:
//   createHash("sha256")
//     .update(`${channel}:${accountId}:${conversationId}`)
//     .digest("hex")
//     .slice(0, 16)
//
// 旧 HC 实现用了 FNV-1a 64-bit → hex(16)
// 问题：同一 binding spec 在 HC 和 OC 生成的 session_key 完全不同，
//       无法与 OC 共享 binding 存储或交叉匹配。修复为真 SHA-256。
static std::string build_binding_hash(const std::string& channel,
                                        const std::string& account_id,
                                        const std::string& conversation_id) {
    std::string input = channel + ":" + account_id + ":" + conversation_id;

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    // slice(0, 16) hex chars = 8 bytes = first 8 bytes of digest
    for (int i = 0; i < 8; ++i) {
        ss << std::setw(2) << static_cast<int>(digest[i]);
    }
    return ss.str();
}

// ── buildConfiguredAcpSessionKey ─────────────────────────────────────────────
// ✅ 严格对齐 OC persistent-bindings.types.ts:71-78
//   return `agent:${sanitizeAgentId(spec.agentId)}:acp:binding:${channel}:${accountId}:${hash}`;

std::string build_configured_acp_session_key(const ConfiguredAcpBindingSpec& spec) {
    std::string hash = build_binding_hash(
        spec.channel, spec.account_id, spec.conversation_id);

    // ✅ 对齐 OC: sanitizeAgentId（而非简单 tolower）
    std::string agent = sanitize_agent_id(spec.agent_id);

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
    const core::Config& /*cfg*/,
    const ConfiguredAcpBindingSpec& spec) {

    std::string session_key = build_configured_acp_session_key(spec);
    AcpBindingSessionResult result;
    result.session_key = session_key;

    // NOTE: Full implementation requires AcpSessionManager integration.
    // The binding lifecycle (match existing / reset / create) is wired
    // at the gateway layer where the session manager is available.
    result.ok = true;
    return result;
}

// ── resolveConfiguredAcpBindingSpecBySessionKey ──────────────────────────────
// 🎯 对齐 persistent-bindings.resolve.ts resolveConfiguredAcpBindingSpecBySessionKey

std::optional<ConfiguredAcpBindingSpec> resolve_configured_acp_binding_spec_by_session_key(
    const core::Config& /*cfg*/,
    const std::string& session_key) {

    // Parse session key format: agent:<agentId>:acp:binding:<channel>:<accountId>:<hash>
    auto parts = split_string(session_key, ':');
    if (parts.size() < 7) return std::nullopt;
    if (parts[0] != "agent" || parts[2] != "acp" || parts[3] != "binding") {
        return std::nullopt;
    }

    // NOTE: Full binding resolution requires iterating config bindings.
    // This is wired at the gateway layer where the full config is available.
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
