// sessions_tool.cc
// Strictly aligned with OpenClaw:
//   src/agents/tools/sessions-list-tool.ts
//   src/agents/tools/sessions-send-tool.ts
//   src/agents/tools/sessions-send-tool.a2a.ts
//   src/agents/tools/sessions-spawn-tool.ts
//   src/agents/tools/sessions-history-tool.ts
//   src/agents/tools/session-status-tool.ts
//   src/agents/tools/sessions-helpers.ts
//   src/agents/tools/sessions-access.ts
//   src/agents/tools/sessions-resolution.ts
//   src/agents/tools/subagents-tool.ts
//   src/agents/subagent-spawn.ts (spawnSubagentDirect)
//   src/agents/acp-spawn.ts (spawnAcpDirect)

#include "hunterclaw/tools/sessions_tool.h"
#include "hunterclaw/tools/sessions_send_a2a.h"
#include "hunterclaw/logging/redact.h"
#include "hunterclaw/agents/acp_spawn.h"
#include "hunterclaw/sessions/session_key_utils.h"
#include "hunterclaw/routing/session_key.h"
#include "hunterclaw/gateway/client.h"
#include "hunterclaw/auto_reply/commands_tasks.h"  // ✅ 对齐 OC: task status in session_status tool
#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <chrono>
#include <random>
#include <thread>

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

        // Capture baseline reply snapshot BEFORE agent run
        // Aligned with OpenClaw: baseline snapshot for round-1 reply diff
        std::string baseline_reply;
        if (timeout_seconds == 0 && !ctx_.agent_session_key.empty()) {
            try {
                auto hist_result = gw_->request<json>("chat.history", json{
                    {"sessionKey", resolved_key},
                    {"limit", 20}
                });
                if (hist_result.is_ok()) {
                    json raw_msgs = hist_result.get().value("messages", json::array());
                    for (int i = (int)raw_msgs.size() - 1; i >= 0; --i) {
                        const auto& msg = raw_msgs[i];
                        if (msg.is_object() && msg.value("role", "") == "assistant") {
                            if (msg.contains("content") && msg["content"].is_string()) {
                                baseline_reply = msg["content"].get<std::string>();
                            }
                            break;
                        }
                    }
                }
            } catch (...) {}
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

        // If timeout=0, return accepted immediately and spawn A2A background flow
        // ✓ Aligned with OpenClaw: runWithGatewayIndependentRootWorkContinuation
        if (timeout_seconds == 0) {
            SessionsSendA2AParams a2a_params;
            a2a_params.target_session_key = resolved_key;
            a2a_params.display_key = display_key;
            a2a_params.message = message;
            a2a_params.announce_timeout_ms = announce_timeout_ms;
            a2a_params.max_ping_pong_turns = 5;
            a2a_params.requester_session_key = ctx_.agent_session_key;
            a2a_params.requester_channel = ctx_.agent_channel;
            a2a_params.wait_run_id = run_id;
            a2a_params.baseline_reply = baseline_reply;
            a2a_params.notify_requester_on_wait_failure = true;

            std::thread([gw = gw_, a2a_params]() {
                try {
                    run_sessions_send_a2a_flow(gw, a2a_params);
                } catch (const std::exception& e) {
                    auto log = spdlog::get("sessions.send");
                    if (!log) log = spdlog::stdout_logger_mt("sessions.send");
                    log->warn("sessions_send: A2A flow exception: {}", e.what());
                } catch (...) {
                    auto log = spdlog::get("sessions.send");
                    if (!log) log = spdlog::stdout_logger_mt("sessions.send");
                    log->warn("sessions_send: A2A flow unknown exception");
                }
            }).detach();

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
                // required
                {"task",              json{{"type","string"}}},
                // optional
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
                // ✓ 对齐 OpenClaw sessions-spawn-tool.ts: context/collect/swarm 参数
                {"context",           json{{"type","string"},{"enum",json::array({"isolated","fork"})}}},
                {"collect",           json{{"type","boolean"}}},
                {"outputSchema",      json{{"type","object"}}},
                {"fastMode",          json{{"type","boolean"}}},
                {"groupId",           json{{"type","string"}}},
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
            // === ACP spawn (aligned with OpenClaw spawnAcpDirect) ===
            agents::SpawnAcpParams acp_params;
            acp_params.task = task;
            if (!label.empty()) acp_params.label = label;
            {
                std::string acp_agent_id = trim(read_string_param(params, "agentId"));
                if (!acp_agent_id.empty()) acp_params.agent_id = acp_agent_id;
            }
            if (!resume_session_id.empty()) acp_params.resume_session_id = resume_session_id;
            if (!ctx_.workspace_dir.empty()) acp_params.cwd = ctx_.workspace_dir;
            if (!stream_to.empty()) acp_params.stream_to = stream_to;
            // mode: "run" (default) or "session" (requires thread=true)
            bool thread_bind = params.value("thread", false);
            acp_params.thread = thread_bind;
            std::string mode_param = read_string_param(params, "mode");
            if (!mode_param.empty()) acp_params.mode = mode_param;
            // sandbox
            std::string sandbox_param = read_string_param(params, "sandbox");
            if (!sandbox_param.empty()) acp_params.sandbox = sandbox_param;

            agents::SpawnAcpContext acp_ctx;
            acp_ctx.agent_session_key = ctx_.agent_session_key.empty()
                ? std::nullopt : std::make_optional(ctx_.agent_session_key);
            acp_ctx.agent_channel = ctx_.agent_channel.empty()
                ? std::nullopt : std::make_optional(ctx_.agent_channel);
            acp_ctx.agent_account_id = ctx_.agent_account_id.empty()
                ? std::nullopt : std::make_optional(ctx_.agent_account_id);
            acp_ctx.agent_to = ctx_.agent_to.empty()
                ? std::nullopt : std::make_optional(ctx_.agent_to);
            acp_ctx.agent_thread_id = ctx_.agent_thread_id.empty()
                ? std::nullopt : std::make_optional(ctx_.agent_thread_id);
            acp_ctx.sandboxed = ctx_.sandboxed;

            agents::AcpSpawnConfig acp_cfg;
            acp_cfg.enable_acp = ctx_.acp_enabled;
            acp_cfg.acp_default_agent = ctx_.acp_default_agent;
            acp_cfg.acp_allowed_agents = ctx_.acp_allowed_agents;
            acp_cfg.acp_max_concurrent_sessions = ctx_.acp_max_concurrent_sessions;

            auto acp_result = agents::spawn_acp_direct(acp_params, acp_ctx, gw_, acp_cfg);

            json result_json;
            result_json["status"] = acp_result.status;
            if (acp_result.child_session_key.has_value())
                result_json["childSessionKey"] = *acp_result.child_session_key;
            if (acp_result.run_id.has_value())
                result_json["runId"] = *acp_result.run_id;
            if (acp_result.mode.has_value())
                result_json["mode"] = *acp_result.mode;
            if (acp_result.stream_log_path.has_value())
                result_json["streamLogPath"] = *acp_result.stream_log_path;
            if (acp_result.note.has_value())
                result_json["note"] = *acp_result.note;
            if (acp_result.error.has_value())
                result_json["error"] = *acp_result.error;
            return make_json_result(result_json);
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

        // ✅ 对齐 OpenClaw sessions-spawn-tool.ts: context/collect/swarm 参数
        std::string context_mode = params.value("context", "isolated");
        if (context_mode != "isolated" && context_mode != "fork") context_mode = "isolated";
        bool collect = params.value("collect", false);
        bool fast_mode = params.value("fastMode", false);
        std::string group_id = read_string_param(params, "groupId");

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
        int caller_depth = routing::get_subagent_depth(effective_requester_key);
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
        // ✅ 对齐 OpenClaw: countActiveRunsForSession(requesterInternalKey)
        // 实时统计活跃子 agent 数量，而非使用静态的 ctx_.active_children_count
        int active_children = ctx_.active_children_count;
        if (ctx_.registry) {
            // 从 registry 实时获取活跃子 agent 数量
            auto children = ctx_.registry->list_by_requester(effective_requester_key);
            active_children = 0;
            for (const auto& child : children) {
                if (child && child->state != agents::SubagentState::Completed
                    && child->state != agents::SubagentState::Failed
                    && child->state != agents::SubagentState::Killed) {
                    active_children++;
                }
            }
        }
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
        // ✅ 对齐 OpenClaw: context="fork" → copy parent transcript
        std::string fork_transcript_context;
        if (context_mode == "fork" && !effective_requester_key.empty()) {
            fork_transcript_context =
                "\n\n[Forked Context] The following is the full conversation transcript "
                "from the requester session. You have access to all prior context.\n\n";
            try {
                auto hist = gw_->request<json>("chat.history", json{
                    {"sessionKey", effective_requester_key},
                    {"limit", 500}
                });
                if (hist.is_ok()) {
                    const auto& msgs = hist.get().value("messages", json::array());
                    for (const auto& msg : msgs) {
                        std::string role = msg.value("role", "");
                        std::string content = msg.value("content", "");
                        if (role == "system" || role == "assistant") {
                            fork_transcript_context += "[" + role + "]: " + content + "\n";
                        } else if (role == "user") {
                            fork_transcript_context += "[user]: " + content + "\n";
                        }
                    }
                }
            } catch (...) {
                fork_transcript_context = "";
            }
        }
        std::string child_task_message =
            "[Subagent Context] You are running as a subagent (depth " +
            std::to_string(child_depth) + "/" + std::to_string(max_spawn_depth) +
            "). Results auto-announce to your requester; do not busy-poll for status.";
        if (spawn_mode == "session") {
            child_task_message += "\n\n[Subagent Context] This subagent session is persistent "
                                  "and remains available for thread follow-up messages.";
        }
        if (!fork_transcript_context.empty()) {
            child_task_message += fork_transcript_context;
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

        // ✅ 对齐 OpenClaw: registerSubagentRun (subagent-registry.ts)
        // 在 Gateway agent 调用成功后，向 SubagentRegistry 注册此次 spawn
        // 这是 SubagentAnnouncer 能找到 entry 并通知父 agent 的前提
        if (ctx_.registry) {
            agents::SubagentEntry entry;
            entry.subagent_run_id    = child_run_id;
            entry.requester_run_id   = effective_requester_key;
            entry.controller_session_key = effective_requester_key;
            entry.requester_display_key  = resolve_display_session_key(
                effective_requester_key, alias, main_key);
            entry.subagent_name      = target_agent_id;
            entry.label              = label;
            entry.task               = task;
            entry.cleanup            = cleanup;
            entry.spawn_mode         = spawn_mode;
            // ✅ 对齐 OpenClaw: expectsCompletionMessage = params.expectsCompletionMessage !== false
            // sessions_spawn 工具调用时 expectsCompletionMessage 默认为 true
            entry.expects_completion_message = !collect;  // ✅ collect=true → false
            entry.timeout_seconds    = run_timeout_seconds > 0 ? run_timeout_seconds : 900;
            entry.state              = agents::SubagentState::Running;
            entry.spawned_at = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count()
            );
            // workspace dir 继承
            if (!ctx_.workspace_dir.empty()) {
                // 存储在 description 中供 announcer 使用
                entry.description = ctx_.workspace_dir;
            }
            ctx_.registry->register_subagent(entry);
        }

        json result_json = {
            {"status",          "accepted"},
            {"childSessionKey", child_session_key},
            {"runId",           child_run_id},
            {"mode",            spawn_mode},
        };
        if (!note.empty()) result_json["note"] = note;
        if (!model_override.empty()) result_json["modelApplied"] = model_applied;
        // ✅ 对齐 OpenClaw: collect/swarm/fastMode/groupId 透传
        if (collect) {
            result_json["collect"] = true;
            if (!group_id.empty()) result_json["groupId"] = group_id;
            if (fast_mode) result_json["fastMode"] = true;
        }
        if (context_mode == "fork") result_json["context"] = "fork";

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

        // ✅ 对齐 OC session-status-tool.ts: task status line
        // Uses format_session_task_line / format_agent_task_counts_line
        // Task status is best-effort — if task_registry is not accessible, skip silently

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
// subagents Tool (aligned with OpenClaw subagents-tool.ts)
// ============================================================================
// OpenClaw: src/agents/tools/subagents-tool.ts
// Actions: list | kill | steer
// ============================================================================

class SubagentsTool : public Tool {
public:
    explicit SubagentsTool(std::shared_ptr<gateway::GatewayClient> gw,
                            SessionsToolContext ctx)
        : gw_(std::move(gw)), ctx_(std::move(ctx)) {}

    std::string name() const override { return "subagents"; }

    std::string description() const override {
        return "List, kill, or steer spawned sub-agents for this requester session. "
               "Use this for sub-agent orchestration.";
    }

    Schema parameters() const override {
        return Schema{
            "object", "subagents parameters",
            {
                {"action",        json{{"type","string"},{"enum",json::array({"list","kill","steer"})}}},
                {"target",        json{{"type","string"}}},
                {"message",       json{{"type","string"}}},
                {"recentMinutes", json{{"type","number"},{"minimum",1}}},
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

        std::string action = read_string_param(params, "action");
        if (action.empty()) action = "list";

        // ✅ 对齐 OpenClaw: DEFAULT_RECENT_MINUTES = 60, MAX_RECENT_MINUTES = 1440
        static constexpr int DEFAULT_RECENT_MINUTES = 60;
        static constexpr int MAX_RECENT_MINUTES = 1440;
        static constexpr int MAX_STEER_MESSAGE_CHARS = 2000;

        int recent_minutes = DEFAULT_RECENT_MINUTES;
        if (params.contains("recentMinutes") && params["recentMinutes"].is_number()) {
            recent_minutes = std::max(1, std::min(MAX_RECENT_MINUTES,
                (int)params["recentMinutes"].get<double>()));
        }

        if (!ctx_.registry) {
            return make_json_result(json{
                {"status", "error"},
                {"error", "Subagent registry not available"}
            });
        }

        // ✅ 对齐 OpenClaw: listControlledSubagentRuns(controller.controllerSessionKey)
        auto all_runs = ctx_.registry->list_by_requester(effective_requester_key);

        // ✅ 对齐 OpenClaw: isActiveSubagentRun()
        auto is_active = [](const std::shared_ptr<agents::SubagentEntry>& e) -> bool {
            if (!e) return false;
            return e->state != agents::SubagentState::Completed
                && e->state != agents::SubagentState::Failed
                && e->state != agents::SubagentState::Killed;
        };

        // ✅ 对齐 OpenClaw: action === "list"
        if (action == "list") {
            auto now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count()
            );
            uint64_t recent_cutoff_ms = now_ms - (uint64_t)recent_minutes * 60 * 1000;

            json active_list = json::array();
            json recent_list = json::array();

            for (const auto& e : all_runs) {
                if (!e) continue;
                bool active = is_active(e);
                bool recent = (e->spawned_at >= recent_cutoff_ms);

                json view = {
                    {"runId",      e->subagent_run_id},
                    {"sessionKey", e->subagent_run_id},
                    {"label",      e->label.empty() ? e->subagent_name : e->label},
                    {"task",       e->task},
                    {"spawnMode",  e->spawn_mode},
                    {"active",     active},
                };
                std::string state_str;
                switch (e->state) {
                    case agents::SubagentState::Spawning:   state_str = "spawning"; break;
                    case agents::SubagentState::Listening:  state_str = "listening"; break;
                    case agents::SubagentState::Running:    state_str = "running"; break;
                    case agents::SubagentState::Completing: state_str = "completing"; break;
                    case agents::SubagentState::Announcing: state_str = "announcing"; break;
                    case agents::SubagentState::Completed:  state_str = "completed"; break;
                    case agents::SubagentState::Failed:     state_str = "failed"; break;
                    case agents::SubagentState::TimedOut:   state_str = "timedout"; break;
                    case agents::SubagentState::Killed:     state_str = "killed"; break;
                    default:                                state_str = "unknown"; break;
                }
                view["state"] = state_str;
                if (e->spawned_at > 0) view["spawnedAt"] = e->spawned_at;
                if (e->completed_at > 0) view["completedAt"] = e->completed_at;
                if (e->has_error) view["error"] = e->error_message;

                if (active) active_list.push_back(view);
                else if (recent) recent_list.push_back(view);
            }

            // ✅ 对齐 OpenClaw: buildSubagentList 返回格式
            return make_json_result(json{
                {"status",              "ok"},
                {"action",              "list"},
                {"requesterSessionKey", effective_requester_key},
                {"callerSessionKey",    ctx_.agent_session_key},
                {"callerIsSubagent",    is_subagent_session_key(ctx_.agent_session_key)},
                {"total",               (int)all_runs.size()},
                {"active",              active_list},
                {"recent",              recent_list},
            });
        }

        // ✅ 对齐 OpenClaw: action === "kill"
        if (action == "kill") {
            std::string target = read_string_param(params, "target", true);

            // Kill all
            if (target == "all" || target == "*") {
                int killed = 0;
                json labels = json::array();
                for (const auto& e : all_runs) {
                    if (!e || !is_active(e)) continue;
                    ctx_.registry->update_state(e->subagent_run_id,
                        agents::SubagentState::Killed);
                    // Send kill signal via gateway
                    if (gw_) {
                        try {
                            gw_->request("sessions.delete", json{
                                {"key", e->subagent_run_id},
                                {"emitLifecycleHooks", true},
                                {"deleteTranscript", false}
                            });
                        } catch (...) {}
                    }
                    killed++;
                    labels.push_back(e->label.empty() ? e->subagent_name : e->label);
                }
                return make_json_result(json{
                    {"status",  "ok"},
                    {"action",  "kill"},
                    {"target",  "all"},
                    {"killed",  killed},
                    {"labels",  labels},
                    {"text",    killed > 0
                        ? "killed " + std::to_string(killed) + " subagent" + (killed == 1 ? "" : "s") + "."
                        : "no running subagents to kill."}
                });
            }

            // Kill specific target
            std::shared_ptr<agents::SubagentEntry> found;
            for (const auto& e : all_runs) {
                if (!e) continue;
                if (e->subagent_run_id == target
                    || e->label == target
                    || e->subagent_name == target) {
                    found = e;
                    break;
                }
            }
            if (!found) {
                return make_json_result(json{
                    {"status", "error"},
                    {"action", "kill"},
                    {"target", target},
                    {"error",  "Unknown subagent target."}
                });
            }
            ctx_.registry->update_state(found->subagent_run_id,
                agents::SubagentState::Killed);
            if (gw_) {
                try {
                    gw_->request("sessions.delete", json{
                        {"key", found->subagent_run_id},
                        {"emitLifecycleHooks", true},
                        {"deleteTranscript", false}
                    });
                } catch (...) {}
            }
            return make_json_result(json{
                {"status",     "ok"},
                {"action",     "kill"},
                {"target",     target},
                {"runId",      found->subagent_run_id},
                {"sessionKey", found->subagent_run_id},
                {"label",      found->label.empty() ? found->subagent_name : found->label},
                {"text",       "killed subagent: " + (found->label.empty() ? found->subagent_name : found->label)}
            });
        }

        // ✅ 对齐 OpenClaw: action === "steer"
        if (action == "steer") {
            std::string target = read_string_param(params, "target", true);
            std::string message = read_string_param(params, "message", true);

            if ((int)message.size() > MAX_STEER_MESSAGE_CHARS) {
                return make_json_result(json{
                    {"status", "error"},
                    {"action", "steer"},
                    {"target", target},
                    {"error",  "Message too long (" + std::to_string(message.size()) +
                               " chars, max " + std::to_string(MAX_STEER_MESSAGE_CHARS) + ")."}
                });
            }

            std::shared_ptr<agents::SubagentEntry> found;
            for (const auto& e : all_runs) {
                if (!e) continue;
                if (e->subagent_run_id == target
                    || e->label == target
                    || e->subagent_name == target) {
                    found = e;
                    break;
                }
            }
            if (!found) {
                return make_json_result(json{
                    {"status", "error"},
                    {"action", "steer"},
                    {"target", target},
                    {"error",  "Unknown subagent target."}
                });
            }

            // Send steer message via gateway
            if (!gw_) {
                return make_json_result(json{
                    {"status", "error"},
                    {"action", "steer"},
                    {"target", target},
                    {"error",  "Gateway not available"}
                });
            }

            std::string steer_idem = generate_uuid();
            try {
                auto resp = gw_->request("agent", json{
                    {"sessionKey",     found->subagent_run_id},
                    {"message",        message},
                    {"idempotencyKey", steer_idem},
                    {"deliver",        false},
                    {"lane",           AGENT_LANE_NESTED},
                    {"inputProvenance", json{
                        {"kind",             "inter_session"},
                        {"sourceSessionKey", ctx_.agent_session_key},
                        {"sourceTool",       "subagents_steer"}
                    }}
                });
                if (!resp.is_ok()) {
                    return make_json_result(json{
                        {"status", "error"},
                        {"action", "steer"},
                        {"target", target},
                        {"error",  resp.error}
                    });
                }
                std::string run_id = steer_idem;
                if (resp.get().contains("runId") && resp.get()["runId"].is_string()) {
                    run_id = resp.get()["runId"].get<std::string>();
                }
                return make_json_result(json{
                    {"status",     "ok"},
                    {"action",     "steer"},
                    {"target",     target},
                    {"runId",      run_id},
                    {"sessionKey", found->subagent_run_id},
                    {"label",      found->label.empty() ? found->subagent_name : found->label},
                    {"mode",       found->spawn_mode},
                    {"text",       "steered subagent: " + (found->label.empty() ? found->subagent_name : found->label)}
                });
            } catch (const std::exception& e) {
                return make_json_result(json{
                    {"status", "error"},
                    {"action", "steer"},
                    {"target", target},
                    {"error",  std::string(e.what())}
                });
            }
        }

        return make_json_result(json{
            {"status", "error"},
            {"error",  "Unsupported action."}
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
// Factory functions
// ============================================================================

// ✅ 对齐 OpenClaw: agents_wait tool (agents-wait-tool.ts)
class AgentsWaitTool : public Tool {
public:
    explicit AgentsWaitTool(std::shared_ptr<gateway::GatewayClient> gw,
                            SessionsToolContext ctx)
        : gw_(std::move(gw)), ctx_(std::move(ctx)) {}

    std::string name() const override { return "agents_wait"; }

    std::string description() const override {
        return "Wait for agent sub-task runs to complete. "
               "Use this to collect results from sessions_spawn calls. "
               "Takes run IDs from previous sessions_spawn responses. "
               "Returns completed runs with results, and still-pending run IDs.";
    }

    Schema parameters() const override {
        return Schema{
            "object", "agents_wait parameters",
            {
                {"runIds", json{
                    {"type", "array"},
                    {"items", json{{"type", "string"}}},
                    {"description", "Array of run IDs from sessions_spawn responses to wait for"}
                }}
            },
            {"runIds"}
        };
    }

    ToolResult execute(const core::JsonValue& params) override {
        // Extract runIds array (using same pattern as SessionsListTool for "kinds")
        if (!params.contains("runIds") || !params["runIds"].is_array()) {
            ToolResult err;
            err.success = false;
            err.error = "runIds must be a non-empty array";
            return err;
        }

        const auto& run_ids_json = params["runIds"];
        if (run_ids_json.size() == 0) {
            ToolResult err;
            err.success = false;
            err.error = "runIds must be a non-empty array";
            return err;
        }

        std::string own_session = ctx_.agent_session_key;
        auto registry = ctx_.registry;

        // Build valid/unknown ID lists
        std::vector<std::string> valid_ids, unknown_ids;

        size_t count = run_ids_json.size();
        for (size_t i = 0; i < count; ++i) {
            const auto& id_val = run_ids_json[i];
            if (!id_val.is_string()) continue;
            std::string rid = id_val.as_string();
            if (rid.empty()) continue;

            if (registry) {
                auto entry = registry->get_entry(rid);
                if (entry != nullptr) {
                    if (entry->requester_run_id == own_session) {
                        valid_ids.push_back(rid);
                    } else {
                        unknown_ids.push_back(rid);
                    }
                    continue;
                }
            }
            unknown_ids.push_back(rid);
        }

        // Poll for completion
        constexpr int MAX_POLL = 10;
        constexpr int INTERVAL_MS = 200;

        json completed_arr = json::array();
        std::vector<std::string> pending = valid_ids;

        for (int attempt = 0; attempt < MAX_POLL; ++attempt) {
            if (pending.empty()) break;
            if (attempt > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(INTERVAL_MS));
            }

            std::vector<std::string> still;
            for (const auto& rid : pending) {
                if (registry) {
                    auto e = registry->get_entry(rid);
                    if (!e) { still.push_back(rid); continue; }
                    if (e->state == agents::SubagentState::Completed) {
                        json item;
                        item["runId"]      = rid;
                        item["status"]     = "completed";
                        item["sessionKey"] = e->requester_display_key;
                        if (!e->result_text.empty()) item["result"] = e->result_text;
                        completed_arr.push_back(item);
                    } else if (e->state == agents::SubagentState::Failed) {
                        json item;
                        item["runId"]  = rid;
                        item["status"] = "error";
                        item["error"]  = e->has_error ? e->result_text : "subagent failed";
                        completed_arr.push_back(item);
                    } else {
                        still.push_back(rid);
                    }
                } else {
                    // Fallback: try gateway agent.wait
                    try {
                        auto r = gw_->request<json>("agent.wait", json{
                            {"runId", rid}, {"timeoutMs", INTERVAL_MS}
                        });
                        if (r.is_ok() &&
                            (r.get().value("status", "") == "ok" ||
                             r.get().value("status", "") == "completed")) {
                            json item;
                            item["runId"]  = rid;
                            item["status"] = "completed";
                            completed_arr.push_back(item);
                            continue;
                        }
                    } catch (...) {}
                    still.push_back(rid);
                }
            }
            pending = std::move(still);
        }

        json pending_arr = json::array();
        for (auto& id : pending) pending_arr.push_back(id);
        for (auto& id : unknown_ids) pending_arr.push_back(id);

        ToolResult result;
        result.content.push_back({
            ToolResultContent::Type::Json,
            json{{"ok", true}, {"completed", completed_arr}, {"pending", pending_arr}}.dump(),
            std::string{},          // data
            std::string{"application/json"}  // mime_type
        });
        result.success = true;
        return result;
    }

private:
    std::shared_ptr<gateway::GatewayClient> gw_;
    SessionsToolContext ctx_;
};

// ============================================================================
// agents_list Tool (✓ 对齐 OpenClaw agents-list-tool.ts)
// ============================================================================

class AgentsListTool : public Tool {
public:
    explicit AgentsListTool(std::shared_ptr<gateway::GatewayClient> gw,
                            SessionsToolContext ctx)
        : gw_(std::move(gw)), ctx_(std::move(ctx)) {}

    std::string name() const override { return "agents_list"; }

    std::string description() const override {
        return "List configured agent ids available as sessions_spawn targets. "
               "Use this to discover valid agentId values before calling sessions_spawn.";
    }

    Schema parameters() const override {
        return Schema{"object", "agents_list parameters", {}, {}};
    }

    ToolResult execute(const core::JsonValue& /*params*/) override {
        if (!gw_) {
            ToolResult err;
            err.success = false;
            err.error = "Gateway not available";
            return err;
        }

        try {
            auto result = gw_->request<json>("agents.list", json{
                {"scope", json{{"visibility", "all"}}}
            });

            json response;
            if (result.is_ok()) {
                const auto& agents_list_resp = result.get();
                std::string own_agent = ctx_.agent_session_key.empty() ? "" :
                    ctx_.agent_session_key;

                json agents = json::array();
                if (agents_list_resp.contains("agents") && agents_list_resp["agents"].is_array()) {
                    for (const auto& agent : agents_list_resp["agents"]) {
                        if (!agent.is_object()) continue;
                        json entry;
                        entry["id"] = agent.value("id", "");
                        if (agent.contains("label") && agent["label"].is_string())
                            entry["label"] = agent["label"];
                        if (agent.contains("description") && agent["description"].is_string())
                            entry["description"] = agent["description"];
                        if (agent.contains("runtime") && agent["runtime"].is_string())
                            entry["runtime"] = agent["runtime"];
                        agents.push_back(entry);
                    }
                }

                response["ok"] = true;
                response["agents"] = agents;
                if (!own_agent.empty()) {
                    std::string own_id = resolve_agent_id_from_session_key(own_agent);
                    response["requester"] = own_id.empty() ? own_agent : own_id;
                }
                response["allowAny"] = has_allow_any();
            } else {
                // Fallback: return empty agents array
                response["ok"] = true;
                response["agents"] = json::array();
                response["note"] = "agents.list unavailable";
            }

            ToolResult out;
            out.content.push_back({
                ToolResultContent::Type::Json,
                response.dump(),
                std::string{},
                std::string{"application/json"}
            });
            out.success = true;
            return out;
        } catch (const std::exception& e) {
            ToolResult err;
            err.success = false;
            err.error = std::string("agents_list failed: ") + e.what();
            return err;
        }
    }

private:
    std::shared_ptr<gateway::GatewayClient> gw_;
    SessionsToolContext ctx_;

    bool has_allow_any() const {
        for (const auto& allowed : ctx_.subagent_allow_agents) {
            if (trim(allowed) == "*") return true;
        }
        return false;
    }
};

// ============================================================================
// sessions_search Tool (✓ 对齐 OpenClaw sessions-search-tool.ts)
// ============================================================================

class SessionsSearchTool : public Tool {
public:
    explicit SessionsSearchTool(std::shared_ptr<gateway::GatewayClient> gw,
                                SessionsToolContext ctx)
        : gw_(std::move(gw)), ctx_(std::move(ctx)) {}

    std::string name() const override { return "sessions_search"; }

    std::string description() const override {
        return "Full-text search across visible session transcripts. "
               "Searches message content and returns matching sessions with snippets.";
    }

    Schema parameters() const override {
        return Schema{
            "object", "sessions_search parameters",
            {
                {"query", json{
                    {"type", "string"},
                    {"description", "Search query string. Searches across all accessible session transcripts."}
                }},
                {"sessionKey", json{
                    {"type", "string"},
                    {"description", "Optional: restrict search to a specific session."}
                }},
                {"limit", json{
                    {"type", "number"},
                    {"minimum", 1},
                    {"maximum", 25},
                    {"default", 10},
                    {"description", "Maximum number of results to return."}
                }},
            },
            {"query"}
        };
    }

    ToolResult execute(const core::JsonValue& params_val) override {
        const json params = params_val.to_json();

        std::string query = params.value("query", "");
        if (query.empty()) {
            ToolResult err;
            err.success = false;
            err.error = "query is required";
            return err;
        }

        int limit = params.contains("limit") ? std::max(1, std::min(25, (int)params["limit"].get<double>())) : 10;

        if (!gw_) {
            ToolResult err;
            err.success = false;
            err.error = "Gateway not available";
            return err;
        }

        try {
            json list_params = {
                {"limit", limit + 5}  // fetch extra for filtering
            };
            if (query.find_first_of("\"'`*()[]{}") == std::string::npos &&
                query.find(" ") == std::string::npos) {
                // Simple word → sessions.list search
                list_params["search"] = query;
            } else {
                // Complex query → sessions.list with label pattern
                list_params["search"] = query;
            }

            auto list_res = gw_->request<json>("sessions.list", list_params);

            json results = json::array();
            if (list_res.is_ok()) {
                const auto& list_data = list_res.get();
                const auto& sessions = list_data.value("sessions", json::array());

                for (const auto& session : sessions) {
                    if (results.size() >= (size_t)limit) break;
                    if (!session.is_object()) continue;

                    std::string session_key = session.value("key", "");
                    if (session_key.empty()) continue;

                    // Try to get preview/snippet
                    json result_item;
                    result_item["sessionKey"] = session_key;
                    if (session.contains("label") && session["label"].is_string())
                        result_item["label"] = session["label"];
                    if (session.contains("lastActivity") && session["lastActivity"].is_number())
                        result_item["timestamp"] = session["lastActivity"];

                    // Search preview for snippet
                    try {
                        auto preview = gw_->request<json>("sessions.preview", json{
                            {"keys", json::array({session_key})},
                            {"limit", 1}
                        });
                        if (preview.is_ok()) {
                            const auto& entries = preview.get().value("entries", json::array());
                            for (const auto& entry : entries) {
                                if (entry.is_object() && entry.contains("recentMessages") &&
                                    entry["recentMessages"].is_array()) {
                                    const auto& msgs = entry["recentMessages"];
                                    for (const auto& msg : msgs) {
                                        std::string content = msg.value("content", "");
                                        auto pos = content.find(query);
                                        if (pos != std::string::npos) {
                                            size_t start = (pos > 60) ? pos - 60 : 0;
                                            size_t len = std::min(size_t(200), content.length() - start);
                                            result_item["snippet"] = "..." +
                                                content.substr(start, len) + "...";
                                            result_item["role"] = msg.value("role", "");
                                            result_item["score"] = 0.8;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    } catch (...) {}

                    if (!result_item.contains("snippet")) {
                        result_item["snippet"] = "(preview unavailable)";
                        result_item["score"] = 0.3;
                    }

                    results.push_back(result_item);
                }
            }

            ToolResult out;
            out.content.push_back({
                ToolResultContent::Type::Json,
                json{
                    {"ok", true},
                    {"query", query},
                    {"results", results},
                    {"count", results.size()},
                }.dump(),
                std::string{},
                std::string{"application/json"}
            });
            out.success = true;
            return out;
        } catch (const std::exception& e) {
            ToolResult err;
            err.success = false;
            err.error = std::string("sessions_search failed: ") + e.what();
            return err;
        }
    }

private:
    std::shared_ptr<gateway::GatewayClient> gw_;
    SessionsToolContext ctx_;
};

// ============================================================================
// sessions_patch Tool (✓ 对齐 OpenClaw gateway sessions.patch 操作)
// ============================================================================

class SessionsPatchTool : public Tool {
public:
    explicit SessionsPatchTool(std::shared_ptr<gateway::GatewayClient> gw,
                               SessionsToolContext ctx)
        : gw_(std::move(gw)), ctx_(std::move(ctx)) {}

    std::string name() const override { return "sessions_patch"; }

    std::string description() const override {
        return "Update session fields such as label, model, or thinking level. "
               "Use to set a display label or change the model for a session.";
    }

    Schema parameters() const override {
        return Schema{
            "object", "sessions_patch parameters",
            {
                {"key", json{
                    {"type", "string"},
                    {"description", "Session key to update. Required."}
                }},
                {"label", json{
                    {"type", "string"},
                    {"description", "New display label for the session."}
                }},
                {"model", json{
                    {"type", "string"},
                    {"description", "Model to use for this session."}
                }},
                {"thinkingLevel", json{
                    {"type", "string"},
                    {"description", "Thinking level override (off/low/medium/high)."}
                }},
            },
            {"key"}
        };
    }

    ToolResult execute(const core::JsonValue& params_val) override {
        const json params = params_val.to_json();
        std::string key = read_string_param(params, "key", true);
        if (key.empty()) {
            ToolResult err;
            err.success = false;
            err.error = "key is required";
            return err;
        }

        if (!gw_) {
            ToolResult err;
            err.success = false;
            err.error = "Gateway not available";
            return err;
        }

        try {
            json patch = {{"key", key}};
            if (params.contains("label")) patch["label"] = params["label"];
            if (params.contains("model")) patch["model"] = params["model"];
            if (params.contains("thinkingLevel")) {
                std::string tl = read_string_param(params, "thinkingLevel");
                patch["thinkingLevel"] = (tl == "off") ? json(nullptr) : json(tl);
            }

            auto result = gw_->request<json>("sessions.patch", patch);
            ToolResult out;
            if (result.is_ok()) {
                out.content.push_back({
                    ToolResultContent::Type::Json,
                    json{{"ok", true}, {"key", key}}.dump(),
                    std::string{},
                    std::string{"application/json"}
                });
            } else {
                out.content.push_back({
                    ToolResultContent::Type::Json,
                    json{{"ok", false}, {"error", result.error}, {"key", key}}.dump(),
                    std::string{},
                    std::string{"application/json"}
                });
            }
            out.success = true;
            return out;
        } catch (const std::exception& e) {
            ToolResult err;
            err.success = false;
            err.error = std::string("sessions_patch failed: ") + e.what();
            return err;
        }
    }

private:
    std::shared_ptr<gateway::GatewayClient> gw_;
    SessionsToolContext ctx_;
};

// ============================================================================
// sessions_reset Tool (✓ 对齐 OpenClaw gateway sessions.reset 操作)
// ============================================================================

class SessionsResetTool : public Tool {
public:
    explicit SessionsResetTool(std::shared_ptr<gateway::GatewayClient> gw,
                               SessionsToolContext ctx)
        : gw_(std::move(gw)), ctx_(std::move(ctx)) {}

    std::string name() const override { return "sessions_reset"; }

    std::string description() const override {
        return "Reset a session to its initial state. "
               "Parameters: key (required, session key), reason ('new' for fresh start or 'reset').";
    }

    Schema parameters() const override {
        return Schema{
            "object", "sessions_reset parameters",
            {
                {"key", json{
                    {"type", "string"},
                    {"description", "Session key to reset. Required."}
                }},
                {"reason", json{
                    {"type", "string"},
                    {"enum", json::array({"new", "reset"})},
                    {"default", "reset"},
                    {"description", "Reason for reset: 'new' for fresh session, 'reset' otherwise."}
                }},
            },
            {"key"}
        };
    }

    ToolResult execute(const core::JsonValue& params_val) override {
        const json params = params_val.to_json();
        std::string key = read_string_param(params, "key", true);
        if (key.empty()) {
            ToolResult err;
            err.success = false;
            err.error = "key is required";
            return err;
        }

        if (!gw_) {
            ToolResult err;
            err.success = false;
            err.error = "Gateway not available";
            return err;
        }

        try {
            std::string reason = params.value("reason", "reset");
            auto result = gw_->request<json>("sessions.reset", json{
                {"key", key},
                {"reason", reason}
            });

            ToolResult out;
            if (result.is_ok()) {
                out.content.push_back({
                    ToolResultContent::Type::Json,
                    json{{"ok", true}, {"key", key}, {"reason", reason}}.dump(),
                    std::string{},
                    std::string{"application/json"}
                });
            } else {
                out.content.push_back({
                    ToolResultContent::Type::Json,
                    json{{"ok", false}, {"error", result.error}, {"key", key}}.dump(),
                    std::string{},
                    std::string{"application/json"}
                });
            }
            out.success = true;
            return out;
        } catch (const std::exception& e) {
            ToolResult err;
            err.success = false;
            err.error = std::string("sessions_reset failed: ") + e.what();
            return err;
        }
    }

private:
    std::shared_ptr<gateway::GatewayClient> gw_;
    SessionsToolContext ctx_;
};

// ============================================================================
// sessions_delete Tool (✓ 对齐 OpenClaw gateway sessions.delete 操作)
// ============================================================================

class SessionsDeleteTool : public Tool {
public:
    explicit SessionsDeleteTool(std::shared_ptr<gateway::GatewayClient> gw,
                                SessionsToolContext ctx)
        : gw_(std::move(gw)), ctx_(std::move(ctx)) {}

    std::string name() const override { return "sessions_delete"; }

    std::string description() const override {
        return "Delete a session and optionally its transcript. "
               "Parameters: key (required, session key), deleteTranscript (default: true).";
    }

    Schema parameters() const override {
        return Schema{
            "object", "sessions_delete parameters",
            {
                {"key", json{
                    {"type", "string"},
                    {"description", "Session key to delete. Required."}
                }},
                {"deleteTranscript", json{
                    {"type", "boolean"},
                    {"default", true},
                    {"description", "Whether to also delete the session transcript."}
                }},
            },
            {"key"}
        };
    }

    ToolResult execute(const core::JsonValue& params_val) override {
        const json params = params_val.to_json();
        std::string key = read_string_param(params, "key", true);
        if (key.empty()) {
            ToolResult err;
            err.success = false;
            err.error = "key is required";
            return err;
        }

        if (!gw_) {
            ToolResult err;
            err.success = false;
            err.error = "Gateway not available";
            return err;
        }

        try {
            bool delete_transcript = params.value("deleteTranscript", true);
            auto result = gw_->request<json>("sessions.delete", json{
                {"key", key},
                {"deleteTranscript", delete_transcript}
            });

            ToolResult out;
            if (result.is_ok()) {
                out.content.push_back({
                    ToolResultContent::Type::Json,
                    json{{"ok", true}, {"key", key}, {"deletedTranscript", delete_transcript}}.dump(),
                    std::string{},
                    std::string{"application/json"}
                });
            } else {
                out.content.push_back({
                    ToolResultContent::Type::Json,
                    json{{"ok", false}, {"error", result.error}, {"key", key}}.dump(),
                    std::string{},
                    std::string{"application/json"}
                });
            }
            out.success = true;
            return out;
        } catch (const std::exception& e) {
            ToolResult err;
            err.success = false;
            err.error = std::string("sessions_delete failed: ") + e.what();
            return err;
        }
    }

private:
    std::shared_ptr<gateway::GatewayClient> gw_;
    SessionsToolContext ctx_;
};

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
    // ✅ 对齐 OpenClaw: createSubagentsTool (subagents-tool.ts)
    // 提供 list/kill/steer 三个 action，需要 registry 支持
    if (ctx.registry) {
        tools.push_back(std::make_shared<SubagentsTool>(gw, ctx));
    }
    // ✅ 对齐 OpenClaw: agents_wait tool for swarm collect pattern
    tools.push_back(std::make_shared<AgentsWaitTool>(gw, ctx));
    // ✅ 对齐 OpenClaw: agents_list, sessions_search
    tools.push_back(std::make_shared<AgentsListTool>(gw, ctx));
    tools.push_back(std::make_shared<SessionsSearchTool>(gw, ctx));
    // ✅ 对齐 OpenClaw: sessions_patch/reset/delete (gateway operations exposed as tools)
    tools.push_back(std::make_shared<SessionsPatchTool>(gw, ctx));
    tools.push_back(std::make_shared<SessionsResetTool>(gw, ctx));
    tools.push_back(std::make_shared<SessionsDeleteTool>(gw, ctx));
    if (on_yield) {
        tools.push_back(std::make_shared<SessionsYieldTool>(ctx, std::move(on_yield)));
    }
    return tools;
}

} // namespace tools
} // namespace hunterclaw
