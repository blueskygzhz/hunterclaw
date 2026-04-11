// 严格对齐 OpenClaw auto-reply/command-auth.ts (724行)
// 命令授权: provider 推断 + owner list 解析 + sender 鉴权

#include "hunterclaw/auto_reply/command_auth.h"
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace auto_reply {

static std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  auto end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

static std::string to_lower(const std::string& s) {
  std::string r = s;
  std::transform(r.begin(), r.end(), r.begin(), ::tolower);
  return r;
}

std::string normalize_sender_id(const std::string& raw) {
  return to_lower(trim(raw));
}

bool is_sender_owner(const std::vector<std::string>& owner_list, const std::string& sender_id) {
  // 对齐 command-auth.ts:593-612 isSenderInOwnerList
  if (sender_id.empty()) return false;
  std::string norm = normalize_sender_id(sender_id);
  for (const auto& owner : owner_list) {
    if (normalize_sender_id(owner) == norm) return true;
  }
  return false;
}

// 对齐 command-auth.ts:380-420 resolveExplicitOwners
std::vector<std::string> resolve_explicit_owners(const std::string& config_json) {
  std::vector<std::string> result;
  try {
    auto cfg = json::parse(config_json);
    // config.owners (array)
    if (cfg.contains("owners") && cfg["owners"].is_array()) {
      for (const auto& o : cfg["owners"]) {
        if (o.is_string()) {
          std::string s = trim(o.get<std::string>());
          if (!s.empty()) result.push_back(s);
        } else if (o.is_number()) {
          result.push_back(std::to_string(o.get<int64_t>()));
        }
      }
    }
    // config.owner (single)
    if (result.empty() && cfg.contains("owner")) {
      if (cfg["owner"].is_string()) {
        std::string s = trim(cfg["owner"].get<std::string>());
        if (!s.empty()) result.push_back(s);
      } else if (cfg["owner"].is_number()) {
        result.push_back(std::to_string(cfg["owner"].get<int64_t>()));
      }
    }
  } catch (...) {}
  return result;
}

bool is_allow_all_mode(const std::string& config_json) {
  // 对齐 command-auth.ts:444-460 resolveOwnerAuthorizationState
  try {
    auto cfg = json::parse(config_json);
    if (cfg.contains("security") && cfg["security"].is_object()) {
      auto& sec = cfg["security"];
      if (sec.contains("allowAll") && sec["allowAll"].is_boolean()) {
        return sec["allowAll"].get<bool>();
      }
    }
  } catch (...) {}
  return false;
}

// 对齐 command-auth.ts:176-222 resolveProviderAllowFrom
std::vector<std::string> resolve_provider_allow_from(
    const std::string& config_json,
    const std::string& provider_id,
    const std::optional<std::string>& account_id
) {
  std::vector<std::string> result;
  try {
    auto cfg = json::parse(config_json);
    std::string norm_provider = to_lower(trim(provider_id));
    
    // config.channels.<provider>.allowFrom
    if (cfg.contains("channels") && cfg["channels"].is_object()) {
      for (auto& [k, v] : cfg["channels"].items()) {
        if (to_lower(trim(k)) != norm_provider) continue;
        if (!v.is_object()) continue;
        
        // Account-specific allowFrom
        if (account_id && !account_id->empty() &&
            v.contains("accounts") && v["accounts"].is_object()) {
          std::string norm_account = to_lower(trim(*account_id));
          for (auto& [ak, av] : v["accounts"].items()) {
            if (to_lower(trim(ak)) != norm_account) continue;
            if (av.is_object() && av.contains("allowFrom") && av["allowFrom"].is_array()) {
              for (const auto& item : av["allowFrom"]) {
                if (item.is_string()) {
                  std::string s = trim(item.get<std::string>());
                  if (!s.empty()) result.push_back(s);
                } else if (item.is_number()) {
                  result.push_back(std::to_string(item.get<int64_t>()));
                }
              }
              return result;
            }
          }
        }
        
        // Channel-level allowFrom
        if (v.contains("allowFrom") && v["allowFrom"].is_array()) {
          for (const auto& item : v["allowFrom"]) {
            if (item.is_string()) {
              std::string s = trim(item.get<std::string>());
              if (!s.empty()) result.push_back(s);
            } else if (item.is_number()) {
              result.push_back(std::to_string(item.get<int64_t>()));
            }
          }
          return result;
        }
      }
    }
  } catch (...) {}
  return result;
}

// 对齐 command-auth.ts:563-724 resolveCommandAuthorization
CommandAuthorization resolve_command_authorization(
    const std::string& config_json,
    const std::string& channel,
    const std::optional<std::string>& from,
    const std::optional<std::string>& to,
    const std::optional<std::string>& account_id,
    const std::optional<std::string>& surface
) {
  CommandAuthorization auth;
  auth.from = from;
  auth.to = to;
  
  // Resolve provider
  std::string provider_id = trim(channel);
  if (provider_id.empty() && surface) {
    provider_id = trim(*surface);
  }
  if (!provider_id.empty()) {
    auth.provider_id = provider_id;
  }
  
  // Resolve owner list
  bool allow_all = is_allow_all_mode(config_json);
  auto explicit_owners = resolve_explicit_owners(config_json);
  
  // Build full owner list: explicit owners + provider allowFrom
  auto provider_allow = !provider_id.empty()
    ? resolve_provider_allow_from(config_json, provider_id, account_id)
    : std::vector<std::string>{};
  
  // Merge: explicit owners take precedence
  std::vector<std::string> owner_list = explicit_owners;
  for (const auto& a : provider_allow) {
    bool found = false;
    for (const auto& o : owner_list) {
      if (normalize_sender_id(o) == normalize_sender_id(a)) { found = true; break; }
    }
    if (!found) owner_list.push_back(a);
  }
  auth.owner_list = owner_list;
  
  // Resolve sender
  std::string sender_id;
  if (from && !from->empty()) {
    sender_id = normalize_sender_id(*from);
    auth.sender_id = sender_id;
  }
  
  // Check if sender is owner
  if (!sender_id.empty()) {
    auth.sender_is_owner = is_sender_owner(owner_list, sender_id);
  }
  
  // Authorization decision
  if (allow_all) {
    auth.is_authorized_sender = true;
  } else if (auth.sender_is_owner) {
    auth.is_authorized_sender = true;
  } else if (!sender_id.empty() && !owner_list.empty()) {
    // Check wildcard
    for (const auto& o : owner_list) {
      if (trim(o) == "*") { auth.is_authorized_sender = true; break; }
    }
  } else if (owner_list.empty()) {
    // No owners configured → allow all (default open)
    auth.is_authorized_sender = true;
  }
  
  return auth;
}

}  // namespace auto_reply
}  // namespace hunterclaw
