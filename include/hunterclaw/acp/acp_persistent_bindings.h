#pragma once

// ============================================================================
// acp_persistent_bindings.h
// 🎯 对齐 OpenClaw: src/acp/persistent-bindings.*.ts
// ============================================================================

#include "hunterclaw/acp/runtime_types.h"
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace core { class Config; }
namespace acp {

// ── Binding Config Shape ─────────────────────────────────────────────────────

struct AcpBindingConfigShape {
    std::optional<AcpRuntimeSessionMode> mode;
    std::string cwd;
    std::string backend;
    std::string label;
};

// ── Configured ACP Binding Spec ──────────────────────────────────────────────

struct ConfiguredAcpBindingSpec {
    std::string channel;
    std::string account_id;
    std::string conversation_id;
    std::string parent_conversation_id;
    std::string agent_id;
    std::string acp_agent_id;
    AcpRuntimeSessionMode mode = AcpRuntimeSessionMode::Persistent;
    std::string cwd;
    std::string backend;
    std::string label;
};

// ── Session Binding Record ───────────────────────────────────────────────────

struct SessionBindingRecord {
    std::string channel;
    std::string account_id;
    std::string conversation_id;
    std::string parent_conversation_id;
    std::string session_key;
    std::string label;
};

// ── Session ACP Meta (for binding matching) ──────────────────────────────────

struct SessionAcpMeta {
    std::string agent;
    AcpRuntimeSessionMode mode = AcpRuntimeSessionMode::Persistent;
    std::string backend;
    std::string cwd;
    std::string state;
};

// ── Binding Session Result ───────────────────────────────────────────────────

struct AcpBindingSessionResult {
    bool ok = false;
    std::string session_key;
    std::string error;
};

// ── Functions ────────────────────────────────────────────────────────────────

AcpRuntimeSessionMode normalize_acp_mode(const std::string& value);
AcpBindingConfigShape normalize_binding_config(const nlohmann::json& raw);
std::string build_configured_acp_session_key(const ConfiguredAcpBindingSpec& spec);
SessionBindingRecord to_configured_acp_binding_record(const ConfiguredAcpBindingSpec& spec);
AcpBindingSessionResult ensure_configured_acp_binding_session(
    const core::Config& cfg, const ConfiguredAcpBindingSpec& spec);
std::optional<ConfiguredAcpBindingSpec> resolve_configured_acp_binding_spec_by_session_key(
    const core::Config& cfg, const std::string& session_key);

// Utility
std::vector<std::string> split_string(const std::string& str, char delimiter);

} // namespace acp
} // namespace hunterclaw
