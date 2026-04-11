// 严格对齐 OpenClaw auto-reply/tool-meta.ts (144行)
#include "hunterclaw/auto_reply/tool_meta.h"
#include <algorithm>
#include <map>
#include <regex>
#include <sstream>

namespace hunterclaw {
namespace auto_reply {

// 工具 emoji 映射 (对齐 OpenClaw agents/tool-display.ts)
static const std::map<std::string, std::pair<std::string, std::string>> TOOL_DISPLAY = {
  {"exec",     {"⚡", "exec"}},
  {"bash",     {"⚡", "bash"}},
  {"read",     {"📖", "read"}},
  {"write",    {"✏️", "write"}},
  {"edit",     {"✏️", "edit"}},
  {"search",   {"🔍", "search"}},
  {"message",  {"💬", "message"}},
  {"cron",     {"⏰", "cron"}},
  {"process",  {"🔄", "process"}},
};

static std::string to_lower(const std::string& s) {
  std::string r = s;
  std::transform(r.begin(), r.end(), r.begin(), ::tolower);
  return r;
}

static std::string get_home_dir() {
  const char* home = std::getenv("HOME");
  return home ? home : "";
}

std::string shorten_path(const std::string& path) {
  std::string home = get_home_dir();
  if (!home.empty() && path.find(home) == 0) {
    return "~" + path.substr(home.size());
  }
  return path;
}

std::string shorten_meta(const std::string& meta) {
  if (meta.empty()) return meta;
  std::string home = get_home_dir();
  if (home.empty()) return meta;
  
  std::string result = meta;
  size_t pos = 0;
  while ((pos = result.find(home, pos)) != std::string::npos) {
    result.replace(pos, home.size(), "~");
    pos += 1;
  }
  return result;
}

static bool is_path_like(const std::string& value) {
  if (value.empty()) return false;
  if (value.find(' ') != std::string::npos) return false;
  if (value.find("://") != std::string::npos) return false;
  if (value.find("·") != std::string::npos) return false;
  if (value.find("&&") != std::string::npos || value.find("||") != std::string::npos) return false;
  return std::regex_match(value, std::regex(R"(^~?(/[^\s]+)+$)"));
}

std::string format_tool_aggregate(
    const std::optional<std::string>& tool_name,
    const std::vector<std::string>& metas,
    bool markdown
) {
  std::vector<std::string> filtered;
  for (const auto& m : metas) {
    if (!m.empty()) filtered.push_back(shorten_meta(m));
  }
  
  // Resolve display
  std::string emoji = "🔧";
  std::string label = tool_name.value_or("tool");
  if (tool_name) {
    auto it = TOOL_DISPLAY.find(to_lower(*tool_name));
    if (it != TOOL_DISPLAY.end()) {
      emoji = it->second.first;
      label = it->second.second;
    }
  }
  std::string prefix = emoji + " " + label;
  
  if (filtered.empty()) return prefix;
  
  // Group by directory + brace-collapse (对齐 tool-meta.ts:32-71)
  std::vector<std::string> raw_segments;
  std::map<std::string, std::vector<std::string>> grouped;
  
  for (const auto& m : filtered) {
    if (!is_path_like(m) || m.find("→") != std::string::npos) {
      raw_segments.push_back(m);
      continue;
    }
    auto slash = m.rfind('/');
    if (slash != std::string::npos && slash > 0) {
      std::string dir = m.substr(0, slash);
      std::string base = m.substr(slash + 1);
      grouped[dir].push_back(base);
    } else {
      grouped["."].push_back(m);
    }
  }
  
  std::vector<std::string> segments;
  for (auto& [dir, files] : grouped) {
    std::string brace;
    if (files.size() > 1) {
      brace = "{";
      for (size_t i = 0; i < files.size(); ++i) {
        if (i > 0) brace += ", ";
        brace += files[i];
      }
      brace += "}";
    } else {
      brace = files[0];
    }
    segments.push_back(dir == "." ? brace : dir + "/" + brace);
  }
  
  std::vector<std::string> all;
  all.insert(all.end(), raw_segments.begin(), raw_segments.end());
  all.insert(all.end(), segments.begin(), segments.end());
  
  std::string meta;
  for (size_t i = 0; i < all.size(); ++i) {
    if (i > 0) meta += "; ";
    meta += all[i];
  }
  
  if (markdown && meta.find('`') == std::string::npos) {
    meta = "`" + meta + "`";
  }
  
  return prefix + ": " + meta;
}

std::string format_tool_prefix(
    const std::optional<std::string>& tool_name,
    const std::optional<std::string>& meta
) {
  std::string emoji = "🔧";
  std::string label = tool_name.value_or("tool");
  if (tool_name) {
    auto it = TOOL_DISPLAY.find(to_lower(*tool_name));
    if (it != TOOL_DISPLAY.end()) {
      emoji = it->second.first;
      label = it->second.second;
    }
  }
  
  if (meta) {
    std::string m = shorten_meta(*meta);
    auto s = m.find_first_not_of(" \t");
    if (s != std::string::npos) {
      return emoji + " " + label + ": " + m.substr(s);
    }
  }
  return emoji + " " + label;
}

}  // namespace auto_reply
}  // namespace hunterclaw
