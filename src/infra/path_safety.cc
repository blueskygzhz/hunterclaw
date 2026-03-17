#include "hunterclaw/infra/path_safety.h"

#include <filesystem>
#include <cstdlib>
#include <algorithm>

#include "absl/strings/str_cat.h"

namespace fs = std::filesystem;

namespace hunterclaw::infra {

PathSafetyChecker::PathSafetyChecker(absl::string_view workspace_root)
    : workspace_root_(workspace_root) {
  // Normalize workspace root to absolute path
  try {
    workspace_root_ = fs::absolute(workspace_root_).string();
  } catch (const std::exception&) {
    // Keep original if conversion fails
  }
}

absl::Status PathSafetyChecker::CheckTraversal(absl::string_view path) const {
  if (ContainsTraversalSequence(path)) {
    return absl::InvalidArgumentError(
        "Path contains traversal sequence (../)");
  }
  return absl::OkStatus();
}

absl::Status PathSafetyChecker::CheckWorkspaceBoundary(
    absl::string_view path) const {
  auto normalized = NormalizePath(path);
  if (!normalized.ok()) {
    return normalized.status();
  }
  
  if (!IsPathInside(workspace_root_, *normalized)) {
    return absl::PermissionDeniedError(
        "Path is outside workspace boundary");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> PathSafetyChecker::NormalizePath(
    absl::string_view path) const {
  // Expand home path first
  auto expanded = ExpandHomePath(path);
  if (!expanded.ok()) {
    return expanded;
  }
  
  // Then resolve canonical path
  return ResolveCanonicalPath(*expanded);
}

absl::StatusOr<bool> PathSafetyChecker::IsSymlink(
    absl::string_view path) const {
  try {
    fs::path p(path);
    return fs::is_symlink(p);
  } catch (const std::exception& e) {
    return absl::InternalError(
        absl::StrCat("Failed to check symlink: ", e.what()));
  }
}

absl::StatusOr<bool> PathSafetyChecker::IsHardlink(
    absl::string_view path) const {
  try {
    fs::path p(path);
    if (!fs::exists(p)) {
      return false;
    }
    return fs::hard_link_count(p) > 1;
  } catch (const std::exception& e) {
    return absl::InternalError(
        absl::StrCat("Failed to check hardlink: ", e.what()));
  }
}

absl::Status PathSafetyChecker::ValidatePath(absl::string_view path) const {
  // Check traversal
  auto traversal_check = CheckTraversal(path);
  if (!traversal_check.ok()) {
    return traversal_check;
  }
  
  // Check workspace boundary
  return CheckWorkspaceBoundary(path);
}

void PathSafetyChecker::SetWorkspaceRoot(absl::string_view new_root) {
  workspace_root_ = std::string(new_root);
  try {
    workspace_root_ = fs::absolute(workspace_root_).string();
  } catch (const std::exception&) {
    // Keep original if conversion fails
  }
}

// Private helper methods

bool PathSafetyChecker::ContainsTraversalSequence(
    absl::string_view path) const {
  return path.find("..") != absl::string_view::npos;
}

absl::StatusOr<std::string> PathSafetyChecker::ExpandHomePath(
    absl::string_view path) const {
  if (path.empty() || path[0] != '~') {
    return std::string(path);
  }
  
  const char* home = std::getenv("HOME");
  if (!home) {
#ifdef _WIN32
    home = std::getenv("USERPROFILE");
#endif
  }
  
  if (!home) {
    return absl::NotFoundError("HOME directory not found");
  }
  
  if (path.length() == 1) {
    return std::string(home);
  }
  
  if (path[1] == '/' || path[1] == '\\') {
    return absl::StrCat(home, path.substr(1));
  }
  
  return std::string(path);
}

absl::StatusOr<std::string> PathSafetyChecker::ResolveCanonicalPath(
    absl::string_view path) const {
  try {
    fs::path p(path);
    
    // Try to get canonical path (resolves symlinks)
    if (fs::exists(p)) {
      return fs::canonical(p).string();
    } else {
      // If path doesn't exist, just return absolute form
      return fs::absolute(p).string();
    }
  } catch (const std::exception& e) {
    return absl::InternalError(
        absl::StrCat("Failed to resolve path: ", e.what()));
  }
}

bool PathSafetyChecker::IsPathInside(const std::string& root,
                                      const std::string& path) const {
  if (root.empty() || path.empty()) {
    return false;
  }
  
  // Normalize both paths
  std::string root_normalized = root;
  std::string path_normalized = path;
  
  // Ensure root has trailing separator
  if (!root_normalized.empty() &&
      root_normalized.back() != fs::path::preferred_separator) {
    root_normalized += fs::path::preferred_separator;
  }
  
  // Check if path starts with root
  if (path_normalized.size() < root_normalized.size()) {
    return false;
  }
  
  return path_normalized.compare(0, root_normalized.size(),
                                  root_normalized) == 0;
}

// Global utility functions

absl::Status QuickPathSafetyCheck(absl::string_view path) {
  if (path.find("..") != absl::string_view::npos) {
    return absl::InvalidArgumentError("Path contains traversal sequence");
  }
  if (ContainsDangerousChars(path)) {
    return absl::InvalidArgumentError("Path contains dangerous characters");
  }
  return absl::OkStatus();
}

bool ContainsDangerousChars(absl::string_view path) {
  // Check for null bytes and other dangerous characters
  for (char c : path) {
    if (c == '\0' || c < 0x20) {
      return true;
    }
  }
  return false;
}

std::string NormalizePathSeparators(absl::string_view path) {
  std::string result(path);
  std::replace(result.begin(), result.end(), '\\', '/');
  return result;
}

}  // namespace hunterclaw::infra
