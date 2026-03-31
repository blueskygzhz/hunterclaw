#include "hunterclaw/infra/exec_approvals.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw::infra {

// ============================================================================
// ExecApprovalManager
// ============================================================================

ExecApprovalManager::ExecApprovalManager(const ExecApprovalsConfig& config)
    : config_(config) {}

absl::StatusOr<ExecApprovalDecision> ExecApprovalManager::RequestApproval(
    const ExecApprovalRequest& request,
    std::chrono::milliseconds timeout) {
  // Check security policy first
  if (config_.security == ExecSecurity::kDeny) {
    return ExecApprovalDecision::kDenied;
  }

  if (config_.security == ExecSecurity::kFull) {
    return ExecApprovalDecision::kApproved;
  }

  // kAllowlist: check if command is in allowlist
  if (IsAllowlisted(request.command_text)) {
    return ExecApprovalDecision::kApproved;
  }

  // Not in allowlist - check ask policy
  if (config_.ask == ExecAsk::kOff) {
    return ExecApprovalDecision::kDenied;
  }

  if (config_.ask == ExecAsk::kOnMiss || config_.ask == ExecAsk::kAlways) {
    return WaitForUserApproval(request, timeout);
  }

  return ExecApprovalDecision::kDenied;
}

bool ExecApprovalManager::IsAllowlisted(absl::string_view command) const {
  std::string cmd_lower(command);
  std::transform(cmd_lower.begin(), cmd_lower.end(), cmd_lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  for (const auto& entry : config_.allowlist) {
    if (PatternMatch(entry.pattern, cmd_lower)) {
      return true;
    }
  }
  return false;
}

absl::Status ExecApprovalManager::AddAllowlistEntry(
    const ExecAllowlistEntry& entry) {
  // Check for duplicate
  for (const auto& e : config_.allowlist) {
    if (e.id == entry.id) {
      return absl::AlreadyExistsError("Allowlist entry already exists: " +
                                      entry.id);
    }
  }
  config_.allowlist.push_back(entry);
  return absl::OkStatus();
}

absl::Status ExecApprovalManager::RemoveAllowlistEntry(
    absl::string_view entry_id) {
  auto it = std::remove_if(
      config_.allowlist.begin(), config_.allowlist.end(),
      [&](const ExecAllowlistEntry& e) { return e.id == entry_id; });

  if (it == config_.allowlist.end()) {
    return absl::NotFoundError("Allowlist entry not found: " +
                               std::string(entry_id));
  }
  config_.allowlist.erase(it, config_.allowlist.end());
  return absl::OkStatus();
}

void ExecApprovalManager::SetSecurityPolicy(ExecSecurity policy) {
  config_.security = policy;
}

void ExecApprovalManager::UpdateConfig(const ExecApprovalsConfig& config) {
  config_ = config;
}

absl::Status ExecApprovalManager::LoadAllowlistFromFile(
    absl::string_view path) {
  std::string path_str(path);
  if (!fs::exists(path_str)) {
    return absl::NotFoundError("Allowlist file not found: " + path_str);
  }

  std::ifstream file(path_str);
  if (!file.is_open()) {
    return absl::InternalError("Failed to open allowlist file: " + path_str);
  }

  try {
    json j = json::parse(file);

    if (j.contains("allowlist") && j["allowlist"].is_array()) {
      config_.allowlist.clear();
      for (const auto& item : j["allowlist"]) {
        ExecAllowlistEntry entry;
        if (item.contains("id") && item["id"].is_string()) {
          entry.id = item["id"];
        }
        if (item.contains("pattern") && item["pattern"].is_string()) {
          entry.pattern = item["pattern"];
        }
        if (item.contains("last_used_at") &&
            item["last_used_at"].is_number()) {
          entry.last_used_at = item["last_used_at"];
        }
        if (item.contains("last_used_command") &&
            item["last_used_command"].is_string()) {
          entry.last_used_command = item["last_used_command"];
        }
        if (item.contains("last_resolved_path") &&
            item["last_resolved_path"].is_string()) {
          entry.last_resolved_path = item["last_resolved_path"];
        }
        config_.allowlist.push_back(entry);
      }
    }

    if (j.contains("defaults") && j["defaults"].is_object()) {
      const auto& defaults = j["defaults"];
      if (defaults.contains("security") && defaults["security"].is_string()) {
        config_.security = NormalizeExecSecurity(
            defaults["security"].get<std::string>());
      }
      if (defaults.contains("ask") && defaults["ask"].is_string()) {
        config_.ask = NormalizeExecAsk(defaults["ask"].get<std::string>());
      }
      if (defaults.contains("auto_allow_skills") &&
          defaults["auto_allow_skills"].is_boolean()) {
        config_.auto_allow_skills = defaults["auto_allow_skills"];
      }
    }
  } catch (const json::exception& e) {
    return absl::InternalError(std::string("JSON parse error: ") + e.what());
  }

  return absl::OkStatus();
}

absl::Status ExecApprovalManager::SaveAllowlistToFile(
    absl::string_view path) {
  json j;
  j["version"] = 1;

  json defaults;
  defaults["security"] = ExecSecurityToString(config_.security);
  defaults["ask"] = ExecAskToString(config_.ask);
  defaults["auto_allow_skills"] = config_.auto_allow_skills;
  j["defaults"] = defaults;

  json allowlist_array = json::array();
  for (const auto& entry : config_.allowlist) {
    json e;
    e["id"] = entry.id;
    e["pattern"] = entry.pattern;
    if (entry.last_used_at > 0) {
      e["last_used_at"] = entry.last_used_at;
    }
    if (!entry.last_used_command.empty()) {
      e["last_used_command"] = entry.last_used_command;
    }
    if (!entry.last_resolved_path.empty()) {
      e["last_resolved_path"] = entry.last_resolved_path;
    }
    allowlist_array.push_back(e);
  }
  j["allowlist"] = allowlist_array;

  std::string path_str{path};
  std::ofstream file{path_str};
  if (!file.is_open()) {
    return absl::InternalError("Failed to open file for writing: " +
                               std::string(path));
  }
  file << j.dump(2) << std::endl;
  return absl::OkStatus();
}

// ============================================================================
// Private methods
// ============================================================================

bool ExecApprovalManager::DetectObfuscation(
    absl::string_view command) const {
  std::string cmd(command);

  // Check for excessive pipes (command chaining)
  size_t pipe_count = std::count(cmd.begin(), cmd.end(), '|');
  if (pipe_count > 3) return true;

  // Check for excessive backslashes (escape obfuscation)
  size_t backslash_count = std::count(cmd.begin(), cmd.end(), '\\');
  if (backslash_count > 5) return true;

  // Check for non-printable characters
  for (char c : cmd) {
    if (!std::isprint(static_cast<unsigned char>(c)) && c != '\n' &&
        c != '\t') {
      return true;
    }
  }

  // Check for base64 decode patterns
  if (cmd.find("base64") != std::string::npos &&
      cmd.find("|") != std::string::npos) {
    return true;
  }

  return false;
}

bool ExecApprovalManager::IsDangerousCommand(
    absl::string_view command) const {
  static const std::vector<std::string> kDangerousPatterns = {
      "rm -rf /",
      "dd if=",
      "mkfs",
      "> /dev/sda",
      "chmod -R 777 /",
      "chown -R",
      ":(){ :|:& };:",  // fork bomb
  };

  std::string cmd(command);
  for (const auto& pattern : kDangerousPatterns) {
    if (cmd.find(pattern) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool ExecApprovalManager::PatternMatch(absl::string_view pattern,
                                       absl::string_view command) const {
  // Simple wildcard matching (* and ?)
  size_t cmd_idx = 0;
  size_t pat_idx = 0;
  size_t star_idx = std::string::npos;
  size_t match_idx = 0;

  while (cmd_idx < command.size()) {
    if (pat_idx < pattern.size() && pattern[pat_idx] == '*') {
      star_idx = pat_idx;
      match_idx = cmd_idx;
      pat_idx++;
    } else if (pat_idx < pattern.size() &&
               (pattern[pat_idx] == '?' ||
                pattern[pat_idx] == command[cmd_idx])) {
      cmd_idx++;
      pat_idx++;
    } else if (star_idx != std::string::npos) {
      pat_idx = star_idx + 1;
      match_idx++;
      cmd_idx = match_idx;
    } else {
      return false;
    }
  }

  while (pat_idx < pattern.size() && pattern[pat_idx] == '*') {
    pat_idx++;
  }

  return pat_idx == pattern.size();
}

std::string ExecApprovalManager::GenerateCommandPreview(
    const std::vector<std::string>& argv) const {
  std::ostringstream oss;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i > 0) oss << " ";
    // Truncate long arguments
    if (argv[i].size() > 64) {
      oss << argv[i].substr(0, 64) << "...";
    } else {
      oss << argv[i];
    }
  }
  return oss.str();
}

absl::StatusOr<ExecApprovalDecision> ExecApprovalManager::WaitForUserApproval(
    const ExecApprovalRequest& request,
    std::chrono::milliseconds timeout) {
  // Default implementation: deny (no IPC mechanism available)
  // In production, this would communicate with the gateway via IPC/socket
  (void)request;
  (void)timeout;
  return ExecApprovalDecision::kDenied;
}

// ============================================================================
// Global utility functions
// ============================================================================

ExecSecurity NormalizeExecSecurity(absl::string_view value) {
  if (value == "deny") return ExecSecurity::kDeny;
  if (value == "allowlist") return ExecSecurity::kAllowlist;
  if (value == "full") return ExecSecurity::kFull;
  return ExecSecurity::kAllowlist;  // default
}

ExecAsk NormalizeExecAsk(absl::string_view value) {
  if (value == "off") return ExecAsk::kOff;
  if (value == "on-miss") return ExecAsk::kOnMiss;
  if (value == "always") return ExecAsk::kAlways;
  return ExecAsk::kOnMiss;  // default
}

ExecHost NormalizeExecHost(absl::string_view value) {
  if (value == "sandbox") return ExecHost::kSandbox;
  if (value == "gateway") return ExecHost::kGateway;
  if (value == "node") return ExecHost::kNode;
  return ExecHost::kGateway;  // default
}

std::string ExecSecurityToString(ExecSecurity security) {
  switch (security) {
    case ExecSecurity::kDeny:      return "deny";
    case ExecSecurity::kAllowlist: return "allowlist";
    case ExecSecurity::kFull:      return "full";
  }
  return "allowlist";
}

std::string ExecAskToString(ExecAsk ask) {
  switch (ask) {
    case ExecAsk::kOff:    return "off";
    case ExecAsk::kOnMiss: return "on-miss";
    case ExecAsk::kAlways: return "always";
  }
  return "on-miss";
}

std::string ExecHostToString(ExecHost host) {
  switch (host) {
    case ExecHost::kSandbox: return "sandbox";
    case ExecHost::kGateway: return "gateway";
    case ExecHost::kNode:    return "node";
  }
  return "gateway";
}

}  // namespace hunterclaw::infra
