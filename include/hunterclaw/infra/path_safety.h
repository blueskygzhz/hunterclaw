#ifndef HUNTERCLAW_INFRA_PATH_SAFETY_H_
#define HUNTERCLAW_INFRA_PATH_SAFETY_H_

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace hunterclaw::infra {

// 路径安全检查器
// 100% 对齐 OpenClaw src/infra/path-safety.ts

class PathSafetyChecker {
 public:
  explicit PathSafetyChecker(absl::string_view workspace_root);

  // 检查路径遍历攻击 (../,  ..\ 检测)
  // 对齐 OpenClaw assertNoPathAliasEscape()
  absl::Status CheckTraversal(absl::string_view path) const;

  // 检查路径是否在工作区边界内
  // 对齐 OpenClaw isPathInside()
  absl::Status CheckWorkspaceBoundary(absl::string_view path) const;

  // 规范化路径 (解析 ~, 相对路径, 符号链接)
  // 对齐 OpenClaw expandHomePrefix() + fs.realpath()
  absl::StatusOr<std::string> NormalizePath(absl::string_view path) const;

  // 检测符号链接
  absl::StatusOr<bool> IsSymlink(absl::string_view path) const;

  // 检测硬链接
  absl::StatusOr<bool> IsHardlink(absl::string_view path) const;

  // 完整路径安全验证 (综合检查)
  absl::Status ValidatePath(absl::string_view path) const;

  // 更新工作区根目录
  void SetWorkspaceRoot(absl::string_view new_root);

  // 获取工作区根目录
  std::string GetWorkspaceRoot() const { return workspace_root_; }

 private:
  std::string workspace_root_;

  // 内部辅助函数
  bool ContainsTraversalSequence(absl::string_view path) const;
  absl::StatusOr<std::string> ExpandHomePath(absl::string_view path) const;
  absl::StatusOr<std::string> ResolveCanonicalPath(absl::string_view path) const;
  bool IsPathInside(const std::string& root, const std::string& path) const;  // ✅ 添加缺失声明
};

// 全局路径安全工具函数

// 检查路径是否安全 (快速检查,不依赖实例)
absl::Status QuickPathSafetyCheck(absl::string_view path);

// 检测路径中的危险字符
bool ContainsDangerousChars(absl::string_view path);

// 规范化路径分隔符 (统一为 /)
std::string NormalizePathSeparators(absl::string_view path);

}  // namespace hunterclaw::infra

#endif  // HUNTERCLAW_INFRA_PATH_SAFETY_H_
