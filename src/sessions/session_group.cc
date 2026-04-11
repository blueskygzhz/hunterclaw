// 严格对齐 OpenClaw config/sessions/group.ts (149行)
#include "hunterclaw/sessions/session_group.h"
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace sessions {

static std::string to_lower(const std::string& s) {
  std::string r = s;
  std::transform(r.begin(), r.end(), r.begin(), ::tolower);
  return r;
}

static std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  auto end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

static std::string normalize_hyphen_slug(const std::string& raw) {
  std::string result;
  for (char c : raw) {
    if (std::isalnum(c)) result += static_cast<char>(::tolower(c));
    else if (c == '-' || c == '_' || c == ' ') {
      if (!result.empty() && result.back() != '-') result += '-';
    }
  }
  while (!result.empty() && result.back() == '-') result.pop_back();
  return result;
}

// 对齐 group.ts:18-29 resolveImplicitGroupSurface
static std::optional<std::pair<std::string, std::string>> resolve_implicit_group_surface(
    const std::string& from, const std::optional<std::string>& chat_type
) {
  if (from.find("@g.us") != std::string::npos) {
    return std::make_pair("whatsapp", "group");
  }
  return std::nullopt;
}

std::string build_group_display_name(
    const std::string& channel,
    const std::optional<std::string>& group_name,
    const std::optional<std::string>& group_id
) {
  std::string name;
  if (group_name && !trim(*group_name).empty()) {
    name = trim(*group_name);
  } else if (group_id && !trim(*group_id).empty()) {
    name = trim(*group_id);
  }
  if (name.empty()) return channel + " group";
  return channel + ":" + name;
}

// 对齐 group.ts:31-149 resolveGroupSessionKey
std::optional<GroupKeyResolution> resolve_group_session_key(
    const std::string& from,
    const std::optional<std::string>& channel,
    const std::optional<std::string>& chat_type,
    const std::optional<std::string>& group_id,
    const std::optional<std::string>& group_name
) {
  std::string norm_chat_type = chat_type ? to_lower(trim(*chat_type)) : "";
  bool is_group = (norm_chat_type == "group" || norm_chat_type == "channel");
  
  // Check implicit group by from address
  auto implicit = resolve_implicit_group_surface(from, chat_type);
  
  if (!is_group && !implicit) return std::nullopt;
  
  std::string resolved_channel;
  std::string resolved_chat_type;
  
  if (is_group) {
    resolved_channel = channel ? to_lower(trim(*channel)) : "";
    resolved_chat_type = norm_chat_type;
  } else if (implicit) {
    resolved_channel = implicit->first;
    resolved_chat_type = implicit->second;
  }
  
  if (resolved_channel.empty()) {
    // Fallback: derive from from address
    auto colon = from.find(':');
    if (colon != std::string::npos) {
      resolved_channel = to_lower(from.substr(0, colon));
    }
  }
  
  // Build group key
  std::string id;
  if (group_id && !trim(*group_id).empty()) {
    id = normalize_hyphen_slug(*group_id);
  } else {
    // Derive from from address
    id = normalize_hyphen_slug(from);
  }
  
  if (id.empty()) return std::nullopt;
  
  std::string key = resolved_channel + ":" + resolved_chat_type + ":" + id;
  
  return GroupKeyResolution{
    key,
    build_group_display_name(resolved_channel, group_name, group_id),
    resolved_channel,
    resolved_chat_type
  };
}

}  // namespace sessions
}  // namespace hunterclaw
