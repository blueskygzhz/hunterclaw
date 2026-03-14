#include "hunterclaw/infra/fs_safe.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace infra {

SafeFileSystem::SafeFileSystem(const Config& config)
    : workspace_root_(config.workspace_root),
      reject_symlinks_(config.reject_symlinks),
      reject_hardlinks_(config.reject_hardlinks),
      max_file_size_(config.max_file_size_mb * 1024 * 1024) {
  workspace_root_ = fs::absolute(workspace_root_).string();
  if (!workspace_root_.empty() &&
      workspace_root_.back() != fs::path::preferred_separator) {
    workspace_root_ += fs::path::preferred_separator;
  }
}

SafeFileSystem::SafeOpenResult SafeFileSystem::SafeOpen(
    const std::string& relative_path) {
  SafeOpenResult result;

  // Resolve path within workspace
  std::string resolved_path;
  try {
    resolved_path = fs::absolute(workspace_root_ + relative_path).string();
  } catch (const std::exception& e) {
    result.success = false;
    result.error_code = ErrorCode::kInvalidPath;
    result.error_message = "Invalid path: " + std::string(e.what());
    return result;
  }

  // Check if path is within workspace
  if (!IsPathInside(workspace_root_, resolved_path)) {
    result.success = false;
    result.error_code = ErrorCode::kOutsideWorkspace;
    result.error_message = "Path is outside workspace";
    return result;
  }

  // Check if path exists
  if (!fs::exists(resolved_path)) {
    result.success = false;
    result.error_code = ErrorCode::kNotFound;
    result.error_message = "File not found";
    return result;
  }

  // Check if path is a regular file (not directory)
  fs::file_status status = fs::status(resolved_path);
  if (fs::is_directory(status)) {
    result.success = false;
    result.error_code = ErrorCode::kNotFile;
    result.error_message = "Path is a directory, not a file";
    return result;
  }

  // Check for symlink (lstat)
  fs::file_status lstat_status = fs::symlink_status(resolved_path);
  if (reject_symlinks_ && fs::is_symlink(lstat_status)) {
    result.success = false;
    result.error_code = ErrorCode::kSymlink;
    result.error_message = "Symlink not allowed";
    return result;
  }

  // Get file statistics
  struct stat file_stat;
  if (stat(resolved_path.c_str(), &file_stat) != 0) {
    result.success = false;
    result.error_code = ErrorCode::kNotFound;
    result.error_message = "Failed to stat file: " + std::string(strerror(errno));
    return result;
  }

  // Check for hardlink
  if (reject_hardlinks_ && file_stat.st_nlink > 1) {
    result.success = false;
    result.error_code = ErrorCode::kInvalidPath;
    result.error_message = "Hardlink not allowed";
    return result;
  }

  // Check file size
  if (max_file_size_ > 0 && file_stat.st_size > max_file_size_) {
    result.success = false;
    result.error_code = ErrorCode::kTooLarge;
    result.error_message = "File size exceeds limit: " +
                           std::to_string(file_stat.st_size) + " > " +
                           std::to_string(max_file_size_);
    return result;
  }

  // Open file with O_NOFOLLOW to prevent TOCTOU attack
  int fd;
#ifdef __linux__
  fd = open(resolved_path.c_str(), O_RDONLY | O_NOFOLLOW);
#else
  // macOS and Windows don't have O_NOFOLLOW, use open + fstat
  fd = open(resolved_path.c_str(), O_RDONLY);
#endif

  if (fd < 0) {
    result.success = false;
    result.error_code = ErrorCode::kNotFound;
    result.error_message = "Failed to open file: " + std::string(strerror(errno));
    return result;
  }

  // Verify file identity (protect against TOCTOU)
  struct stat fd_stat;
  if (fstat(fd, &fd_stat) != 0) {
    close(fd);
    result.success = false;
    result.error_code = ErrorCode::kPathMismatch;
    result.error_message = "Failed to fstat file";
    return result;
  }

  // Compare inode and device to detect race condition
  if (file_stat.st_ino != fd_stat.st_ino ||
      file_stat.st_dev != fd_stat.st_dev) {
    close(fd);
    result.success = false;
    result.error_code = ErrorCode::kPathMismatch;
    result.error_message = "File changed during open (TOCTOU detected)";
    return result;
  }

  // Success
  result.success = true;
  result.file_descriptor = fd;
  result.real_path = resolved_path;
  result.file_size = fd_stat.st_size;
  result.inode = fd_stat.st_ino;
  result.device = fd_stat.st_dev;

  return result;
}

SafeFileSystem::SafeReadResult SafeFileSystem::SafeRead(
    const std::string& relative_path) {
  SafeReadResult result;

  // Open file safely
  SafeOpenResult open_result = SafeOpen(relative_path);
  if (!open_result.success) {
    result.success = false;
    result.error_code = open_result.error_code;
    result.error_message = open_result.error_message;
    return result;
  }

  // Read file content
  std::vector<char> buffer(open_result.file_size);
  ssize_t bytes_read = read(open_result.file_descriptor, buffer.data(),
                             open_result.file_size);
  close(open_result.file_descriptor);

  if (bytes_read < 0) {
    result.success = false;
    result.error_code = ErrorCode::kNotFound;
    result.error_message = "Failed to read file: " + std::string(strerror(errno));
    return result;
  }

  // Success
  result.success = true;
  result.content = std::string(buffer.data(), bytes_read);
  result.real_path = open_result.real_path;
  result.file_size = bytes_read;

  return result;
}

SafeFileSystem::SafeWriteResult SafeFileSystem::SafeWrite(
    const std::string& relative_path, const std::string& content) {
  SafeWriteResult result;

  // Resolve path within workspace
  std::string resolved_path;
  try {
    resolved_path = fs::absolute(workspace_root_ + relative_path).string();
  } catch (const std::exception& e) {
    result.success = false;
    result.error_code = ErrorCode::kInvalidPath;
    result.error_message = "Invalid path: " + std::string(e.what());
    return result;
  }

  // Check if path is within workspace
  if (!IsPathInside(workspace_root_, resolved_path)) {
    result.success = false;
    result.error_code = ErrorCode::kOutsideWorkspace;
    result.error_message = "Path is outside workspace";
    return result;
  }

  // Create parent directories if needed
  fs::path parent_path = fs::path(resolved_path).parent_path();
  try {
    fs::create_directories(parent_path);
  } catch (const std::exception& e) {
    result.success = false;
    result.error_code = ErrorCode::kInvalidPath;
    result.error_message =
        "Failed to create parent directory: " + std::string(e.what());
    return result;
  }

  // Write to temporary file first (atomic write)
  std::string temp_path = resolved_path + ".tmp." +
                           std::to_string(std::time(nullptr));

  std::ofstream temp_file(temp_path, std::ios::binary | std::ios::trunc);
  if (!temp_file.is_open()) {
    result.success = false;
    result.error_code = ErrorCode::kNotFound;
    result.error_message = "Failed to create temp file";
    return result;
  }

  temp_file.write(content.data(), content.size());
  temp_file.close();

  if (temp_file.fail()) {
    fs::remove(temp_path);
    result.success = false;
    result.error_code = ErrorCode::kNotFound;
    result.error_message = "Failed to write temp file";
    return result;
  }

  // Atomically rename temp file to target file
  try {
    fs::rename(temp_path, resolved_path);
  } catch (const std::exception& e) {
    fs::remove(temp_path);
    result.success = false;
    result.error_code = ErrorCode::kNotFound;
    result.error_message = "Failed to rename file: " + std::string(e.what());
    return result;
  }

  // Success
  result.success = true;
  result.real_path = resolved_path;
  result.bytes_written = content.size();

  return result;
}

bool SafeFileSystem::IsPathInside(const std::string& root,
                                  const std::string& target) const {
  if (root.empty() || target.empty()) {
    return false;
  }

  std::string root_normalized = fs::path(root).string();
  std::string target_normalized = fs::path(target).string();

  if (!root_normalized.empty() &&
      root_normalized.back() != fs::path::preferred_separator) {
    root_normalized += fs::path::preferred_separator;
  }

  if (target_normalized.size() < root_normalized.size()) {
    return false;
  }

  return target_normalized.compare(0, root_normalized.size(),
                                    root_normalized) == 0;
}

}  // namespace infra
}  // namespace hunterclaw
