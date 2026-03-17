#ifndef HUNTERCLAW_INFRA_FS_SAFE_H_
#define HUNTERCLAW_INFRA_FS_SAFE_H_

#include <string>
#include <sys/stat.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace hunterclaw::infra {

// 安全文件系统操作
// 100% 对齐 OpenClaw src/infra/fs-safe.ts

// 安全打开结果
// 对齐 OpenClaw SafeOpenResult
struct SafeOpenResult {
  int fd;                    // 文件描述符
  std::string real_path;     // 规范化路径
  struct stat stat_info;     // 文件元数据
};

// 安全读取结果
// 对齐 OpenClaw SafeLocalReadResult
struct SafeReadResult {
  std::string content;       // 文件内容
  std::string real_path;     // 规范化路径
  struct stat stat_info;     // 文件元数据
};

// 安全打开错误码
// 对齐 OpenClaw SafeOpenErrorCode
enum class SafeOpenErrorCode {
  kInvalidPath,       // 非法路径
  kNotFound,          // 文件不存在
  kOutsideWorkspace,  // 路径在工作区外
  kSymlink,           // 符号链接不允许
  kNotFile,           // 不是普通文件
  kPathMismatch,      // 路径不匹配 (TOCTOU 检测)
  kTooLarge,          // 文件过大
  kHardlink           // 硬链接不允许
};

// 安全文件系统类
// 对齐 OpenClaw openVerifiedLocalFile() + safeWriteFile()
class SafeFileSystem {
 public:
  explicit SafeFileSystem(absl::string_view workspace_root);

  // 安全打开文件 (TOCTOU + Symlink + Hardlink 防护)
  // 对齐 OpenClaw openVerifiedLocalFile()
  absl::StatusOr<SafeOpenResult> SafeOpen(
      absl::string_view path,
      int flags = 0,  // O_RDONLY by default
      bool reject_hardlinks = true);

  // 安全读取文件 (完整内容)
  // 对齐 OpenClaw safeReadLocalFile()
  absl::StatusOr<SafeReadResult> SafeReadFile(
      absl::string_view path,
      bool reject_hardlinks = true);

  // 安全读取文件 (指定大小限制)
  absl::StatusOr<SafeReadResult> SafeReadFileWithLimit(
      absl::string_view path,
      size_t max_size_bytes,
      bool reject_hardlinks = true);

  // 安全写入文件 (原子操作,写入临时文件后 rename)
  // 对齐 OpenClaw safeWriteFile()
  absl::Status SafeWriteFile(
      absl::string_view path,
      absl::string_view content,
      mode_t mode = 0644);

  // 安全追加文件
  absl::Status SafeAppendFile(
      absl::string_view path,
      absl::string_view content);

  // 安全删除文件 (验证后删除)
  absl::Status SafeDeleteFile(absl::string_view path);

  // 更新工作区根目录
  void SetWorkspaceRoot(absl::string_view new_root);

 private:
  std::string workspace_root_;

  // 验证文件身份 (防止TOCTOU)
  // 对齐 OpenClaw sameFileIdentity()
  absl::Status VerifyFileIdentity(
      int fd,
      absl::string_view path,
      const struct stat& expected_stat);

  // 获取文件元数据 (fstat + lstat 双重验证)
  absl::StatusOr<struct stat> GetSafeFileStat(
      int fd,
      absl::string_view path);

  // 原子写入 (写临时文件 + rename)
  absl::Status AtomicWriteFile(
      absl::string_view path,
      absl::string_view content,
      mode_t mode);
  
  // 检查路径是否在工作区内  
  bool IsPathInside(const std::string& root, const std::string& path) const;  // ✅ 添加缺失声明
};

// 全局工具函数

// 比较文件身份 (inode + device + size + mtime)
// 对齐 OpenClaw sameFileIdentity()
bool SameFileIdentity(const struct stat& stat1, const struct stat& stat2);

// 检测文件是否为符号链接
absl::StatusOr<bool> IsSymlink(absl::string_view path);

// 检测文件是否为硬链接
absl::StatusOr<bool> IsHardlink(absl::string_view path);

// 错误码转字符串
std::string SafeOpenErrorCodeToString(SafeOpenErrorCode code);

}  // namespace hunterclaw::infra

#endif  // HUNTERCLAW_INFRA_FS_SAFE_H_
