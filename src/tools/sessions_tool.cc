// sessions_tool.cc
// Strictly aligned with OpenClaw:
//   src/agents/tools/sessions-list-tool.ts
//   src/agents/tools/sessions-send-tool.ts
//   src/agents/tools/sessions-spawn-tool.ts
//   src/agents/tools/sessions-history-tool.ts
//   src/agents/tools/session-status-tool.ts
//   src/agents/tools/sessions-helpers.ts
//   src/agents/tools/sessions-access.ts
//   src/agents/tools/sessions-resolution.ts
//   src/agents/subagent-spawn.ts (spawnSubagentDirect)
//   src/agents/acp-spawn.ts (spawnAcpDirect)

#include "hunterclaw/tools/sessions_tool.h"
#include "hunterclaw/sessions/session_key_utils.h"
#include "hunterclaw/routing/session_key.h"
#include "hunterclaw/gateway/client.h"
#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <chrono>
#include <random>

using json = nlohmann::json;

namespace hunterclaw {
namespace tools {

using namespace hunterclaw::sessions;
using namespace hunterclaw::routing;

// ============================================================================
// Constants (aligned with OpenClaw)
// ============================================================================

static constexpr int SESSIONS_HISTORY_MAX_BYTES = 80 * 1024;
static constexpr int SESSIONS_HISTORY_TEXT_MAX_CHARS = 4000;
static constexpr int SESSIONS_LIST_MESSAGE_LIMIT_CAP = 20;

// Aligned with OpenClaw: AGENT_LANE_NESTED
static const std::string AGENT_LANE_NESTED = "nested";
// Aligned with OpenClaw: INTERNAL_MESSAGE_CHANNEL
static const std::string INTERNAL_MESSAGE_CHANNEL = "internal";

// Aligned with OpenClaw: SUBAGENT_SPAWN_ACCEPTED_NOTE
static const std::string SUBAGENT_SPAWN_ACCEPTED_NOTE =
    "Auto-announce is push-based. After spawning children, do NOT call sessions_list, "
    "sessions_history, exec sleep, or any polling tool. Wait for completion events to arrive "
    "as user messages, track expected child session keys, and only send your final answer "
    "after ALL expected completions arrive. If a child completion event arrives AFTER your "
    "final answer, reply ONLY with NO_REPLY.";

static const std::string SUBAGENT_SPAWN_SESSION_ACCEPTED_NOTE =
    "thread-bound session stays active after this task; continue in-thread for follow-ups.";

// ============================================================================
// Helpers: UUID generation
// ============================================================================

static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    uint64_t hi = dis(gen);
    uint64_t lo = dis(gen);
    // Format as UUID v4
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        (uint32_t)(hi >> 32),
        (uint16_t)(hi >> 16),
        (uint16_t)(hi),
        (uint16_t)(lo >> 48),
        (unsigned long long)(lo & 0x0000FFFFFFFFFFFFULL));
    return std::string(buf);
}

// ============================================================================
// Helpers: string utilities
// ============================================================================

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

static std::string read_string_param(const json& params, const std::string& key,
                                     bool required = false) {
    if (params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    if (required) {
        throw std::runtime_error("Missing required parameter: " + key);
    }
    return "";
}

// ============================================================================
// Helpers: session key resolution (aligned with OpenClaw sessions-resolution.ts)
// ============================================================================

// Aligned with OpenClaw: resolveMainSessionAlias()
static void resolve_main_session_alias(const SessionsToolContext& ctx,
                                       std::string& main_key, std::string& alias) {
    main_key = ctx.main_key.empty() ? "main" : ctx.main_key;
    // scope "global" → alias = "global", else alias = main_key
    alias = (ctx.session_scope == "global") ? "global" : main_key;
}

// Aligned with OpenClaw: resolveDisplaySessionKey()
static std::string resolve_display_session_key(const std::string& key,
                                                const std::string& alias,
                                                const std::string& main_key) {
    if (key == alias || key == main_key) return "main";
    return key;
}

// Aligned with OpenClaw: resolveInternalSessionKey()
static std::string resolve_internal_session_key(const std::string& key,
                                                  const std::string& alias,
                                                  const std::string& main_key) {
    if (key == "main") return alias;
    return key;
}

// Aligned with OpenClaw: resolveSandboxedSessionToolContext()
static void resolve_sandboxed_session_tool_context(
    const SessionsToolContext& ctx,
    std::string& main_key_out,
    std::string& alias_out,
    std::string& requester_internal_key_out,
    std::string& effective_requester_key_out,
    bool& restrict_to_spawned_out
) {
    resolve_main_session_alias(ctx, main_key_out, alias_out);

    std::string requester_key = trim(ctx.agent_session_key);
    if (!requester_key.empty()) {
        requester_internal_key_out = resolve_internal_session_key(
            requester_key, alias_out, main_key_out);
    } else {
        requester_internal_key_out = "";
    }
    effective_requester_key_out = requester_internal_key_out.empty()
        ? alias_out
        : requester_internal_key_out;

    // restrictToSpawned: sandboxed + visibility=spawned + has requester key + not subagent
    bool visibility_spawned = (ctx.sandbox_session_visibility == "spawned");
    restrict_to_spawned_out = ctx.sandboxed
        && visibility_spawned
        && !requester_internal_key_out.empty()
        && !is_subagent_session_key(requester_internal_key_out);
}

// ============================================================================
// Helpers: session kind classification (aligned with OpenClaw sessions-helpers.ts)
// ============================================================================

// Aligned with OpenClaw: classifySessionKind()
static std::string classify_session_kind(const std::string& key,
                                          const std::string& gateway_kind,
                                          const std::string& alias,
                                          const std::string& main_key) {
    if (key == alias || key == main_key) return "main";
    if (key.substr(0, 5) == "cron:") return "cron";
    if (key.substr(0, 5) == "hook:") return "hook";
    if (key.substr(0, 5) == "node-" || key.substr(0, 5) == "node:") return "node";
    if (gateway_kind == "group") return "group";
    if (key.find(":group:") != std::string::npos ||
        key.find(":channel:") != std::string::npos) return "group";
    return "other";
}

// Aligned with OpenClaw: deriveChannel()
static std::string derive_channel(const std::string& key,
                                   const std::string& kind,
                                   const std::string& channel,
                                   const std::string& last_channel) {
    if (kind == "cron" || kind == "hook" || kind == "node") return "internal";
    std::string ch = trim(channel);
    if (!ch.empty()) return ch;
    std::string lch = trim(last_channel);
    if (!lch.empty()) return lch;
    // Parse from key: channel:group:id or channel:channel:id
    auto parts_str = key;
    std::vector<std::string> parts;
    std::istringstream ss(parts_str);
    std::string tok;
    while (std::getline(ss, tok, ':')) {
        if (!tok.empty()) parts.push_back(tok);
    }
    if (parts.size() >= 3 && (parts[1] == "group" || parts[1] == "channel")) {
        return parts[0];
    }
    return "unknown";
}

// ============================================================================
// Helpers: visibility guard (aligned with OpenClaw sessions-access.ts)
// ============================================================================

// Aligned with OpenClaw: createAgentToAgentPolicy()
struct A2APolicy {
    bool enabled = false;
    std::vector<std::string> allow_patterns;

    bool matches_allow(const std::string& agent_id) const {
        if (allow_patterns.empty()) return true;
        for (const auto& pattern : allow_patterns) {
            std::string p = trim(pattern);
            if (p.empty()) continue;
            if (p == "*") return true;
            if (p.find('*') == std::string::npos) {
                if (p == agent_id) return true;
            } else {
                // Simple wildcard: replace * with .*
                std::string regex_str;
                for (char c : p) {
                    if (c == '*') regex_str += ".*";
                    else if (std::string(".+?^${}()|[]\\").find(c) != std::string::npos)
                        regex_str += std::string("\\") + c;
                    else regex_str += c;
                }
                try {
                    std::regex re(regex_str, std::regex_constants::icase);
                    if (std::regex_match(agent_id, re)) return true;
                } catch (...) {}
            }
        }
        return false;
    }

    bool is_allowed(const std::string& requester_agent_id,
                    const std::string& target_agent_id) const {
        if (requester_agent_id == target_agent_id) return true;
        if (!enabled) return false;
        return matches_allow(requester_agent_id) && matches_allow(target_agent_id);
    }
};

// Aligned with OpenClaw: resolveEffectiveSessionToolsVisibility()
static std::string resolve_effective_visibility(const SessionsToolContext& ctx) {
    // Default is "tree"
    std::string vis = trim(ctx.sessions_visibility);
    if (vis != "self" && vis != "tree" && vis != "agent" && vis != "all") {
        vis = "tree";
    }
    if (!ctx.sandboxed) return vis;
    // sandboxed: clamp to "tree" if sandbox_session_visibility=spawned and vis != tree
    if (ctx.sandbox_session_visibility == "spawned" && vis != "tree") {
        return "tree";
    }
    return vis;
}

// Aligned with OpenClaw: createSessionVisibilityGuard() check()
struct VisibilityGuard {
    std::string action; // "list", "send", "history", "status"
    std::string requester_session_key;
    std::string requester_agent_id;
    std::string visibility; // "self", "tree", "agent", "all"
    A2APolicy a2a_policy;
    std::set<std::string> spawned_keys; // for "tree" visibility

    struct CheckResult {
        bool allowed;
        std::string error;
        std::string status; // "forbidden"
    };

    CheckResult check(const std::string& target_session_key) const {
        std::string target_agent_id = resolve_agent_id_from_session_key(target_session_key);
        bool is_cross_agent = (target_agent_id != requester_agent_id);

        if (is_cross_agent) {
            if (visibility != "all") {
                return {false, cross_visibility_message(), "forbidden"};
            }
            if (!a2a_policy.enabled) {
                return {false, a2a_disabled_message(), "forbidden"};
            }
            if (!a2a_policy.is_allowed(requester_agent_id, target_agent_id)) {
                return {false, a2a_denied_message(), "forbidden"};
            }
            return {true, "", ""};
        }

        if (visibility == "self" && target_session_key != requester_session_key) {
            return {false, self_visibility_message(), "forbidden"};
        }

        if (visibility == "tree"
            && target_session_key != requester_session_key
            && spawned_keys.find(target_session_key) == spawned_keys.end()) {
            return {false, tree_visibility_message(), "forbidden"};
        }

        return {true, "", ""};
    }

private:
    std::string action_prefix() const {
        if (action == "history") return "Session history";
        if (action == "send")    return "Session send";
        if (action == "status")  return "Session status";
        return "Session list";
    }
    std::string cross_visibility_message() const {
        if (action == "history")
            return "Session history visibility is restricted. Set tools.sessions.visibility=all to allow cross-agent access.";
        if (action == "send")
            return "Session send visibility is restricted. Set tools.sessions.visibility=all to allow cross-agent access.";
        if (action == "status")
            return "Session status visibility is restricted. Set tools.sessions.visibility=all to allow cross-agent access.";
        return "Session list visibility is restricted. Set tools.sessions.visibility=all to allow cross-agent access.";
    }
    std::string a2a_disabled_message() const {
        if (action == "history")
            return "Agent-to-agent history is disabled. Set tools.agentToAgent.enabled=true to allow cross-agent access.";
        if (action == "send")
            return "Agent-to-agent messaging is disabled. Set tools.agentToAgent.enabled=true to allow cross-agent sends.";
        if (action == "status")
            return "Agent-to-agent status is disabled. Set tools.agentToAgent.enabled=true to allow cross-agent access.";
        return "Agent-to-agent listing is disabled. Set tools.agentToAgent.enabled=true to allow cross-agent visibility.";
    }
    std::string a2a_denied_message() const {
        if (action == "history") return "Agent-to-agent history denied by tools.agentToAgent.allow.";
        if (action == "send")    return "Agent-to-agent messaging denied by tools.agentToAgent.allow.";
        if (action == "status")  return "Agent-to-agent status denied by tools.agentToAgent.allow.";
        return "Agent-to-agent listing denied by tools.agentToAgent.allow.";
    }
    std::string self_visibility_message() const {
        return action_prefix() + " visibility is restricted to the current session (tools.sessions.visibility=self).";
    }
    std::string tree_visibility_message() const {
        return action_prefix() + " visibility is restricted to the current session tree (tools.sessions.visibility=tree).";
    }
};

// Build visibility guard (fetches spawned keys from gateway for "tree" mode)
static VisibilityGuard build_visibility_guard(
    const std::string& action,
    const std::string& requester_session_key,
    const std::string& visibility,
    const A2APolicy& a2a_policy,
    gateway::GatewayClient* gw
) {
    VisibilityGuard guard;
    guard.action = action;
    guard.requester_session_key = requester_session_key;
    guard.requester_agent_id = resolve_agent_id_from_session_key(requester_session_key);
    guard.visibility = visibility;
    guard.a2a_policy = a2a_policy;

    // For "tree" visibility, fetch spawned session keys from gateway
    // Aligned with OpenClaw: listSpawnedSessionKeys()
    if (visibility == "tree" && gw) {
        try {
            auto result = gw->request("sessions.list", json{
                {"includeGlobal", false},
                {"includeUnknown", false},
                {"limit", 500},
                {"spawnedBy", requester_session_key}
            });
            if (result.is_ok()) {
                const auto& resp = result.get();
                if (resp.contains("sessions") && resp["sessions"].is_array()) {
                    for (const auto& entry : resp["sessions"]) {
                        if (entry.contains("key") && entry["key"].is_string()) {
                            std::string k = trim(entry["key"].get<std::string>());
                            if (!k.empty()) guard.spawned_keys.insert(k);
                        }
                    }
                }
            }
        } catch (...) {}
    }

    return guard;
}

// ============================================================================
// Helpers: session reference resolution (aligned with OpenClaw sessions-resolution.ts)
// ============================================================================

// Aligned with OpenClaw: looksLikeSessionId()
static bool looks_like_session_id(const std::string& value) {
    // UUID-like pattern: 8-4-4-4-12 hex chars
    static const std::regex uuid_re{
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
    };
    return std::regex_match(trim(value), uuid_re);
}

// Aligned with OpenClaw: looksLikeSessionKey()
static bool looks_like_session_key(const std::string& value) {
    std::string raw = trim(value);
    if (raw.empty()) return false;
    if (raw == "main" || raw == "global" || raw == "unknown") return true;
    if (is_acp_session_key(raw)) return true;
    if (raw.substr(0, 6) == "agent:") return true;
    if (raw.substr(0, 5) == "cron:" || raw.substr(0, 5) == "hook:") return true;
    if (raw.substr(0, 5) == "node-" || raw.substr(0, 5) == "node:") return true;
    if (raw.find(":group:") != std::string::npos ||
        raw.find(":channel:") != std::string::npos) return true;
    return false;
}

// Aligned with OpenClaw: shouldResolveSessionIdInput()
static bool should_resolve_session_id_input(const std::string& value) {
    return looks_like_session_id(value) || !looks_like_session_key(value);
}

struct SessionResolution {
    bool ok = false;
    std::string key;
    std::string display_key;
    bool resolved_via_session_id = false;
    std::string status; // "error" | "forbidden"
    std::string error;
};

// Aligned with OpenClaw: resolveSessionReference()
static SessionResolution resolve_session_reference(
    const std::string& session_key_param,
    const std::string& alias,
    const std::string& main_key,
    const std::string& requester_internal_key,
    bool restrict_to_spawned,
    gateway::GatewayClient* gw
) {
    std::string raw = trim(session_key_param);

    if (should_resolve_session_id_input(raw)) {
        // Try key-based resolution first
        if (gw) {
            try {
                json resolve_params = {{"key", raw}};
                if (restrict_to_spawned && !requester_internal_key.empty()) {
                    resolve_params["spawnedBy"] = requester_internal_key;
                }
                auto result = gw->request("sessions.resolve", resolve_params);
                if (result.is_ok()) {
                    const auto& resp = result.get();
                    if (resp.contains("key") && resp["key"].is_string()) {
                        std::string resolved_key = trim(resp["key"].get<std::string>());
                        if (!resolved_key.empty()) {
                            return SessionResolution{
                                true,
                                resolved_key,
                                resolve_display_session_key(resolved_key, alias, main_key),
                                false, "", ""
                            };
                        }
                    }
                }
            } catch (...) {}
        }

        // Try sessionId resolution
        if (gw) {
            try {
                json resolve_params = {{"sessionId", raw}};
                if (restrict_to_spawned && !requester_internal_key.empty()) {
                    resolve_params["spawnedBy"] = requester_internal_key;
                    resolve_params["includeGlobal"] = false;
                    resolve_params["includeUnknown"] = false;
                } else {
                    resolve_params["includeGlobal"] = true;
                    resolve_params["includeUnknown"] = true;
                }
                auto result = gw->request("sessions.resolve", resolve_params);
                if (result.is_ok()) {
                    const auto& resp = result.get();
                    if (resp.contains("key") && resp["key"].is_string()) {
                        std::string resolved_key = trim(resp["key"].get<std::string>());
                        if (!resolved_key.empty()) {
                            return SessionResolution{
                                true,
                                resolved_key,
                                resolve_display_session_key(resolved_key, alias, main_key),
                                true, "", ""
                            };
                        }
                    }
                }
            } catch (const std::exception& e) {
                if (restrict_to_spawned) {
                    return SessionResolution{
                        false, "", "", false, "forbidden",
                        "Session not visible from this sandboxed agent session: " + raw
                    };
                }
                return SessionResolution{
                    false, "", "", false, "error",
                    std::string(e.what()) + " (use the full sessionKey from sessions_list)"
                };
            }
        }

        if (restrict_to_spawned) {
            return SessionResolution{
                false, "", "", false, "forbidden",
                "Session not visible from this sandboxed agent session: " + raw
            };
        }
        return SessionResolution{
            false, "", "", false, "error",
            "Session not found: " + raw + " (use the full sessionKey from sessions_list)"
        };
    }

    // Direct key resolution
    std::string resolved_key = resolve_internal_session_key(raw, alias, main_key);
    std::string display_key = resolve_display_session_key(resolved_key, alias, main_key);
    return SessionResolution{true, resolved_key, display_key, false, "", ""};
}

// Aligned with OpenClaw: isResolvedSessionVisibleToRequester()
static bool is_resolved_session_visible(
    const std::string& requester_session_key,
    const std::string& target_session_key,
    bool restrict_to_spawned,
    bool resolved_via_session_id,
    gateway::GatewayClient* gw
) {
    if (!restrict_to_spawned) return true;
    if (resolved_via_session_id) return true;
    if (requester_session_key == target_session_key) return true;

    // Check spawned keys
    if (!gw) return false;
    try {
        auto result = gw->request("sessions.list", json{
            {"includeGlobal", false},
            {"includeUnknown", false},
            {"limit", 500},
            {"spawnedBy", requester_session_key}
        });
        if (result.is_ok()) {
            const auto& resp = result.get();
            if (resp.contains("sessions") && resp["sessions"].is_array()) {
                for (const auto& entry : resp["sessions"]) {
                    if (entry.contains("key") && entry["key"].is_string()) {
                        if (trim(entry["key"].get<std::string>()) == target_session_key) {
                            return true;
                        }
                    }
                }
            }
        }
    } catch (...) {}
    return false;
}

// ============================================================================
// Helpers: history sanitization (aligned with OpenClaw sessions-history-tool.ts)
// ============================================================================

// Aligned with OpenClaw: redactSensitiveText()
static std::string redact_sensitive_text(const std::string& text) {
    std::string result = text;
    // Redact API keys
    static const std::regex api_key_re{R"(\b(?:sk|api|key)[-_]?[a-zA-Z0-9]{20,}\b)"};
    result = std::regex_replace(result, api_key_re, "[REDACTED]");
    // Redact bearer tokens
    static const std::regex bearer_re{R"(\b(?:Bearer|bearer)\s+[a-zA-Z0-9\-_.]{20,}\b)"};
    result = std::regex_replace(result, bearer_re, "Bearer [REDACTED]");
    return result;
}

// Aligned with OpenClaw: truncateHistoryText()
struct TruncateResult {
    std::string text;
    bool truncated;
    bool redacted;
};

static TruncateResult truncate_history_text(const std::string& text) {
    std::string sanitized = redact_sensitive_text(text);
    bool redacted = (sanitized != text);
    if ((int)sanitized.size() <= SESSIONS_HISTORY_TEXT_MAX_CHARS) {
        return {sanitized, false, redacted};
    }
    std::string cut = sanitized.substr(0, SESSIONS_HISTORY_TEXT_MAX_CHARS);
    return {cut + "\n…(truncated)…", true, redacted};
}

// Aligned with OpenClaw: sanitizeHistoryMessage()
struct SanitizedMessage {
    json message;
    bool truncated;
    bool redacted;
};

static SanitizedMessage sanitize_history_message(const json& message) {
    if (!message.is_object()) {
        return {message, false, false};
    }
    json entry = message;
    bool truncated = false;
    bool redacted = false;

    // Remove details, usage, cost
    if (entry.contains("details")) { entry.erase("details"); truncated = true; }
    if (entry.contains("usage"))   { entry.erase("usage");   truncated = true; }
    if (entry.contains("cost"))    { entry.erase("cost");     truncated = true; }

    // Sanitize content
    if (entry.contains("content") && entry["content"].is_string()) {
        auto res = truncate_history_text(entry["content"].get<std::string>());
        entry["content"] = res.text;
        truncated |= res.truncated;
        redacted  |= res.redacted;
    } else if (entry.contains("content") && entry["content"].is_array()) {
        json new_content = json::array();
        for (auto& block : entry["content"]) {
            if (!block.is_object()) { new_content.push_back(block); continue; }
            json b = block;
            std::string type = b.value("type", "");

            if (b.contains("text") && b["text"].is_string()) {
                auto res = truncate_history_text(b["text"].get<std::string>());
                b["text"] = res.text;
                truncated |= res.truncated;
                redacted  |= res.redacted;
            }
            if (type == "thinking") {
                if (b.contains("thinking") && b["thinking"].is_string()) {
                    auto res = truncate_history_text(b["thinking"].get<std::string>());
                    b["thinking"] = res.text;
                    truncated |= res.truncated;
                    redacted  |= res.redacted;
                }
                // Remove thinkingSignature (large + not useful)
                if (b.contains("thinkingSignature")) {
                    b.erase("thinkingSignature");
                    truncated = true;
                }
            }
            if (b.contains("partialJson") && b["partialJson"].is_string()) {
                auto res = truncate_history_text(b["partialJson"].get<std::string>());
                b["partialJson"] = res.text;
                truncated |= res.truncated;
                redacted  |= res.redacted;
            }
            if (type == "image") {
                int bytes = 0;
                if (b.contains("data") && b["data"].is_string()) {
                    bytes = (int)b["data"].get<std::string>().size();
                    b.erase("data");
                    truncated = true;
                }
                b["omitted"] = true;
                if (bytes > 0) b["bytes"] = bytes;
            }
            new_content.push_back(b);
        }
        entry["content"] = new_content;
    }

    if (entry.contains("text") && entry["text"].is_string()) {
        auto res = truncate_history_text(entry["text"].get<std::string>());
        entry["text"] = res.text;
        truncated |= res.truncated;
        redacted  |= res.redacted;
    }

    return {entry, truncated, redacted};
}

// Aligned with OpenClaw: stripToolMessages()
static json strip_tool_messages(const json& messages) {
    json result = json::array();
    for (const auto& msg : messages) {
        if (msg.is_object()) {
            std::string role = msg.value("role", "");
            if (role == "toolResult" || role == "tool") continue;
        }
        result.push_back(msg);
    }
    return result;
}

// Aligned with OpenClaw: capArrayByJsonBytes() + enforceSessionsHistoryHardCap()
static json cap_history_by_bytes(const json& items, int max_bytes,
                                  bool& hard_capped_out) {
    hard_capped_out = false;
    std::string serialized = items.dump();
    if ((int)serialized.size() <= max_bytes) {
        return items;
    }

    // Try last item only
    if (!items.empty()) {
        json last_only = json::array();
        last_only.push_back(items.back());
        std::string last_str = last_only.dump();
        if ((int)last_str.size() <= max_bytes) {
            hard_capped_out = true;
            return last_only;
        }
    }

    // Placeholder
    hard_capped_out = true;
    json placeholder = json::array();
    placeholder.push_back(json{
        {"role", "assistant"},
        {"content", "[sessions_history omitted: message too large]"}
    });
    return placeholder;
}

// ============================================================================
// Tool context helpers
// ============================================================================

static A2APolicy build_a2a_policy(const SessionsToolContext& ctx) {
    A2APolicy policy;
    policy.enabled = ctx.a2a_enabled;
    policy.allow_patterns = ctx.a2a_allow;
    return policy;
}

// ============================================================================
// sessions_list Tool (aligned with OpenClaw sessions-list-tool.ts)
// ============================================================================

class SessionsListTool : public Tool {
public:
    explicit SessionsListTool(std::shared_ptr<gateway::GatewayClient> gw,
                               SessionsToolContext ctx)
        : gw_(std::move(gw)), ctx_(std::move(ctx)) {}

    std::string name() const override { return "sessions_list"; }

    std::string description() const override {
        return "List sessions with optional filters and last messages.";
    }

    Schema parameters() const override {
        return Schema{
            "object", "sessions_list parameters",
            {
                {"kinds",         json{{"type","array"},{"items",json{{"type","string"}}}}},
                {"limit",         json{{"type","number"},{"minimum",1}}},
                {"activeMinutes", json{{"type","number"},{"minimum",1}}},
                {"messageLimit",  json{{"type","number"},{"minimum",0}}},
            },
            {}
        };
    }

    ToolResult execute(const core::JsonValue& params_val) override {
        const json params = params_val.to_json();

        std::string main_key, alias, requester_internal_key, effective_requester_key;
        bool restrict_to_spawned;
        resolve_sandboxed_session_tool_context(ctx_,
            main_key, alias, requester_internal_key,
            effective_requester_key, restrict_to_spawned);

        std::string visibility = resolve_effective_visibility(ctx_);
        A2APolicy a2a_policy = build_a2a_policy(ctx_);

        // Parse kinds filter
        std::set<std::string> allowed_kinds;
        bool has_kinds_filter = false;
        if (params.contains("kinds") && params["kinds"].is_array()) {
            has_kinds_filter = true;
            static const std::set<std::string> valid_kinds =
                {"main", "group", "cron", "hook", "node", "other"};
            for (const auto& k : params["kinds"]) {
                if (k.is_string()) {
                    std::string kv = to_lower(trim(k.get<std::string>()));
                    if (valid_kinds.count(kv)) allowed_kinds.insert(kv);
                }
            }
        }

        // Parse limit
        std::optional<int> limit;
        if (params.contains("limit") && params["limit"].is_number()) {
            limit = std::max(1, (int)params["limit"].get<double>());
        }

        // Parse activeMinutes
        std::optional<int> active_minutes;
        if (params.contains("activeMinutes") && params["activeMinutes"].is_number()) {
            active_minutes = std::max(1, (int)params["activeMinutes"].get<double>());
        }

        // Parse messageLimit (cap at 20)
        int message_limit = 0;
        if (params.contains("messageLimit") && params["messageLimit"].is_number()) {
            message_limit = std::max(0, (int)params["messageLimit"].get<double>());
        }
        message_limit = std::min(message_limit, SESSIONS_LIST_MESSAGE_LIMIT_CAP);

        // Call gateway: sessions.list
        json list_params = {
            {"includeGlobal",  !restrict_to_spawned},
            {"includeUnknown", !restrict_to_spawned},
        };
        if (limit.has_value()) list_params["limit"] = limit.value();
        if (active_minutes.has_value()) list_params["activeMinutes"] = active_minutes.value();
        if (restrict_to_spawned) list_params["spawnedBy"] = effective_requester_key;

        json sessions_raw = json::array();
        if (gw_) {
            try {
                auto result = gw_->request("sessions.list", list_params);
                if (result.is_ok() && result.get().contains("sessions")) {
                    sessions_raw = result.get()["sessions"];
                }
            } catch (...) {}
        }

        // Build visibility guard
        VisibilityGuard vis_guard = build_visibility_guard(
            "list", effective_requester_key, visibility, a2a_policy, gw_.get());

        // Process sessions
        json rows = json::array();
        std::vector<std::pair<json*, std::string>> history_targets; // row ptr + resolved key

        for (const auto& entry : sessions_raw) {
            if (!entry.is_object()) continue;
            std::string key = entry.value("key", "");
            if (key.empty()) continue;

            auto access = vis_guard.check(key);
            if (!access.allowed) continue;

            if (key == "unknown") continue;
            if (key == "global" && alias != "global") continue;

            std::string gateway_kind = entry.value("kind", "");
            std::string kind = classify_session_kind(key, gateway_kind, alias, main_key);
            if (has_kinds_filter && !allowed_kinds.count(kind)) continue;

            std::string display_key = resolve_display_session_key(key, alias, main_key);

            // Delivery context
            std::string delivery_channel, delivery_to, delivery_account_id;
            if (entry.contains("deliveryContext") && entry["deliveryContext"].is_object()) {
                const auto& dc = entry["deliveryContext"];
                delivery_channel    = dc.value("channel", "");
                delivery_to         = dc.value("to", "");
                delivery_account_id = dc.value("accountId", "");
            }
            std::string last_channel = !delivery_channel.empty()
                ? delivery_channel
                : entry.value("lastChannel", "");
            std::string last_account_id = !delivery_account_id.empty()
                ? delivery_account_id
                : entry.value("lastAccountId", "");
            std::string last_to = !delivery_to.empty()
                ? delivery_to
                : entry.value("lastTo", "");

            std::string entry_channel = entry.value("channel", "");
            std::string derived_channel = derive_channel(key, kind, entry_channel, last_channel);

            json row = {
                {"key",     display_key},
                {"kind",    kind},
                {"channel", derived_channel},
            };
            if (!entry.value("label", "").empty())
                row["label"] = entry["label"];
            if (!entry.value("displayName", "").empty())
                row["displayName"] = entry["displayName"];
            if (!delivery_channel.empty() || !delivery_to.empty() || !delivery_account_id.empty()) {
                json dc_obj = json::object();
                if (!delivery_channel.empty())    dc_obj["channel"]   = delivery_channel;
                if (!delivery_to.empty())         dc_obj["to"]        = delivery_to;
                if (!delivery_account_id.empty()) dc_obj["accountId"] = delivery_account_id;
                row["deliveryContext"] = dc_obj;
            }
            if (entry.contains("updatedAt") && entry["updatedAt"].is_number())
                row["updatedAt"] = entry["updatedAt"];
            if (!entry.value("sessionId", "").empty())
                row["sessionId"] = entry["sessionId"];
            if (!entry.value("model", "").empty())
                row["model"] = entry["model"];
            if (entry.contains("contextTokens") && entry["contextTokens"].is_number())
                row["contextTokens"] = entry["contextTokens"];
            if (entry.contains("totalTokens") && entry["totalTokens"].is_number())
                row["totalTokens"] = entry["totalTokens"];
            if (!entry.value("thinkingLevel", "").empty())
                row["thinkingLevel"] = entry["thinkingLevel"];
            if (!entry.value("verboseLevel", "").empty())
                row["verboseLevel"] = entry["verboseLevel"];
            if (entry.contains("systemSent") && entry["systemSent"].is_boolean())
                row["systemSent"] = entry["systemSent"];
            if (entry.contains("abortedLastRun") && entry["abortedLastRun"].is_boolean())
                row["abortedLastRun"] = entry["abortedLastRun"];
            if (!entry.value("sendPolicy", "").empty())
                row["sendPolicy"] = entry["sendPolicy"];
            if (!last_channel.empty())    row["lastChannel"]   = last_channel;
            if (!last_to.empty())         row["lastTo"]        = last_to;
            if (!last_account_id.empty()) row["lastAccountId"] = last_account_id;

            rows.push_back(row);
        }

        // Fetch message history for each row if messageLimit > 0
        if (message_limit > 0 && gw_) {
            for (auto& row : rows) {
                std::string display_key = row.value("key", "");
                std::string resolved_key = resolve_internal_session_key(
                    display_key, alias, main_key);
                try {
                    auto hist_result = gw_->request("chat.history", json{
                        {"sessionKey", resolved_key},
                        {"limit", message_limit}
                    });
                    if (hist_result.is_ok()) {
                        json raw_msgs = hist_result.get().value("messages", json::array());
                        json filtered = strip_tool_messages(raw_msgs);
                        // Cap to messageLimit
                        if ((int)filtered.size() > message_limit) {
                            json capped = json::array();
                            int start = (int)filtered.size() - message_limit;
                            for (int i = start; i < (int)filtered.size(); ++i)
                                capped.push_back(filtered[i]);
                            filtered = capped;
                        }
                        row["messages"] = filtered;
                    }
                } catch (...) {}
            }
        }

        ToolResult result;
        result.content.push_back({ToolResultContent::Type::Json,
            json{{"count", rows.size()}, {"sessions", rows}}.dump()});
        result.success = true;
        return result;
    }

private:
    std::shared_ptr<gateway::GatewayClient> gw_;
    SessionsToolContext ctx_;
};

// ============================================================================
// sessions_send Tool (aligned with OpenClaw sessions-send-tool.ts)
// ============================================================================

class SessionsSendTool : public Tool {
public:
    explicit SessionsSendTool(std::shared_ptr<gateway::GatewayClient> gw,
                               SessionsToolContext ctx)
        : gw_(std::move(gw)), ctx_(std::move(ctx)) {}

    std::string name() const override { return "sessions_send"; }

    std::string description() const override {
        return "Send a message into another session. Use sessionKey or label to identify the target.";
    }

    Schema parameters() const override {
        return Schema{
            "object", "sessions_send parameters",
            {
                {"sessionKey",     json{{"type","string"}}},
                {"label",          json{{"type","string"},{"minLength",1},{"maxLength",64}}},
                {"agentId",        json{{"type","string"},{"minLength",1},{"maxLength",64}}},
                {"message",        json{{"type","string"}}},
                {"timeoutSeconds", json{{"type","number"},{"minimum",0}}},
            },
            {"message"}
        };
    }

    ToolResult execute(const core::JsonValue& params_val) override {
        const json params = params_val.to_json();

        std::string main_key, alias, requester_internal_key, effective_requester_key;
        bool restrict_to_spawned;
        resolve_sandboxed_session_tool_context(ctx_,
            main_key, alias, requester_internal_key,
            effective_requester_key, restrict_to_spawned);

        A2APolicy a2a_policy = build_a2a_policy(ctx_);
        std::string visibility = resolve_effective_visibility(ctx_);

        std::string message = read_string_param(params, "message", true);
        std::string session_key_param = read_string_param(params, "sessionKey");
        std::string label_param = trim(read_string_param(params, "label"));
        std::string label_agent_id_param = trim(read_string_param(params, "agentId"));

        std::string run_id = generate_uuid();

        // Cannot provide both sessionKey and label
        if (!session_key_param.empty() && !label_param.empty()) {
            return make_json_result(json{
                {"runId", run_id},
                {"status", "error"},
                {"error", "Provide either sessionKey or label (not both)."}
            });
        }

        std::string session_key = session_key_param;

        // Resolve by label
        if (session_key.empty() && !label_param.empty()) {
            std::string requester_agent_id = resolve_agent_id_from_session_key(effective_requester_key);
            std::string requested_agent_id = label_agent_id_param.empty()
                ? ""
                : normalize_agent_id(label_agent_id_param);

            if (restrict_to_spawned && !requested_agent_id.empty()
                && requested_agent_id != requester_agent_id) {
                return make_json_result(json{
                    {"runId", run_id},
                    {"status", "forbidden"},
                    {"error", "Sandboxed sessions_send label lookup is limited to this agent"}
                });
            }

            if (!requester_agent_id.empty() && !requested_agent_id.empty()
                && requested_agent_id != requester_agent_id) {
                if (!a2a_policy.enabled) {
                    return make_json_result(json{
                        {"runId", run_id},
                        {"status", "forbidden"},
                        {"error", "Agent-to-agent messaging is disabled. Set tools.agentToAgent.enabled=true to allow cross-agent sends."}
                    });
                }
                if (!a2a_policy.is_allowed(requester_agent_id, requested_agent_id)) {
                    return make_json_result(json{
                        {"runId", run_id},
                        {"status", "forbidden"},
                        {"error", "Agent-to-agent messaging denied by tools.agentToAgent.allow."}
                    });
                }
            }

            // Resolve label via gateway
            json resolve_params = {{"label", label_param}};
            if (!requested_agent_id.empty()) resolve_params["agentId"] = requested_agent_id;
            if (restrict_to_spawned) resolve_params["spawnedBy"] = effective_requester_key;

            std::string resolved_key;
            try {
                if (gw_) {
                    auto result = gw_->request("sessions.resolve", resolve_params);
                    if (result.is_ok() && result.get().contains("key")) {
                        resolved_key = trim(result.get()["key"].get<std::string>());
                    }
                }
            } catch (const std::exception& e) {
                if (restrict_to_spawned) {
                    return make_json_result(json{
                        {"runId", run_id},
                        {"status", "forbidden"},
                        {"error", "Session not visible from this sandboxed agent session."}
                    });
                }
                return make_json_result(json{
                    {"runId", run_id},
                    {"status", "error"},
                    {"error", std::string(e.what())}
                });
            }

            if (resolved_key.empty()) {
                if (restrict_to_spawned) {
                    return make_json_result(json{
                        {"runId", run_id},
                        {"status", "forbidden"},
                        {"error", "Session not visible from this sandboxed agent session."}
                    });
                }
                return make_json_result(json{
                    {"runId", run_id},
                    {"status", "error"},
                    {"error", "No session found with label: " + label_param}
                });
            }
            session_key = resolved_key;
        }

        if (session_key.empty()) {
            return make_json_result(json{
                {"runId", run_id},
                {"status", "error"},
                {"error", "Either sessionKey or label is required"}
            });
        }

        // Resolve session reference
        auto resolved = resolve_session_reference(
            session_key, alias, main_key,
            effective_requester_key, restrict_to_spawned, gw_.get());
        if (!resolved.ok) {
            return make_json_result(json{
                {"runId", run_id},
                {"status", resolved.status},
                {"error", resolved.error}
            });
        }

        // Check visibility for sandboxed sessions
        bool visible = is_resolved_session_visible(
            effective_requester_key, resolved.key,
            restrict_to_spawned, resolved.resolved_via_session_id, gw_.get());
        if (!visible) {
            return make_json_result(json{
                {"runId", run_id},
                {"status", "forbidden"},
                {"error", "Session not visible from this sandboxed agent session: " + session_key},
                {"sessionKey", resolved.display_key}
            });
        }

        std::string resolved_key = resolved.key;
        std::string display_key = resolved.display_key;

        // Visibility guard check
        VisibilityGuard vis_guard = build_visibility_guard(
            "send", effective_requester_key, visibility, a2a_policy, gw_.get());
        auto access = vis_guard.check(resolved_key);
        if (!access.allowed) {
            return make_json_result(json{
                {"runId", run_id},
                {"status", access.status},
                {"error", access.error},
                {"sessionKey", display_key}
            });
        }

        // Parse timeout
        int timeout_seconds = 30;
        if (params.contains("timeoutSeconds") && params["timeoutSeconds"].is_number()) {
            timeout_seconds = std::max(0, (int)params["timeoutSeconds"].get<double>());
        }
        int timeout_ms = timeout_seconds * 1000;
        int announce_timeout_ms = (timeout_seconds == 0) ? 30000 : timeout_ms;
        (void)announce_timeout_ms; // used for A2A flow

        std::string idempotency_key = generate_uuid();
        run_id = idempotency_key;

        // Build agent-to-agent message context (aligned with OpenClaw buildAgentToAgentMessageContext)
        std::string extra_system_prompt;
        if (!ctx_.agent_session_key.empty()) {
            extra_system_prompt = "[Inter-session message from " + ctx_.agent_session_key + "]";
        }

        // Send params (aligned with OpenClaw sessions-send-tool.ts)
        json send_params = {
            {"message",         message},
            {"sessionKey",      resolved_key},
            {"idempotencyKey",  idempotency_key},
            {"deliver",         false},
            {"channel",         INTERNAL_MESSAGE_CHANNEL},
            {"lane",            AGENT_LANE_NESTED},
            {"inputProvenance", json{
                {"kind",             "inter_session"},
                {"sourceSessionKey", ctx_.agent_session_key},
                {"sourceChannel",    ctx_.agent_channel},
                {"sourceTool",       "sessions_send"}
            }}
        };
        if (!extra_system_prompt.empty()) {
            send_params["extraSystemPrompt"] = extra_system_prompt;
        }

        // Start agent run
        if (!gw_) {
            return make_json_result(json{
                {"runId", run_id},
                {"status", "error"},
                {"error", "Gateway not available"},
                {"sessionKey", display_key}
            });
        }

        try {
            auto response = gw_->request<json>("agent", send_params);
            if (!response.is_ok()) {
                return make_json_result(json{
                    {"runId", run_id},
                    {"status", "error"},
                    {"error", response.error},
                    {"sessionKey", display_key}
                });
            }
            const auto& resp = response.get();
            if (resp.contains("runId") && resp["runId"].is_string()) {
                std::string resp_run_id = trim(resp["runId"].get<std::string>());
                if (!resp_run_id.empty()) run_id = resp_run_id;
            }
        } catch (const std::exception& e) {
            std::string msg = e.what();
            return make_json_result(json{
                {"runId", run_id},
                {"status", msg.find("gateway timeout") != std::string::npos ? "timeout" : "error"},
                {"error", msg},
                {"sessionKey", display_key}
            });
        }

        // If timeout=0, return accepted immediately (async A2A flow)
        if (timeout_seconds == 0) {
            return make_json_result(json{
                {"runId", run_id},
                {"status", "accepted"},
                {"sessionKey", display_key},
                {"delivery", json{{"status","pending"},{"mode","announce"}}}
            });
        }

        // Wait for agent run to complete
        std::string wait_status, wait_error;
        try {
            auto wait_result = gw_->request<json>("agent.wait", json{
                {"runId", run_id},
                {"timeoutMs", timeout_ms}
            });
            if (wait_result.is_ok()) {
                const auto& w = wait_result.get();
                wait_status = w.value("status", "");
                wait_error  = w.value("error", "");
            }
        } catch (const std::exception& e) {
            std::string msg = e.what();
            return make_json_result(json{
                {"runId", run_id},
                {"status", msg.find("gateway timeout") != std::string::npos ? "timeout" : "error"},
                {"error", msg},
                {"sessionKey", display_key}
            });
        }

        if (wait_status == "timeout") {
            return make_json_result(json{
                {"runId", run_id},
                {"status", "timeout"},
                {"error", wait_error},
                {"sessionKey", display_key}
            });
        }
        if (wait_status == "error") {
            return make_json_result(json{
                {"runId", run_id},
                {"status", "error"},
                {"error", wait_error.empty() ? "agent error" : wait_error},
                {"sessionKey", display_key}
            });
        }

        // Fetch reply from history
        std::string reply;
        try {
            auto hist_result = gw_->request<json>("chat.history", json{
                {"sessionKey", resolved_key},
                {"limit", 50}
            });
            if (hist_result.is_ok()) {
                json raw_msgs = hist_result.get().value("messages", json::array());
                json filtered = strip_tool_messages(raw_msgs);
                if (!filtered.empty()) {
                    const auto& last = filtered.back();
                    if (last.is_object() && last.value("role", "") == "assistant") {
                        if (last.contains("content") && last["content"].is_string()) {
                            reply = last["content"].get<std::string>();
                        } else if (last.contains("content") && last["content"].is_array()) {
                            for (const auto& block : last["content"]) {
                                if (block.is_object() && block.value("type", "") == "text") {
                                    reply += block.value("text", "");
                                }
                            }
                        }
                    }
                }
            }
        } catch (...) {}

        json result_json = {
            {"runId", run_id},
            {"status", "ok"},
            {"sessionKey", display_key},
            {"delivery", json{{"status","pending"},{"mode","announce"}}}
        };
        if (!reply.empty()) result_json["reply"] = reply;

        return make_json_result(result_json);
    }

private:
    std::shared_ptr<gateway::GatewayClient> gw_;
    SessionsToolContext ctx_;

    static ToolResult make_json_result(const json& data) {
        ToolResult result;
        result.content.push_back({ToolResultContent::Type::Json, data.dump()});
        result.success = true;
        return result;
    }
};

// ============================================================================
// sessions_spawn Tool (aligned with OpenClaw sessions-spawn-tool.ts +
//                      subagent-spawn.ts spawnSubagentDirect)
// ============================================================================

class SessionsSpawnTool : public Tool {
public:
    explicit SessionsSpawnTool(std::shared_ptr<gateway::GatewayClient> gw,
                                SessionsToolContext ctx)
        : gw_(std::move(gw)), ctx_(std::move(ctx)) {}

    std::string name() const override { return "sessions_spawn"; }

    std::string description() const override {
        return "Spawn an isolated session (runtime=\"subagent\" or runtime=\"acp\"). "
               "mode=\"run\" is one-shot and mode=\"session\" is persistent/thread-bound. "
               "Subagents inherit the parent workspace directory automatically.";
    }

    Schema parameters() const override {
        return Schema{
            "object", "sessions_spawn parameters",
            {
                {"task",              json{{"type","string"}}},
                {"label",             json{{"type","string"}}},
                {"runtime",           json{{"type","string"},{"enum",json::array({"subagent","acp"})}}},
                {"agentId",           json{{"type","string"}}},
                {"model",             json{{"type","string"}}},
                {"thinking",          json{{"type","string"}}},
                {"runTimeoutSeconds", json{{"type","number"},{"minimum",0}}},
                {"timeoutSeconds",    json{{"type","number"},{"minimum",0}}},
                {"thread",            json{{"type","boolean"}}},
                {"mode",              json{{"type","string"},{"enum",json::array({"run","session"})}}},
                {"cleanup",           json{{"type","string"},{"enum",json::array({"delete","keep"})}}},
                {"sandbox",           json{{"type","string"},{"enum",json::array({"inherit","require"})}}},
            },
            {"task"}
        };
    }

    ToolResult execute(const core::JsonValue& params_val) override {
        const json params = params_val.to_json();

        // Unsupported params check (aligned with OpenClaw)
        static const std::vector<std::string> unsupported_keys =
            {"target", "transport", "channel", "to", "threadId", "thread_id",
             "replyTo", "reply_to"};
        for (const auto& key : unsupported_keys) {
            if (params.contains(key)) {
                return make_json_result(json{
                    {"status", "error"},
                    {"error", "sessions_spawn does not support \"" + key +
                              "\". Use \"message\" or \"sessions_send\" for channel delivery."}
                });
            }
        }

        std::string task = read_string_param(params, "task", true);
        std::string label = trim(read_string_param(params, "label"));
        std::string runtime = params.value("runtime", "subagent");
        if (runtime != "subagent" && runtime != "acp") runtime = "subagent";

        // streamTo only for acp
        std::string stream_to = read_string_param(params, "streamTo");
        if (stream_to == "parent" && runtime != "acp") {
            return make_json_result(json{
                {"status", "error"},
                {"error", "streamTo is only supported for runtime=acp; got runtime=" + runtime}
            });
        }

        // resumeSessionId only for acp
        std::string resume_session_id = read_string_param(params, "resumeSessionId");
        if (!resume_session_id.empty() && runtime != "acp") {
            return make_json_result(json{
                {"status", "error"},
                {"error", "resumeSessionId is only supported for runtime=acp; got runtime=" + runtime}
            });
        }

        if (runtime == "acp") {
            // ACP spawn: not fully implemented, return error
            return make_json_result(json{
                {"status", "error"},
                {"error", "runtime=acp is not supported in this build"}
            });
        }

        // === subagent spawn (aligned with OpenClaw spawnSubagentDirect) ===

        std::string main_key, alias, requester_internal_key, effective_requester_key;
        bool restrict_to_spawned;
        resolve_sandboxed_session_tool_context(ctx_,
            main_key, alias, requester_internal_key,
            effective_requester_key, restrict_to_spawned);

        std::string requested_agent_id = trim(read_string_param(params, "agentId"));
        std::string model_override = read_string_param(params, "model");
        std::string thinking_override = read_string_param(params, "thinking");

        // Validate agentId format
        if (!requested_agent_id.empty() && !is_valid_agent_id(requested_agent_id)) {
            return make_json_result(json{
                {"status", "error"},
                {"error", "Invalid agentId \"" + requested_agent_id +
                          "\". Agent IDs must match [a-z0-9][a-z0-9_-]{0,63}. "
                          "Use agents_list to discover valid targets."}
            });
        }

        bool request_thread_binding = params.value("thread", false);
        std::string sandbox_mode = params.value("sandbox", "inherit");

        // Resolve spawn mode
        std::string mode_param = read_string_param(params, "mode");
        std::string spawn_mode;
        if (mode_param == "run" || mode_param == "session") {
            spawn_mode = mode_param;
        } else {
            spawn_mode = request_thread_binding ? "session" : "run";
        }

        if (spawn_mode == "session" && !request_thread_binding) {
            return make_json_result(json{
                {"status", "error"},
                {"error", "mode=\"session\" requires thread=true so the subagent can stay bound to a thread."}
            });
        }

        std::string cleanup = "keep";
        if (spawn_mode != "session") {
            std::string cleanup_param = read_string_param(params, "cleanup");
            if (cleanup_param == "keep" || cleanup_param == "delete") {
                cleanup = cleanup_param;
            }
        }

        // Timeout: prefer runTimeoutSeconds, fallback to timeoutSeconds
        int run_timeout_seconds = 0;
        if (params.contains("runTimeoutSeconds") && params["runTimeoutSeconds"].is_number()) {
            run_timeout_seconds = std::max(0, (int)params["runTimeoutSeconds"].get<double>());
        } else if (params.contains("timeoutSeconds") && params["timeoutSeconds"].is_number()) {
            run_timeout_seconds = std::max(0, (int)params["timeoutSeconds"].get<double>());
        }

        // Depth limit check (aligned with OpenClaw)
        int caller_depth = get_subagent_depth(effective_requester_key);
        int max_spawn_depth = ctx_.max_spawn_depth > 0 ? ctx_.max_spawn_depth : 5;
        if (caller_depth >= max_spawn_depth) {
            return make_json_result(json{
                {"status", "forbidden"},
                {"error", "sessions_spawn is not allowed at this depth (current depth: " +
                          std::to_string(caller_depth) + ", max: " +
                          std::to_string(max_spawn_depth) + ")"}
            });
        }

        // Max children check (aligned with OpenClaw)
        int max_children = ctx_.max_children_per_agent > 0 ? ctx_.max_children_per_agent : 5;
        int active_children = ctx_.active_children_count;
        if (active_children >= max_children) {
            return make_json_result(json{
                {"status", "forbidden"},
                {"error", "sessions_spawn has reached max active children for this session (" +
                          std::to_string(active_children) + "/" +
                          std::to_string(max_children) + ")"}
            });
        }

        // Resolve target agent ID
        std::string requester_agent_id = resolve_agent_id_from_session_key(effective_requester_key);
        std::string target_agent_id = requested_agent_id.empty()
            ? requester_agent_id
            : normalize_agent_id(requested_agent_id);

        // Cross-agent allowlist check (aligned with OpenClaw)
        if (target_agent_id != requester_agent_id) {
            bool allow_any = false;
            bool allow_target = false;
            for (const auto& allowed : ctx_.subagent_allow_agents) {
                std::string a = trim(allowed);
                if (a == "*") { allow_any = true; break; }
                if (normalize_agent_id(a) == target_agent_id) { allow_target = true; }
            }
            if (!allow_any && !allow_target) {
                std::string allowed_text;
                for (const auto& a : ctx_.subagent_allow_agents) {
                    if (!allowed_text.empty()) allowed_text += ", ";
                    allowed_text += normalize_agent_id(trim(a));
                }
                if (allowed_text.empty()) allowed_text = "none";
                return make_json_result(json{
                    {"status", "forbidden"},
                    {"error", "agentId is not allowed for sessions_spawn (allowed: " + allowed_text + ")"}
                });
            }
        }

        // Generate child session key
        std::string child_session_key = "agent:" + target_agent_id + ":subagent:" + generate_uuid();
        int child_depth = caller_depth + 1;

        if (!gw_) {
            return make_json_result(json{
                {"status", "error"},
                {"error", "Gateway not available"},
                {"childSessionKey", child_session_key}
            });
        }

        // Patch child session: spawnDepth, subagentRole, subagentControlScope
        std::string subagent_role = (child_depth >= max_spawn_depth) ? "leaf" : "orchestrator";
        std::string control_scope = (child_depth >= max_spawn_depth) ? "none" : "full";
        {
            auto patch_result = gw_->request("sessions.patch", json{
                {"key",                 child_session_key},
                {"spawnDepth",          child_depth},
                {"subagentRole",        subagent_role == "orchestrator" ? json(nullptr) : json(subagent_role)},
                {"subagentControlScope", control_scope}
            });
            if (!patch_result.is_ok()) {
                return make_json_result(json{
                    {"status", "error"},
                    {"error", patch_result.error},
                    {"childSessionKey", child_session_key}
                });
            }
        }

        // Patch model override
        bool model_applied = false;
        if (!model_override.empty()) {
            auto patch_result = gw_->request("sessions.patch", json{
                {"key",   child_session_key},
                {"model", model_override}
            });
            if (!patch_result.is_ok()) {
                return make_json_result(json{
                    {"status", "error"},
                    {"error", patch_result.error},
                    {"childSessionKey", child_session_key}
                });
            }
            model_applied = true;
        }

        // Patch thinking override
        if (!thinking_override.empty()) {
            json thinking_val = (thinking_override == "off") ? json(nullptr) : json(thinking_override);
            auto patch_result = gw_->request("sessions.patch", json{
                {"key",           child_session_key},
                {"thinkingLevel", thinking_val}
            });
            if (!patch_result.is_ok()) {
                return make_json_result(json{
                    {"status", "error"},
                    {"error", patch_result.error},
                    {"childSessionKey", child_session_key}
                });
            }
        }

        // Patch spawnedBy lineage
        {
            json lineage_patch = {
                {"key",       child_session_key},
                {"spawnedBy", effective_requester_key}
            };
            if (!ctx_.workspace_dir.empty()) {
                lineage_patch["spawnedWorkspaceDir"] = ctx_.workspace_dir;
            }
            auto patch_result = gw_->request("sessions.patch", lineage_patch);
            if (!patch_result.is_ok()) {
                // Cleanup
                try {
                    gw_->request("sessions.delete", json{
                        {"key", child_session_key},
                        {"emitLifecycleHooks", false},
                        {"deleteTranscript", true}
                    });
                } catch (...) {}
                return make_json_result(json{
                    {"status", "error"},
                    {"error", patch_result.error},
                    {"childSessionKey", child_session_key}
                });
            }
        }

        // Build child task message (aligned with OpenClaw)
        std::string child_task_message =
            "[Subagent Context] You are running as a subagent (depth " +
            std::to_string(child_depth) + "/" + std::to_string(max_spawn_depth) +
            "). Results auto-announce to your requester; do not busy-poll for status.";
        if (spawn_mode == "session") {
            child_task_message += "\n\n[Subagent Context] This subagent session is persistent "
                                  "and remains available for thread follow-up messages.";
        }
        child_task_message += "\n\n[Subagent Task]: " + task;

        // Build extra system prompt (aligned with OpenClaw buildSubagentSystemPrompt)
        std::string extra_system_prompt;
        if (!ctx_.agent_session_key.empty()) {
            extra_system_prompt = "[Subagent spawned by: " + ctx_.agent_session_key + "]";
        }
        if (!label.empty()) {
            extra_system_prompt += "\n[Label: " + label + "]";
        }

        // Dispatch agent run (aligned with OpenClaw callGateway('agent', ...))
        std::string child_idem = generate_uuid();
        std::string child_run_id = child_idem;

        json agent_params = {
            {"message",         child_task_message},
            {"sessionKey",      child_session_key},
            {"idempotencyKey",  child_idem},
            {"deliver",         false},
            {"lane",            "subagent"},
            {"timeout",         run_timeout_seconds},
        };
        if (!ctx_.agent_channel.empty())
            agent_params["channel"] = ctx_.agent_channel;
        if (!ctx_.agent_to.empty())
            agent_params["to"] = ctx_.agent_to;
        if (!ctx_.agent_account_id.empty())
            agent_params["accountId"] = ctx_.agent_account_id;
        if (!extra_system_prompt.empty())
            agent_params["extraSystemPrompt"] = extra_system_prompt;
        if (!thinking_override.empty())
            agent_params["thinking"] = thinking_override;
        if (!label.empty())
            agent_params["label"] = label;
        if (!ctx_.workspace_dir.empty())
            agent_params["workspaceDir"] = ctx_.workspace_dir;

        try {
            auto response = gw_->request<json>("agent", agent_params);
            if (!response.is_ok()) {
                // Cleanup
                try {
                    gw_->request("sessions.delete", json{
                        {"key", child_session_key},
                        {"deleteTranscript", true},
                        {"emitLifecycleHooks", false}
                    });
                } catch (...) {}
                return make_json_result(json{
                    {"status", "error"},
                    {"error", response.error},
                    {"childSessionKey", child_session_key},
                    {"runId", child_run_id}
                });
            }
            const auto& resp = response.get();
            if (resp.contains("runId") && resp["runId"].is_string()) {
                std::string resp_run_id = trim(resp["runId"].get<std::string>());
                if (!resp_run_id.empty()) child_run_id = resp_run_id;
            }
        } catch (const std::exception& e) {
            // Cleanup
            try {
                gw_->request("sessions.delete", json{
                    {"key", child_session_key},
                    {"deleteTranscript", true},
                    {"emitLifecycleHooks", false}
                });
            } catch (...) {}
            return make_json_result(json{
                {"status", "error"},
                {"error", std::string(e.what())},
                {"childSessionKey", child_session_key},
                {"runId", child_run_id}
            });
        }

        // Determine note (aligned with OpenClaw)
        bool is_cron_session = is_cron_session_key(ctx_.agent_session_key);
        std::string note;
        if (spawn_mode == "session") {
            note = SUBAGENT_SPAWN_SESSION_ACCEPTED_NOTE;
        } else if (!is_cron_session) {
            note = SUBAGENT_SPAWN_ACCEPTED_NOTE;
        }

        json result_json = {
            {"status",          "accepted"},
            {"childSessionKey", child_session_key},
            {"runId",           child_run_id},
            {"mode",            spawn_mode},
        };
        if (!note.empty()) result_json["note"] = note;
        if (!model_override.empty()) result_json["modelApplied"] = model_applied;

        return make_json_result(result_json);
    }

private:
    std::shared_ptr<gateway::GatewayClient> gw_;
    SessionsToolContext ctx_;

    static ToolResult make_json_result(const json& data) {
        ToolResult result;
        result.content.push_back({ToolResultContent::Type::Json, data.dump()});
        result.success = true;
        return result;
    }
};

// ============================================================================
// sessions_history Tool (aligned with OpenClaw sessions-history-tool.ts)
// ============================================================================

class SessionsHistoryTool : public Tool {
public:
    explicit SessionsHistoryTool(std::shared_ptr<gateway::GatewayClient> gw,
                                  SessionsToolContext ctx)
        : gw_(std::move(gw)), ctx_(std::move(ctx)) {}

    std::string name() const override { return "sessions_history"; }

    std::string description() const override {
        return "Fetch message history for a session.";
    }

    Schema parameters() const override {
        return Schema{
            "object", "sessions_history parameters",
            {
                {"sessionKey",    json{{"type","string"}}},
                {"limit",         json{{"type","number"},{"minimum",1}}},
                {"includeTools",  json{{"type","boolean"}}},
            },
            {"sessionKey"}
        };
    }

    ToolResult execute(const core::JsonValue& params_val) override {
        const json params = params_val.to_json();

        std::string session_key_param = read_string_param(params, "sessionKey", true);

        std::string main_key, alias, requester_internal_key, effective_requester_key;
        bool restrict_to_spawned;
        resolve_sandboxed_session_tool_context(ctx_,
            main_key, alias, requester_internal_key,
            effective_requester_key, restrict_to_spawned);

        // Resolve session reference
        auto resolved = resolve_session_reference(
            session_key_param, alias, main_key,
            effective_requester_key, restrict_to_spawned, gw_.get());
        if (!resolved.ok) {
            return make_json_result(json{
                {"status", resolved.status},
                {"error",  resolved.error}
            });
        }

        // Check sandboxed visibility
        bool visible = is_resolved_session_visible(
            effective_requester_key, resolved.key,
            restrict_to_spawned, resolved.resolved_via_session_id, gw_.get());
        if (!visible) {
            return make_json_result(json{
                {"status", "forbidden"},
                {"error",  "Session not visible from this sandboxed agent session: " + session_key_param}
            });
        }

        std::string resolved_key = resolved.key;
        std::string display_key = resolved.display_key;

        // Visibility guard
        A2APolicy a2a_policy = build_a2a_policy(ctx_);
        std::string visibility = resolve_effective_visibility(ctx_);
        VisibilityGuard vis_guard = build_visibility_guard(
            "history", effective_requester_key, visibility, a2a_policy, gw_.get());
        auto access = vis_guard.check(resolved_key);
        if (!access.allowed) {
            return make_json_result(json{
                {"status", access.status},
                {"error",  access.error}
            });
        }

        // Parse params
        std::optional<int> limit;
        if (params.contains("limit") && params["limit"].is_number()) {
            limit = std::max(1, (int)params["limit"].get<double>());
        }
        bool include_tools = params.value("includeTools", false);

        // Fetch history from gateway
        if (!gw_) {
            return make_json_result(json{
                {"status", "error"},
                {"error",  "Gateway not available"}
            });
        }

        json hist_params = {{"sessionKey", resolved_key}};
        if (limit.has_value()) hist_params["limit"] = limit.value();

        json raw_messages = json::array();
        try {
            auto result = gw_->request("chat.history", hist_params);
            if (result.is_ok()) {
                raw_messages = result.get().value("messages", json::array());
            }
        } catch (const std::exception& e) {
            return make_json_result(json{
                {"status", "error"},
                {"error",  std::string(e.what())}
            });
        }

        // Filter tool messages
        json selected_messages = include_tools ? raw_messages : strip_tool_messages(raw_messages);

        // Sanitize messages
        bool content_truncated = false;
        bool content_redacted = false;
        json sanitized_messages = json::array();
        for (const auto& msg : selected_messages) {
            auto sanitized = sanitize_history_message(msg);
            content_truncated |= sanitized.truncated;
            content_redacted  |= sanitized.redacted;
            sanitized_messages.push_back(sanitized.message);
        }

        // Cap by bytes
        bool hard_capped = false;
        json capped_messages = cap_history_by_bytes(
            sanitized_messages, SESSIONS_HISTORY_MAX_BYTES, hard_capped);

        bool dropped_messages = (int)capped_messages.size() < (int)selected_messages.size();

        // Calculate bytes
        int bytes = (int)capped_messages.dump().size();

        return make_json_result(json{
            {"sessionKey",      display_key},
            {"messages",        capped_messages},
            {"truncated",       dropped_messages || content_truncated || hard_capped},
            {"droppedMessages", dropped_messages || hard_capped},
            {"contentTruncated", content_truncated},
            {"contentRedacted",  content_redacted},
            {"bytes",           bytes}
        });
    }

private:
    std::shared_ptr<gateway::GatewayClient> gw_;
    SessionsToolContext ctx_;

    static ToolResult make_json_result(const json& data) {
        ToolResult result;
        result.content.push_back({ToolResultContent::Type::Json, data.dump()});
        result.success = true;
        return result;
    }
};

// ============================================================================
// session_status Tool (aligned with OpenClaw session-status-tool.ts)
// ============================================================================

class SessionStatusTool : public Tool {
public:
    explicit SessionStatusTool(std::shared_ptr<gateway::GatewayClient> gw,
                                SessionsToolContext ctx)
        : gw_(std::move(gw)), ctx_(std::move(ctx)) {}

    std::string name() const override { return "session_status"; }

    std::string description() const override {
        return "Show a /status-equivalent session status card (usage + time + cost when available). "
               "Use for model-use questions (\xf0\x9f\x93\x8a session_status). "
               "Optional: set per-session model override (model=default resets overrides).";
    }

    Schema parameters() const override {
        return Schema{
            "object", "session_status parameters",
            {
                {"sessionKey", json{{"type","string"}}},
                {"model",      json{{"type","string"}}},
            },
            {}
        };
    }

    ToolResult execute(const core::JsonValue& params_val) override {
        const json params = params_val.to_json();

        std::string main_key, alias, requester_internal_key, effective_requester_key;
        bool restrict_to_spawned;
        resolve_sandboxed_session_tool_context(ctx_,
            main_key, alias, requester_internal_key,
            effective_requester_key, restrict_to_spawned);

        A2APolicy a2a_policy = build_a2a_policy(ctx_);
        std::string requester_agent_id = resolve_agent_id_from_session_key(
            ctx_.agent_session_key.empty() ? effective_requester_key : ctx_.agent_session_key);

        std::string requested_key_param = read_string_param(params, "sessionKey");
        std::string requested_key_raw = requested_key_param.empty()
            ? ctx_.agent_session_key
            : requested_key_param;
        requested_key_raw = trim(requested_key_raw);

        if (requested_key_raw.empty()) {
            ToolResult result;
            result.success = false;
            result.error = "sessionKey required";
            return result;
        }

        // Cross-agent access check
        if (requested_key_raw.substr(0, 6) == "agent:") {
            std::string target_agent_id = resolve_agent_id_from_session_key(requested_key_raw);
            if (target_agent_id != requester_agent_id) {
                if (!a2a_policy.enabled) {
                    ToolResult result;
                    result.success = false;
                    result.error = "Agent-to-agent status is disabled. "
                                   "Set tools.agentToAgent.enabled=true to allow cross-agent access.";
                    return result;
                }
                if (!a2a_policy.is_allowed(requester_agent_id, target_agent_id)) {
                    ToolResult result;
                    result.success = false;
                    result.error = "Agent-to-agent session status denied by tools.agentToAgent.allow.";
                    return result;
                }
            }
        }

        // Fetch session status from gateway
        if (!gw_) {
            ToolResult result;
            result.success = false;
            result.error = "Gateway not available";
            return result;
        }

        std::string internal_key = resolve_internal_session_key(
            requested_key_raw, alias, main_key);
        std::string display_key = resolve_display_session_key(
            internal_key, alias, main_key);

        // Handle model override if provided
        std::string model_raw = read_string_param(params, "model");
        bool changed_model = false;
        if (!model_raw.empty() && gw_) {
            json model_val;
            if (to_lower(trim(model_raw)) == "default") {
                model_val = nullptr; // reset
            } else {
                model_val = model_raw;
            }
            try {
                gw_->request("sessions.patch", json{
                    {"key",   internal_key},
                    {"model", model_val}
                });
                changed_model = true;
            } catch (...) {}
        }

        // Fetch session info from gateway
        json session_info;
        try {
            auto result = gw_->request("sessions.get", json{{"key", internal_key}});
            if (result.is_ok()) {
                session_info = result.get();
            }
        } catch (...) {}

        // Build status text (aligned with OpenClaw buildStatusMessage)
        std::string model = session_info.value("model", ctx_.default_model);
        std::string kind = classify_session_kind(internal_key, "", alias, main_key);
        std::string channel = session_info.value("channel",
            session_info.value("lastChannel", "unknown"));

        std::string status_text = "**Session Status**\n";
        status_text += "- Key: `" + display_key + "`\n";
        status_text += "- Kind: " + kind + "\n";
        status_text += "- Model: " + model + "\n";
        status_text += "- Channel: " + channel + "\n";

        if (session_info.contains("updatedAt") && session_info["updatedAt"].is_number()) {
            status_text += "- Last active: " +
                std::to_string((int64_t)session_info["updatedAt"].get<double>()) + "\n";
        }
        if (session_info.contains("contextTokens") && session_info["contextTokens"].is_number()) {
            status_text += "- Context tokens: " +
                std::to_string((int)session_info["contextTokens"].get<double>()) + "\n";
        }
        if (changed_model) {
            status_text += "- Model updated to: " + model_raw + "\n";
        }

        ToolResult result;
        result.content.push_back({ToolResultContent::Type::Text, status_text});
        result.details = json{
            {"ok",          true},
            {"sessionKey",  internal_key},
            {"changedModel", changed_model},
            {"statusText",  status_text}
        };
        result.success = true;
        return result;
    }

private:
    std::shared_ptr<gateway::GatewayClient> gw_;
    SessionsToolContext ctx_;
};

// ============================================================================
// sessions_yield Tool (aligned with OpenClaw sessions-yield-tool.ts)
// ============================================================================

class SessionsYieldTool : public Tool {
public:
    explicit SessionsYieldTool(SessionsToolContext ctx,
                                std::function<void(const std::string&)> on_yield)
        : ctx_(std::move(ctx)), on_yield_(std::move(on_yield)) {}

    std::string name() const override { return "sessions_yield"; }

    std::string description() const override {
        return "End your current turn. Use after spawning subagents to receive "
               "their results as the next message.";
    }

    Schema parameters() const override {
        return Schema{
            "object", "sessions_yield parameters",
            {
                {"message", json{{"type","string"}}},
            },
            {}
        };
    }

    ToolResult execute(const core::JsonValue& params_val) override {
        const json params = params_val.to_json();
        std::string message = read_string_param(params, "message");
        if (message.empty()) message = "Turn yielded.";

        if (ctx_.session_id.empty()) {
            return make_json_result(json{
                {"status", "error"},
                {"error",  "No session context"}
            });
        }
        if (!on_yield_) {
            return make_json_result(json{
                {"status", "error"},
                {"error",  "Yield not supported in this context"}
            });
        }

        on_yield_(message);
        return make_json_result(json{
            {"status",  "yielded"},
            {"message", message}
        });
    }

private:
    SessionsToolContext ctx_;
    std::function<void(const std::string&)> on_yield_;

    static ToolResult make_json_result(const json& data) {
        ToolResult result;
        result.content.push_back({ToolResultContent::Type::Json, data.dump()});
        result.success = true;
        return result;
    }
};

// ============================================================================
// Factory functions
// ============================================================================

std::vector<std::shared_ptr<Tool>> create_sessions_tools(
    std::shared_ptr<gateway::GatewayClient> gw,
    const SessionsToolContext& ctx,
    std::function<void(const std::string&)> on_yield
) {
    std::vector<std::shared_ptr<Tool>> tools;
    tools.push_back(std::make_shared<SessionsListTool>(gw, ctx));
    tools.push_back(std::make_shared<SessionsSendTool>(gw, ctx));
    tools.push_back(std::make_shared<SessionsSpawnTool>(gw, ctx));
    tools.push_back(std::make_shared<SessionsHistoryTool>(gw, ctx));
    tools.push_back(std::make_shared<SessionStatusTool>(gw, ctx));
    if (on_yield) {
        tools.push_back(std::make_shared<SessionsYieldTool>(ctx, std::move(on_yield)));
    }
    return tools;
}

} // namespace tools
} // namespace hunterclaw
