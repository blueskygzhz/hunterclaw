#include "hunterclaw/context/context_visibility.h"

#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace context {

// ============================================================================
// Evaluate (对齐 security/context-visibility.ts:16-38)
// ============================================================================

ContextVisibilityDecision evaluate_supplemental_context_visibility(
    ContextVisibilityMode mode,
    ContextVisibilityKind kind,
    bool sender_allowed
) {
  // 对齐 evaluateSupplementalContextVisibility
  if (mode == ContextVisibilityMode::ALL) {
    return {true, ContextVisibilityDecisionReason::MODE_ALL};
  }
  if (sender_allowed) {
    return {true, ContextVisibilityDecisionReason::SENDER_ALLOWED};
  }
  if (mode == ContextVisibilityMode::ALLOWLIST_QUOTE && kind == ContextVisibilityKind::QUOTE) {
    return {true, ContextVisibilityDecisionReason::QUOTE_OVERRIDE};
  }
  return {false, ContextVisibilityDecisionReason::BLOCKED};
}

bool should_include_supplemental_context(
    ContextVisibilityMode mode,
    ContextVisibilityKind kind,
    bool sender_allowed
) {
  return evaluate_supplemental_context_visibility(mode, kind, sender_allowed).include;
}

// ============================================================================
// Config Resolution (对齐 config/context-visibility.ts:19-46)
// ============================================================================

static std::string normalize_lowercase(const std::string& s) {
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(), ::tolower);
  auto start = result.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  auto end = result.find_last_not_of(" \t\n\r");
  return result.substr(start, end - start + 1);
}

std::optional<ContextVisibilityMode> resolve_default_context_visibility(
    const std::string& config_json
) {
  // 对齐 resolveDefaultContextVisibility
  try {
    auto cfg = json::parse(config_json);
    if (cfg.contains("channels") && cfg["channels"].is_object() &&
        cfg["channels"].contains("defaults") && cfg["channels"]["defaults"].is_object()) {
      std::string mode = cfg["channels"]["defaults"].value("contextVisibility", "");
      return string_to_context_visibility_mode(mode);
    }
  } catch (...) {}
  return std::nullopt;
}

ContextVisibilityMode resolve_channel_context_visibility_mode(
    const std::string& config_json,
    const std::string& channel,
    const std::optional<std::string>& account_id,
    const std::optional<ContextVisibilityMode>& configured
) {
  // 对齐 resolveChannelContextVisibilityMode
  // Priority: configured > account override > channel default > global default > ALL
  if (configured) {
    return *configured;
  }
  
  try {
    auto cfg = json::parse(config_json);
    
    // Check channel-specific account override
    if (account_id && !account_id->empty() &&
        cfg.contains("channels") && cfg["channels"].is_object()) {
      auto& channels = cfg["channels"];
      if (channels.contains(channel) && channels[channel].is_object()) {
        auto& ch = channels[channel];
        if (ch.contains("accounts") && ch["accounts"].is_object()) {
          std::string norm_account = normalize_lowercase(*account_id);
          for (auto& [k, v] : ch["accounts"].items()) {
            if (normalize_lowercase(k) == norm_account && v.is_object()) {
              std::string mode = v.value("contextVisibility", "");
              auto m = string_to_context_visibility_mode(mode);
              if (m) return *m;
            }
          }
        }
      }
    }
    
    // Check channel default
    if (cfg.contains("channels") && cfg["channels"].is_object() &&
        cfg["channels"].contains(channel) && cfg["channels"][channel].is_object()) {
      std::string mode = cfg["channels"][channel].value("contextVisibility", "");
      auto m = string_to_context_visibility_mode(mode);
      if (m) return *m;
    }
    
    // Check global default
    auto global_default = resolve_default_context_visibility(config_json);
    if (global_default) return *global_default;
    
  } catch (...) {}
  
  return ContextVisibilityMode::ALL;
}

// ============================================================================
// Conversion
// ============================================================================

std::string context_visibility_mode_to_string(ContextVisibilityMode mode) {
  switch (mode) {
    case ContextVisibilityMode::ALL: return "all";
    case ContextVisibilityMode::ALLOWLIST: return "allowlist";
    case ContextVisibilityMode::ALLOWLIST_QUOTE: return "allowlist_quote";
  }
  return "all";
}

std::optional<ContextVisibilityMode> string_to_context_visibility_mode(const std::string& str) {
  std::string lower = normalize_lowercase(str);
  if (lower == "all") return ContextVisibilityMode::ALL;
  if (lower == "allowlist") return ContextVisibilityMode::ALLOWLIST;
  if (lower == "allowlist_quote") return ContextVisibilityMode::ALLOWLIST_QUOTE;
  return std::nullopt;
}

}  // namespace context
}  // namespace hunterclaw
