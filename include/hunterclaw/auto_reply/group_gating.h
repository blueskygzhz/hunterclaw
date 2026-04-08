// Aligned with openclaw:
//   src/web/auto-reply/monitor/group-gating.ts
//   src/auto-reply/group-activation.ts
//   src/auto-reply/send-policy.ts
#pragma once

#include <string>
#include <optional>
#include <map>
#include <vector>
#include <regex>

namespace hunterclaw {
namespace auto_reply {

struct GroupHistoryEntry;  // forward decl

// ============================================================================
// GroupActivationMode (aligned with openclaw: GroupActivationMode)
// ============================================================================

enum class GroupActivationMode {
    Mention,  // "mention" — respond only when @-mentioned
    Always    // "always" — respond to every group message
};

std::optional<GroupActivationMode> normalize_group_activation(const std::string& raw);

struct GroupActivationParseResult {
    bool has_command = false;
    std::optional<GroupActivationMode> mode;
};

GroupActivationParseResult parse_activation_command(const std::string& raw);

// ============================================================================
// SendPolicyOverride (aligned with openclaw: SendPolicyOverride)
// ============================================================================

enum class SendPolicyOverride {
    Allow,    // "allow" / "on"
    Deny,     // "deny" / "off"
    Inherit   // "inherit" / "default" / "reset"
};

std::optional<SendPolicyOverride> normalize_send_policy_override(const std::string& raw);

struct SendPolicyParseResult {
    bool has_command = false;
    std::optional<SendPolicyOverride> mode;
};

SendPolicyParseResult parse_send_policy_command(const std::string& raw);

// ============================================================================
// GroupGatingParams / GroupGatingResult
// Aligned with openclaw: applyGroupGating (group-gating.ts)
// ============================================================================

struct GroupGatingParams {
    std::string conversation_id;
    std::string group_history_key;
    std::string msg_body;
    std::string sender_display;
    std::string message_id;
    int64_t timestamp = 0;

    // Allowlist
    bool group_allowlist_enabled = false;
    bool group_allowed = true;

    // Activation
    GroupActivationMode default_activation = GroupActivationMode::Mention;
    bool was_mentioned = false;
    bool is_owner_sender = false;

    // History (optional)
    std::map<std::string, std::vector<GroupHistoryEntry>>* history_map = nullptr;
    int history_limit = 50;
};

struct GroupGatingResult {
    bool should_process = false;
    std::optional<GroupActivationMode> activation_command_mode;  // if /activation was issued
};

GroupGatingResult apply_group_gating(const GroupGatingParams& params);

} // namespace auto_reply
} // namespace hunterclaw
