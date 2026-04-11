// 严格对齐 OpenClaw auto-reply/model.ts (52行) + model-runtime.ts (97行)
#include "hunterclaw/auto_reply/model_directive.h"
#include <algorithm>
#include <regex>
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

static std::string escape_regex(const std::string& s) {
  static const std::string special = R"(\.^$|?*+()[]{}/)";
  std::string result;
  for (char c : s) {
    if (special.find(c) != std::string::npos) result += '\\';
    result += c;
  }
  return result;
}

// 对齐 model.ts:4-51 extractModelDirective
ModelDirectiveResult extract_model_directive(
    const std::string& body,
    const std::vector<std::string>& aliases
) {
  if (body.empty()) return {"", std::nullopt, std::nullopt, false};
  
  // Match /model directive
  std::regex model_re(R"((?:^|\s)/model(?=$|\s|:)\s*:?\s*([A-Za-z0-9_.:@-]+(?:/[A-Za-z0-9_.:@-]+)*)?)");
  std::smatch model_match;
  bool has_model = std::regex_search(body, model_match, model_re);
  
  // Try alias match if no model match
  std::smatch alias_match;
  bool has_alias = false;
  if (!has_model && !aliases.empty()) {
    std::string alias_pattern = "(?:^|\\s)\\/(";
    for (size_t i = 0; i < aliases.size(); ++i) {
      std::string a = trim(aliases[i]);
      if (a.empty()) continue;
      if (i > 0) alias_pattern += "|";
      alias_pattern += escape_regex(a);
    }
    alias_pattern += ")(?=$|\\s|:)(?:\\s*:\\s*)?";
    
    try {
      std::regex alias_re(alias_pattern, std::regex_constants::icase);
      has_alias = std::regex_search(body, alias_match, alias_re);
    } catch (...) {}
  }
  
  std::string raw;
  std::string match_str;
  if (has_model) {
    raw = trim(model_match[1].str());
    match_str = model_match[0].str();
  } else if (has_alias) {
    raw = trim(alias_match[1].str());
    match_str = alias_match[0].str();
  }
  
  if (!has_model && !has_alias) {
    return {trim(body), std::nullopt, std::nullopt, false};
  }
  
  // Split trailing auth profile (e.g. "claude-opus-4@profile1")
  std::optional<std::string> raw_model = raw.empty() ? std::nullopt : std::optional(raw);
  std::optional<std::string> raw_profile;
  if (!raw.empty()) {
    auto at_pos = raw.rfind('@');
    if (at_pos != std::string::npos && at_pos > 0 && at_pos < raw.size() - 1) {
      raw_model = raw.substr(0, at_pos);
      raw_profile = raw.substr(at_pos + 1);
    }
  }
  
  // Remove the matched directive from body
  std::string cleaned = body;
  auto pos = cleaned.find(match_str);
  if (pos != std::string::npos) {
    cleaned.replace(pos, match_str.size(), " ");
  }
  // Collapse whitespace
  cleaned = std::regex_replace(cleaned, std::regex("\\s+"), " ");
  cleaned = trim(cleaned);
  
  return {cleaned, raw_model, raw_profile, true};
}

// 对齐 model-runtime.ts:7-23 formatProviderModelRef
std::string format_provider_model_ref(const std::string& provider, const std::string& model) {
  std::string p = trim(provider);
  std::string m = trim(model);
  if (p.empty()) return m;
  if (m.empty()) return p;
  
  std::string prefix = p + "/";
  std::string lower_m = to_lower(m);
  std::string lower_prefix = to_lower(prefix);
  if (lower_m.substr(0, lower_prefix.size()) == lower_prefix) {
    std::string suffix = trim(m.substr(prefix.size()));
    if (!suffix.empty()) return p + "/" + suffix;
  }
  return p + "/" + m;
}

// 对齐 model-runtime.ts:26-97 resolveSelectedAndActiveModel
SelectedAndActiveModel resolve_selected_and_active_model(const std::string& session_entry_json) {
  SelectedAndActiveModel result;
  result.is_fallback = false;
  
  try {
    auto entry = json::parse(session_entry_json);
    result.selected_provider = entry.value("selectedProvider", entry.value("provider", ""));
    result.selected_model = entry.value("selectedModel", entry.value("model", ""));
    result.active_provider = entry.value("activeProvider", result.selected_provider);
    result.active_model = entry.value("activeModel", result.selected_model);
    
    result.selected_label = format_provider_model_ref(result.selected_provider, result.selected_model);
    result.active_label = format_provider_model_ref(result.active_provider, result.active_model);
    
    result.is_fallback = (to_lower(result.active_label) != to_lower(result.selected_label));
  } catch (...) {}
  
  return result;
}

}  // namespace auto_reply
}  // namespace hunterclaw
