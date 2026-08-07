#pragma once

// 🎯 严格对齐 OpenClaw: src/agents/sandbox/sanitize-env-vars.ts
// 环境变量净化：阻止 API key/token/密码等敏感变量注入子进程 (含 MCP stdio server)。

#include <string>
#include <vector>
#include <map>

namespace hunterclaw {
namespace sandbox {

// 净化结果 (对齐 OpenClaw sanitizeEnvVars 返回值)
struct EnvVarSanitizationResult {
    std::map<std::string, std::string> allowed;   // 通过校验、允许注入的变量
    std::vector<std::string> blocked;              // 被阻断的变量名
    std::vector<std::string> warnings;             // 告警 (如 base64 疑似凭据)
};

// 🎯 对齐 OpenClaw sanitizeEnvVars
// 过滤掉危险的宿主环境变量 (API_KEY/TOKEN/PASSWORD/SECRET 等)。
// strict_mode=true 时仅放行 ALLOWED_ENV_VAR_PATTERNS 白名单变量。
EnvVarSanitizationResult sanitize_env_vars(
    const std::map<std::string, std::string>& env_vars,
    bool strict_mode = false
);

}  // namespace sandbox
}  // namespace hunterclaw
