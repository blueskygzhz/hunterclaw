#ifndef HUNTERCLAW_INFRA_EXEC_APPROVALS_H_
#define HUNTERCLAW_INFRA_EXEC_APPROVALS_H_

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace hunterclaw::infra {

// 命令执行审批系统
// 100% 对齐 OpenClaw src/infra/exec-approvals.ts

// 执行主机类型
// 对齐 OpenClaw ExecHost
enum class ExecHost {
  kSandbox,   // 沙箱环境
  kGateway,   // Gateway 进程
  kNode       // Node 进程
};

// 安全策略
// 对齐 OpenClaw ExecSecurity
enum class ExecSecurity {
  kDeny,       // 拒绝所有命令
  kAllowlist,  // 仅允许白名单
  kFull        // 允许所有 (开发模式)
};

// 询问策略
// 对齐 OpenClaw ExecAsk
enum class ExecAsk {
  kOff,       // 不询问
  kOnMiss,    // 白名单未命中时询问
  kAlways     // 总是询问
};

// 审批决策
enum class ExecApprovalDecision {
  kApproved,          // 批准
  kDenied,            // 拒绝
  kTimeout,           // 超时
  kAllowAlways        // 批准并添加到白名单
};

// 命令审批请求
// 对齐 OpenClaw ExecApprovalRequest
struct ExecApprovalRequest {
  std::string id;                                     // 请求 ID
  std::vector<std::string> argv;                      // 命令参数
  std::string command_text;                           // 命令文本
  std::string command_preview;                        // 命令预览 (脱敏)
  std::string cwd;                                    // 工作目录
  std::string agent_id;                               // Agent ID
  std::string session_key;                            // Session 密钥
  std::map<std::string, std::string> env_hash;        // 环境变量哈希
  std::chrono::system_clock::time_point created_at;   // 创建时间
  std::chrono::system_clock::time_point expires_at;   // 过期时间
};

// 审批响应
// 对齐 OpenClaw ExecApprovalResolved
struct ExecApprovalResponse {
  std::string id;                                     // 请求 ID
  ExecApprovalDecision decision;                      // 决策
  std::string resolved_by;                            // 决策者
  std::chrono::system_clock::time_point resolved_at;  // 决策时间
};

// 白名单条目
// 对齐 OpenClaw ExecAllowlistEntry
struct ExecAllowlistEntry {
  std::string id;                 // 条目 ID
  std::string pattern;            // 匹配模式 (支持通配符)
  int64_t last_used_at = 0;       // 最后使用时间
  std::string last_used_command;  // 最后使用的命令
  std::string last_resolved_path; // 最后解析的路径
};

// 审批配置
// 对齐 OpenClaw ExecApprovalsDefaults
struct ExecApprovalsConfig {
  ExecSecurity security = ExecSecurity::kAllowlist;
  ExecAsk ask = ExecAsk::kOnMiss;
  ExecSecurity ask_fallback = ExecSecurity::kDeny;
  bool auto_allow_skills = false;
  std::vector<ExecAllowlistEntry> allowlist;
};

// 命令执行审批管理器
class ExecApprovalManager {
 public:
  explicit ExecApprovalManager(const ExecApprovalsConfig& config);

  // 请求命令执行批准
  // 对齐 OpenClaw requestExecApproval()
  absl::StatusOr<ExecApprovalDecision> RequestApproval(
      const ExecApprovalRequest& request,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(120000));

  // 检查命令是否在白名单中
  // 对齐 OpenClaw isExecAllowlisted()
  bool IsAllowlisted(absl::string_view command) const;

  // 添加白名单条目
  // 对齐 OpenClaw addExecAllowlistEntry()
  absl::Status AddAllowlistEntry(const ExecAllowlistEntry& entry);

  // 移除白名单条目
  absl::Status RemoveAllowlistEntry(absl::string_view entry_id);

  // 设置安全策略
  void SetSecurityPolicy(ExecSecurity policy);

  // 获取当前配置
  ExecApprovalsConfig GetConfig() const { return config_; }

  // 更新配置
  void UpdateConfig(const ExecApprovalsConfig& config);

  // 加载白名单 (从文件)
  absl::Status LoadAllowlistFromFile(absl::string_view path);

  // 保存白名单 (到文件)
  absl::Status SaveAllowlistToFile(absl::string_view path);

 private:
  ExecApprovalsConfig config_;

  // 混淆检测 (Base64, URL 编码等)
  // 对齐 OpenClaw detectExecObfuscation()
  bool DetectObfuscation(absl::string_view command) const;

  // 危险命令检测
  bool IsDangerousCommand(absl::string_view command) const;

  // 模式匹配 (支持通配符)
  bool PatternMatch(absl::string_view pattern, absl::string_view command) const;

  // 生成命令预览 (脱敏)
  std::string GenerateCommandPreview(const std::vector<std::string>& argv) const;

  // 等待用户审批 (通过 IPC/Socket)
  absl::StatusOr<ExecApprovalDecision> WaitForUserApproval(
      const ExecApprovalRequest& request,
      std::chrono::milliseconds timeout);
};

// 全局工具函数

// 规范化安全策略
ExecSecurity NormalizeExecSecurity(absl::string_view value);

// 规范化询问策略
ExecAsk NormalizeExecAsk(absl::string_view value);

// 规范化执行主机
ExecHost NormalizeExecHost(absl::string_view value);

// 枚举转字符串
std::string ExecSecurityToString(ExecSecurity security);
std::string ExecAskToString(ExecAsk ask);
std::string ExecHostToString(ExecHost host);

// ============================================================================
// Policy Merge Functions (对齐 OpenClaw exec-approvals.ts:547-554)
// ============================================================================

// 取更严格的安全策略 (deny < allowlist < full)
// 对齐 OpenClaw: minSecurity(a, b)
ExecSecurity MinSecurity(ExecSecurity a, ExecSecurity b);

// 取更严格的询问策略 (off < on-miss < always)
// 对齐 OpenClaw: maxAsk(a, b)
ExecAsk MaxAsk(ExecAsk a, ExecAsk b);

// 对齐 OpenClaw: requiresExecApproval({ ask, security, analysisOk, allowlistSatisfied })
bool RequiresExecApproval(ExecAsk ask, ExecSecurity security,
                          bool analysis_ok, bool allowlist_satisfied);

}  // namespace hunterclaw::infra

#endif  // HUNTERCLAW_INFRA_EXEC_APPROVALS_H_
