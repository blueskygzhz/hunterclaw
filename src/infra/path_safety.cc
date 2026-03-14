#include "hunterclaw/infra/path_safety.h"

#include <filesystem>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace infra {

PathSafetyChecker::PathSafetyChecker(const Config& config)
    : workspace_root_(config.workspace_root),
      allow_home_access_(config.allow_home_access),
      block_patterns_(config.block_patterns) {
  // Normalize workspace root to absolute path with trailing separator
  workspace_root_ = fs::absolute(workspace_root_).string();
  if (!workspace_root_.empty() &&
      workspace_root_.back() != fs::path::preferred_separator) {
    workspace_root_ += fs::path::preferred_separator;
  }
}

PathSafetyChecker::CheckResult PathSafetyChecker::CheckPath(
    const std::string& path,
    const std::unordered_map<std::string, std::string>& env) const {
  CheckResult result;
  result.is_safe = true;

  // Expand ~ to home directory
  std::string expanded_path = ExpandHomePath(path, env);

  // Convert to absolute path
  fs::path abs_path;
  try {
    abs_path = fs::absolute(expanded_path);
  } catch (const std::exception& e) {
    result.is_safe = false;
    result.violations.push_back(PathViolation{
        .type = ViolationType::kInvalidPath,
        .description = "Invalid path: " + std::string(e.what()),
        .path = path});
    return result;
  }

  std::string abs_path_str = abs_path.string();
  result.resolved_path = abs_path_str;

  // Check for path traversal attempts (../)
  if (path.find("..") != std::string::npos) {
    result.is_safe = false;
    result.violations.push_back(PathViolation{
        .type = ViolationType::kPathTraversal,
        .description =
            "Path contains \"..\" (potential path traversal attack)",
        .path = path,
        .resolved_path = abs_path_str});
  }

  // Check if path is within workspace root
  if (!IsPathInside(workspace_root_, abs_path_str)) {
    result.is_safe = false;
    result.violations.push_back(PathViolation{
        .type = ViolationType::kOutsideWorkspace,
        .description = "Path is outside workspace root",
        .path = path,
        .resolved_path = abs_path_str,
        .workspace_root = workspace_root_});
    return result;
  }

  // Check against block patterns (e.g., *.ssh/*, /etc/passwd)
  for (const auto& pattern : block_patterns_) {
    if (MatchesPattern(abs_path_str, pattern)) {
      result.is_safe = false;
      result.violations.push_back(PathViolation{
          .type = ViolationType::kBlockedPattern,
          .description = "Path matches blocked pattern: " + pattern,
          .path = path,
          .resolved_path = abs_path_str,
          .blocked_pattern = pattern});
    }
  }

  return result;
}

std::string PathSafetyChecker::ExpandHomePath(
    const std::string& path,
    const std::unordered_map<std::string, std::string>& env) const {
  if (path.empty() || path[0] != '~') {
    return path;
  }

  std::string home_dir;
  auto home_it = env.find("HOME");
  if (home_it != env.end()) {
    home_dir = home_it->second;
  } else {
#ifdef _WIN32
    auto userprofile_it = env.find("USERPROFILE");
    if (userprofile_it != env.end()) {
      home_dir = userprofile_it->second;
    }
#else
    home_dir = std::getenv("HOME");
#endif
  }

  if (home_dir.empty()) {
    return path;  // Cannot expand, return as-is
  }

  if (path.length() == 1) {
    return home_dir;
  }

  if (path[1] == '/' || path[1] == '\\') {
    return home_dir + path.substr(1);
  }

  return path;  // ~user not supported, return as-is
}

bool PathSafetyChecker::IsPathInside(const std::string& root,
                                     const std::string& target) const {
  // Both paths must be absolute
  if (root.empty() || target.empty()) {
    return false;
  }

  // Normalize paths for comparison
  std::string root_normalized = fs::path(root).string();
  std::string target_normalized = fs::path(target).string();

  // Ensure root has trailing separator
  if (!root_normalized.empty() &&
      root_normalized.back() != fs::path::preferred_separator) {
    root_normalized += fs::path::preferred_separator;
  }

  // Check if target starts with root
  if (target_normalized.size() < root_normalized.size()) {
    return false;
  }

  return target_normalized.compare(0, root_normalized.size(),
                                    root_normalized) == 0;
}

bool PathSafetyChecker::MatchesPattern(const std::string& path,
                                       const std::string& pattern) const {
  // Simple wildcard matching (* and ?)
  // For production, use std::regex or glob library

  size_t path_idx = 0;
  size_t pattern_idx = 0;
  size_t star_idx = std::string::npos;
  size_t match_idx = 0;

  while (path_idx < path.length()) {
    if (pattern_idx < pattern.length() && pattern[pattern_idx] == '*') {
      star_idx = pattern_idx;
      match_idx = path_idx;
      pattern_idx++;
    } else if (pattern_idx < pattern.length() &&
               (pattern[pattern_idx] == '?' ||
                pattern[pattern_idx] == path[path_idx])) {
      path_idx++;
      pattern_idx++;
    } else if (star_idx != std::string::npos) {
      pattern_idx = star_idx + 1;
      match_idx++;
      path_idx = match_idx;
    } else {
      return false;
    }
  }

  while (pattern_idx < pattern.length() && pattern[pattern_idx] == '*') {
    pattern_idx++;
  }

  return pattern_idx == pattern.length();
}

}  // namespace infra
}  // namespace hunterclaw
