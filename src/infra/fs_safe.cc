#include "hunterclaw/infra/fs_safe.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "absl/strings/str_cat.h"

namespace fs = std::filesystem;

namespace hunterclaw::infra {

SafeFileSystem::SafeFileSystem(absl::string_view workspace_root)
    : workspace_root_(workspace_root) {
  try {
    workspace_root_ = fs::absolute(workspace_root_).string();
  } catch (const std::exception&) {
    // Keep original if conversion fails
  }
}

absl::StatusOr<SafeOpenResult> SafeFileSystem::SafeOpen(
    absl::string_view path,
    int flags,
    bool reject_hardlinks) {
  // Normalize path
  std::string normalized_path;
  try {
    fs::path abs_path = fs::absolute(std::string(path));
    normalized_path = abs_path.string();
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid path: ", e.what()));
  }

  // Check workspace boundary
  if (!IsPathInside(workspace_root_, normalized_path)) {
    return absl::PermissionDeniedError("Path is outside workspace");
  }

  // Check if path exists
  if (!fs::exists(normalized_path)) {
    return absl::NotFoundError("File not found");
  }

  // Check if it's a regular file
  if (!fs::is_regular_file(normalized_path)) {
    return absl::InvalidArgumentError("Not a regular file");
  }

  // Check for symlink using lstat
  struct stat lstat_info;
  if (lstat(normalized_path.c_str(), &lstat_info) != 0) {
    return absl::InternalError(
        absl::StrCat("Failed to lstat: ", strerror(errno)));
  }

  if (S_ISLNK(lstat_info.st_mode)) {
    return absl::PermissionDeniedError("Symlink not allowed");
  }

  // Check for hardlink
  if (reject_hardlinks && lstat_info.st_nlink > 1) {
    return absl::PermissionDeniedError("Hardlink not allowed");
  }

  // Open file with O_NOFOLLOW to prevent TOCTOU
  int open_flags = flags;
  if (open_flags == 0) {
    open_flags = O_RDONLY;
  }
#ifdef __linux__
  open_flags |= O_NOFOLLOW;
#endif

  int fd = open(normalized_path.c_str(), open_flags);
  if (fd < 0) {
    return absl::InternalError(
        absl::StrCat("Failed to open file: ", strerror(errno)));
  }

  // Verify file identity using fstat
  struct stat fstat_info;
  if (fstat(fd, &fstat_info) != 0) {
    close(fd);
    return absl::InternalError(
        absl::StrCat("Failed to fstat: ", strerror(errno)));
  }

  // Compare inode and device to detect TOCTOU
  if (lstat_info.st_ino != fstat_info.st_ino ||
      lstat_info.st_dev != fstat_info.st_dev) {
    close(fd);
    return absl::AbortedError("File changed during open (TOCTOU detected)");
  }

  SafeOpenResult result;
  result.fd = fd;
  result.real_path = normalized_path;
  result.stat_info = fstat_info;

  return result;
}

absl::StatusOr<SafeReadResult> SafeFileSystem::SafeReadFile(
    absl::string_view path,
    bool reject_hardlinks) {
  // Use SafeOpen to get file descriptor
  auto open_result = SafeOpen(path, O_RDONLY, reject_hardlinks);
  if (!open_result.ok()) {
    return open_result.status();
  }

  int fd = open_result->fd;
  off_t file_size = open_result->stat_info.st_size;

  // Read file content
  std::string content(file_size, '\0');
  ssize_t bytes_read = read(fd, content.data(), file_size);
  close(fd);

  if (bytes_read < 0) {
    return absl::InternalError(
        absl::StrCat("Failed to read file: ", strerror(errno)));
  }

  content.resize(bytes_read);

  SafeReadResult result;
  result.content = std::move(content);
  result.real_path = open_result->real_path;
  result.stat_info = open_result->stat_info;

  return result;
}

absl::StatusOr<SafeReadResult> SafeFileSystem::SafeReadFileWithLimit(
    absl::string_view path,
    size_t max_size_bytes,
    bool reject_hardlinks) {
  // Use SafeOpen to get file descriptor
  auto open_result = SafeOpen(path, O_RDONLY, reject_hardlinks);
  if (!open_result.ok()) {
    return open_result.status();
  }

  int fd = open_result->fd;
  off_t file_size = open_result->stat_info.st_size;

  // Check file size limit
  if (static_cast<size_t>(file_size) > max_size_bytes) {
    close(fd);
    return absl::ResourceExhaustedError(
        absl::StrCat("File size exceeds limit: ", file_size, " > ",
                     max_size_bytes));
  }

  // Read file content
  std::string content(file_size, '\0');
  ssize_t bytes_read = read(fd, content.data(), file_size);
  close(fd);

  if (bytes_read < 0) {
    return absl::InternalError(
        absl::StrCat("Failed to read file: ", strerror(errno)));
  }

  content.resize(bytes_read);

  SafeReadResult result;
  result.content = std::move(content);
  result.real_path = open_result->real_path;
  result.stat_info = open_result->stat_info;

  return result;
}

absl::Status SafeFileSystem::SafeWriteFile(
    absl::string_view path,
    absl::string_view content,
    mode_t mode) {
  return AtomicWriteFile(path, content, mode);
}

absl::Status SafeFileSystem::SafeAppendFile(
    absl::string_view path,
    absl::string_view content) {
  // Normalize path
  std::string normalized_path;
  try {
    fs::path abs_path = fs::absolute(std::string(path));
    normalized_path = abs_path.string();
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid path: ", e.what()));
  }

  // Check workspace boundary
  if (!IsPathInside(workspace_root_, normalized_path)) {
    return absl::PermissionDeniedError("Path is outside workspace");
  }

  // Open file in append mode
  int fd = open(normalized_path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd < 0) {
    return absl::InternalError(
        absl::StrCat("Failed to open file: ", strerror(errno)));
  }

  // Write content
  ssize_t bytes_written = write(fd, content.data(), content.size());
  close(fd);

  if (bytes_written < 0 || static_cast<size_t>(bytes_written) != content.size()) {
    return absl::InternalError("Failed to write file");
  }

  return absl::OkStatus();
}

absl::Status SafeFileSystem::SafeDeleteFile(absl::string_view path) {
  // Normalize path
  std::string normalized_path;
  try {
    fs::path abs_path = fs::absolute(std::string(path));
    normalized_path = abs_path.string();
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid path: ", e.what()));
  }

  // Check workspace boundary
  if (!IsPathInside(workspace_root_, normalized_path)) {
    return absl::PermissionDeniedError("Path is outside workspace");
  }

  // Delete file
  try {
    if (!fs::remove(normalized_path)) {
      return absl::NotFoundError("File not found");
    }
  } catch (const std::exception& e) {
    return absl::InternalError(
        absl::StrCat("Failed to delete file: ", e.what()));
  }

  return absl::OkStatus();
}

void SafeFileSystem::SetWorkspaceRoot(absl::string_view new_root) {
  workspace_root_ = std::string(new_root);
  try {
    workspace_root_ = fs::absolute(workspace_root_).string();
  } catch (const std::exception&) {
    // Keep original if conversion fails
  }
}

// Private methods

absl::Status SafeFileSystem::VerifyFileIdentity(
    int fd,
    absl::string_view path,
    const struct stat& expected_stat) {
  struct stat fd_stat;
  if (fstat(fd, &fd_stat) != 0) {
    return absl::InternalError(
        absl::StrCat("Failed to fstat: ", strerror(errno)));
  }

  if (!SameFileIdentity(expected_stat, fd_stat)) {
    return absl::AbortedError("File identity mismatch (TOCTOU detected)");
  }

  return absl::OkStatus();
}

absl::StatusOr<struct stat> SafeFileSystem::GetSafeFileStat(
    int fd,
    absl::string_view path) {
  // lstat for symlink check
  struct stat lstat_info;
  if (lstat(std::string(path).c_str(), &lstat_info) != 0) {
    return absl::InternalError(
        absl::StrCat("Failed to lstat: ", strerror(errno)));
  }

  // fstat for actual file
  struct stat fstat_info;
  if (fstat(fd, &fstat_info) != 0) {
    return absl::InternalError(
        absl::StrCat("Failed to fstat: ", strerror(errno)));
  }

  // Verify they match
  if (lstat_info.st_ino != fstat_info.st_ino ||
      lstat_info.st_dev != fstat_info.st_dev) {
    return absl::AbortedError("File changed (TOCTOU detected)");
  }

  return fstat_info;
}

absl::Status SafeFileSystem::AtomicWriteFile(
    absl::string_view path,
    absl::string_view content,
    mode_t mode) {
  // Normalize path
  std::string normalized_path;
  try {
    fs::path abs_path = fs::absolute(std::string(path));
    normalized_path = abs_path.string();
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid path: ", e.what()));
  }

  // Check workspace boundary
  if (!IsPathInside(workspace_root_, normalized_path)) {
    return absl::PermissionDeniedError("Path is outside workspace");
  }

  // Create parent directories
  fs::path parent_path = fs::path(normalized_path).parent_path();
  try {
    fs::create_directories(parent_path);
  } catch (const std::exception& e) {
    return absl::InternalError(
        absl::StrCat("Failed to create parent directory: ", e.what()));
  }

  // Write to temporary file
  std::string temp_path = normalized_path + ".tmp." +
                          std::to_string(std::time(nullptr));

  std::ofstream temp_file(temp_path, std::ios::binary | std::ios::trunc);
  if (!temp_file.is_open()) {
    return absl::InternalError("Failed to create temp file");
  }

  temp_file.write(content.data(), content.size());
  temp_file.close();

  if (temp_file.fail()) {
    fs::remove(temp_path);
    return absl::InternalError("Failed to write temp file");
  }

  // Set file permissions
  chmod(temp_path.c_str(), mode);

  // Atomically rename
  try {
    fs::rename(temp_path, normalized_path);
  } catch (const std::exception& e) {
    fs::remove(temp_path);
    return absl::InternalError(
        absl::StrCat("Failed to rename file: ", e.what()));
  }

  return absl::OkStatus();
}

bool SafeFileSystem::IsPathInside(const std::string& root,
                                  const std::string& path) const {
  if (root.empty() || path.empty()) {
    return false;
  }

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

bool SameFileIdentity(const struct stat& stat1, const struct stat& stat2) {
  return stat1.st_ino == stat2.st_ino &&
         stat1.st_dev == stat2.st_dev &&
         stat1.st_size == stat2.st_size &&
         stat1.st_mtime == stat2.st_mtime;
}

absl::StatusOr<bool> IsSymlink(absl::string_view path) {
  try {
    fs::path p(path);
    return fs::is_symlink(p);
  } catch (const std::exception& e) {
    return absl::InternalError(
        absl::StrCat("Failed to check symlink: ", e.what()));
  }
}

absl::StatusOr<bool> IsHardlink(absl::string_view path) {
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

std::string SafeOpenErrorCodeToString(SafeOpenErrorCode code) {
  switch (code) {
    case SafeOpenErrorCode::kInvalidPath:
      return "Invalid path";
    case SafeOpenErrorCode::kNotFound:
      return "File not found";
    case SafeOpenErrorCode::kOutsideWorkspace:
      return "Path is outside workspace";
    case SafeOpenErrorCode::kSymlink:
      return "Symlink not allowed";
    case SafeOpenErrorCode::kNotFile:
      return "Not a regular file";
    case SafeOpenErrorCode::kPathMismatch:
      return "Path mismatch (TOCTOU detected)";
    case SafeOpenErrorCode::kTooLarge:
      return "File too large";
    case SafeOpenErrorCode::kHardlink:
      return "Hardlink not allowed";
    default:
      return "Unknown error";
  }
}

}  // namespace hunterclaw::infra
