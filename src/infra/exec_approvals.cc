#include "hunterclaw/infra/exec_approvals.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace infra {

ExecApprovalSystem::ExecApprovalSystem(const Config& config)
    : security_level_(config.security_level),
      ask_mode_(config.ask_mode),
      auto_allow_skills_(config.auto_allow_skills),
      allowlist_(config.allowlist),
      approval_callback_(config.approval_callback) {}

ExecApprovalSystem::CheckResult ExecApprovalSystem::CheckCommand(
    const std::string& command, const std::vector<std::string>& argv,
    const std::unordered_map<std::string, std::string>& env,
    const std::string& cwd) {
  CheckResult result;
  result.allowed = false;

  // Parse command and detect obfuscation
  ObfuscationCheckResult obfuscation =
      DetectCommandObfuscation(command, argv);

  if (obfuscation.is_obfuscated) {
    result.allowed = false;
    result.reason = "Command obfuscation detected";
    result.violations.push_back(CommandViolation{
        .type = ViolationType::kObfuscation,
        .description = obfuscation.description,
        .command = command,
        .risk_score = obfuscation.risk_score});
    return result;
  }

  // Check security level
  if (security_level_ == SecurityLevel::kDeny) {
    result.allowed = false;
    result.reason = "All commands denied by security policy";
    return result;
  }

  if (security_level_ == SecurityLevel::kFull) {
    result.allowed = true;
    result.reason = "Full security mode (no restrictions)";
    return result;
  }

  // SecurityLevel::kAllowlist - check against allowlist
  result.allowed = IsCommandAllowlisted(command, argv, cwd);

  if (result.allowed) {
    result.reason = "Command matches allowlist";
    return result;
  }

  // Not in allowlist - check ask mode
  if (ask_mode_ == AskMode::kOff) {
    result.allowed = false;
    result.reason = "Command not in allowlist (ask mode: off)";
    return result;
  }

  if (ask_mode_ == AskMode::kOnMiss || ask_mode_ == AskMode::kAlways) {
    // Request user approval
    bool user_approved = RequestUserApproval(command, argv, env, cwd);

    if (user_approved) {
      result.allowed = true;
      result.reason = "User approved command";

      // Optionally add to allowlist for future
      // (implementation depends on user preference)
    } else {
      result.allowed = false;
      result.reason = "User denied command";
    }

    return result;
  }

  result.allowed = false;
  result.reason = "Command not in allowlist and ask mode is off";
  return result;
}

bool ExecApprovalSystem::IsCommandAllowlisted(
    const std::string& command, const std::vector<std::string>& argv,
    const std::string& cwd) const {
  // Normalize command for comparison
  std::string normalized_command = NormalizeCommand(command);

  for (const auto& entry : allowlist_) {
    if (MatchesPattern(normalized_command, entry.pattern)) {
      return true;
    }
  }

  return false;
}

ExecApprovalSystem::ObfuscationCheckResult
ExecApprovalSystem::DetectCommandObfuscation(
    const std::string& command, const std::vector<std::string>& argv) const {
  ObfuscationCheckResult result;
  result.is_obfuscated = false;
  result.risk_score = 0;

  // Check for suspicious patterns
  std::vector<std::string> suspicious_patterns = {
      // Base64 encoding
      "base64",
      "echo.*|.*base64",
      "printf.*|.*base64",

      // Hex encoding
      "\\x[0-9a-fA-F]{2}",
      "0x[0-9a-fA-F]+",

      // Unicode escapes
      "\\u[0-9a-fA-F]{4}",

      // Command substitution obfuscation
      "$(.*)",
      "`.*`",

      // Eval obfuscation
      "eval",
      "exec",

      // Shell variable obfuscation
      "${.*}",
      "\\$\\(.*\\)",

      // Compression
      "gzip",
      "gunzip",
      "tar.*z",

      // Encoding tricks
      "iconv",
      "tr",
      "sed 's/",
  };

  for (const auto& pattern : suspicious_patterns) {
    if (command.find(pattern) != std::string::npos) {
      result.is_obfuscated = true;
      result.risk_score += 10;
      result.description += "Suspicious pattern: " + pattern + "; ";
    }
  }

  // Check for excessive pipes (command chaining)
  size_t pipe_count = std::count(command.begin(), command.end(), '|');
  if (pipe_count > 3) {
    result.is_obfuscated = true;
    result.risk_score += pipe_count * 5;
    result.description += "Excessive command chaining (" +
                          std::to_string(pipe_count) + " pipes); ";
  }

  // Check for excessive backslashes (escape obfuscation)
  size_t backslash_count =
      std::count(command.begin(), command.end(), '\\');
  if (backslash_count > 5) {
    result.is_obfuscated = true;
    result.risk_score += backslash_count * 2;
    result.description += "Excessive escape characters; ";
  }

  // Check for non-printable characters
  for (char c : command) {
    if (!std::isprint(static_cast<unsigned char>(c)) && c != '\n' &&
        c != '\t') {
      result.is_obfuscated = true;
      result.risk_score += 20;
      result.description += "Non-printable characters detected; ";
      break;
    }
  }

  return result;
}

bool ExecApprovalSystem::RequestUserApproval(
    const std::string& command, const std::vector<std::string>& argv,
    const std::unordered_map<std::string, std::string>& env,
    const std::string& cwd) {
  if (!approval_callback_) {
    return false;  // No approval callback, deny
  }

  // Build approval request
  ApprovalRequest request;
  request.command = command;
  request.argv = argv;
  request.env_keys = ExtractEnvKeys(env);
  request.cwd = cwd;
  request.timestamp = std::time(nullptr);

  // Call approval callback (blocking)
  return approval_callback_(request);
}

std::string ExecApprovalSystem::NormalizeCommand(
    const std::string& command) const {
  std::string normalized = command;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return normalized;
}

bool ExecApprovalSystem::MatchesPattern(const std::string& command,
                                        const std::string& pattern) const {
  // Simple wildcard matching (* and ?)
  size_t cmd_idx = 0;
  size_t pat_idx = 0;
  size_t star_idx = std::string::npos;
  size_t match_idx = 0;

  while (cmd_idx < command.length()) {
    if (pat_idx < pattern.length() && pattern[pat_idx] == '*') {
      star_idx = pat_idx;
      match_idx = cmd_idx;
      pat_idx++;
    } else if (pat_idx < pattern.length() &&
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

  while (pat_idx < pattern.length() && pattern[pat_idx] == '*') {
    pat_idx++;
  }

  return pat_idx == pattern.length();
}

std::vector<std::string> ExecApprovalSystem::ExtractEnvKeys(
    const std::unordered_map<std::string, std::string>& env) const {
  std::vector<std::string> keys;
  keys.reserve(env.size());
  for (const auto& [key, value] : env) {
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

void ExecApprovalSystem::LoadAllowlistFromFile(const std::string& file_path) {
  if (!fs::exists(file_path)) {
    return;  // File not found, keep empty allowlist
  }

  std::ifstream file(file_path);
  if (!file.is_open()) {
    return;  // Failed to open, keep empty allowlist
  }

  try {
    json j = json::parse(file);

    if (j.contains("allowlist") && j["allowlist"].is_array()) {
      allowlist_.clear();
      for (const auto& entry : j["allowlist"]) {
        if (entry.contains("pattern") && entry["pattern"].is_string()) {
          AllowlistEntry e;
          e.pattern = entry["pattern"];
          if (entry.contains("last_used_at") &&
              entry["last_used_at"].is_number()) {
            e.last_used_at = entry["last_used_at"];
          }
          if (entry.contains("last_used_command") &&
              entry["last_used_command"].is_string()) {
            e.last_used_command = entry["last_used_command"];
          }
          allowlist_.push_back(e);
        }
      }
    }

    if (j.contains("defaults") && j["defaults"].is_object()) {
      const auto& defaults = j["defaults"];
      if (defaults.contains("security") && defaults["security"].is_string()) {
        std::string security = defaults["security"];
        if (security == "deny") {
          security_level_ = SecurityLevel::kDeny;
        } else if (security == "allowlist") {
          security_level_ = SecurityLevel::kAllowlist;
        } else if (security == "full") {
          security_level_ = SecurityLevel::kFull;
        }
      }
      if (defaults.contains("ask") && defaults["ask"].is_string()) {
        std::string ask = defaults["ask"];
        if (ask == "off") {
          ask_mode_ = AskMode::kOff;
        } else if (ask == "on-miss") {
          ask_mode_ = AskMode::kOnMiss;
        } else if (ask == "always") {
          ask_mode_ = AskMode::kAlways;
        }
      }
    }
  } catch (const json::exception& e) {
    // JSON parsing error, keep default settings
  }
}

void ExecApprovalSystem::SaveAllowlistToFile(const std::string& file_path) {
  json j;
  j["version"] = 1;

  json defaults;
  if (security_level_ == SecurityLevel::kDeny) {
    defaults["security"] = "deny";
  } else if (security_level_ == SecurityLevel::kAllowlist) {
    defaults["security"] = "allowlist";
  } else if (security_level_ == SecurityLevel::kFull) {
    defaults["security"] = "full";
  }

  if (ask_mode_ == AskMode::kOff) {
    defaults["ask"] = "off";
  } else if (ask_mode_ == AskMode::kOnMiss) {
    defaults["ask"] = "on-miss";
  } else if (ask_mode_ == AskMode::kAlways) {
    defaults["ask"] = "always";
  }

  defaults["auto_allow_skills"] = auto_allow_skills_;
  j["defaults"] = defaults;

  json allowlist_array = json::array();
  for (const auto& entry : allowlist_) {
    json e;
    e["pattern"] = entry.pattern;
    if (entry.last_used_at > 0) {
      e["last_used_at"] = entry.last_used_at;
    }
    if (!entry.last_used_command.empty()) {
      e["last_used_command"] = entry.last_used_command;
    }
    allowlist_array.push_back(e);
  }
  j["allowlist"] = allowlist_array;

  // Write to file
  std::ofstream file(file_path);
  if (file.is_open()) {
    file << j.dump(2) << std::endl;
  }
}

}  // namespace infra
}  // namespace hunterclaw
