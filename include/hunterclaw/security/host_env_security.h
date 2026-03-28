#pragma once

#include <string>
#include <vector>
#include <set>
#include <unordered_set>
#include <map>
#include <optional>

namespace hunterclaw {
namespace security {

// ============================================================================
// Host Environment Security Policy - 主机环境变量安全策略
// ============================================================================

/**
 * 🎯 对齐 OpenClaw: host-env-security-policy.json
 *
 * blockedKeys: 完全阻止的环境变量名（精确匹配，大小写不敏感）
 * blockedPrefixes: 阻止的环境变量前缀
 * blockedOverrideKeys: 阻止通过 agent/gateway 覆盖的环境变量名
 * blockedOverridePrefixes: 阻止通过 agent/gateway 覆盖的环境变量前缀
 */

// 🎯 对齐 OpenClaw: HOST_DANGEROUS_ENV_KEY_VALUES (blockedKeys, uppercased)
inline const std::vector<std::string> HOST_DANGEROUS_ENV_KEY_VALUES = {
    "NODE_OPTIONS",
    "NODE_PATH",
    "PYTHONHOME",
    "PYTHONPATH",
    "PERL5LIB",
    "PERL5OPT",
    "RUBYLIB",
    "RUBYOPT",
    "BASH_ENV",
    "ENV",
    "GIT_EXTERNAL_DIFF",
    "GIT_EXEC_PATH",
    "SHELL",
    "SHELLOPTS",
    "PS4",
    "GCONV_PATH",
    "IFS",
    "SSLKEYLOGFILE",
};

// 🎯 对齐 OpenClaw: HOST_DANGEROUS_ENV_PREFIXES (blockedPrefixes, uppercased)
inline const std::vector<std::string> HOST_DANGEROUS_ENV_PREFIXES = {
    "DYLD_",
    "LD_",
    "BASH_FUNC_",
};

// 🎯 对齐 OpenClaw: HOST_DANGEROUS_OVERRIDE_ENV_KEY_VALUES (blockedOverrideKeys, uppercased)
inline const std::vector<std::string> HOST_DANGEROUS_OVERRIDE_ENV_KEY_VALUES = {
    "HOME",
    "ZDOTDIR",
    "GIT_SSH_COMMAND",
    "GIT_SSH",
    "GIT_PROXY_COMMAND",
    "GIT_ASKPASS",
    "SSH_ASKPASS",
    "LESSOPEN",
    "LESSCLOSE",
    "PAGER",
    "MANPAGER",
    "GIT_PAGER",
    "EDITOR",
    "VISUAL",
    "FCEDIT",
    "SUDO_EDITOR",
    "PROMPT_COMMAND",
    "HISTFILE",
    "PERL5DB",
    "PERL5DBCMD",
    "OPENSSL_CONF",
    "OPENSSL_ENGINES",
    "PYTHONSTARTUP",
    "WGETRC",
    "CURL_HOME",
};

// 🎯 对齐 OpenClaw: HOST_DANGEROUS_OVERRIDE_ENV_PREFIXES (blockedOverridePrefixes, uppercased)
inline const std::vector<std::string> HOST_DANGEROUS_OVERRIDE_ENV_PREFIXES = {
    "GIT_CONFIG_",
    "NPM_CONFIG_",
};

// 🎯 对齐 OpenClaw: HOST_SHELL_WRAPPER_ALLOWED_OVERRIDE_ENV_KEY_VALUES
inline const std::vector<std::string> HOST_SHELL_WRAPPER_ALLOWED_OVERRIDE_ENV_KEY_VALUES = {
    "TERM",
    "LANG",
    "LC_ALL",
    "LC_CTYPE",
    "LC_MESSAGES",
    "COLORTERM",
    "NO_COLOR",
    "FORCE_COLOR",
};

// ============================================================================
// Host Env Security Functions - 主机环境变量安全函数
// ============================================================================

/**
 * normalize_env_var_key - 规范化环境变量键名
 *
 * 🎯 对齐 OpenClaw: normalizeEnvVarKey(rawKey, options?)
 * - trim 空白
 * - portable=true 时: 只允许 [A-Za-z_][A-Za-z0-9_]* 格式
 * - 返回 nullopt 表示无效键名
 */
std::optional<std::string> normalize_env_var_key(
    const std::string& raw_key,
    bool portable = false
);

/**
 * is_dangerous_host_env_var_name - 检查环境变量名是否危险
 *
 * 🎯 对齐 OpenClaw: isDangerousHostEnvVarName(rawKey)
 * - 检查是否在 HOST_DANGEROUS_ENV_KEY_VALUES 中
 * - 检查是否以 HOST_DANGEROUS_ENV_PREFIXES 中的前缀开头
 */
bool is_dangerous_host_env_var_name(const std::string& raw_key);

/**
 * is_dangerous_host_env_override_var_name - 检查环境变量名是否危险（覆盖场景）
 *
 * 🎯 对齐 OpenClaw: isDangerousHostEnvOverrideVarName(rawKey)
 * - 检查是否在 HOST_DANGEROUS_OVERRIDE_ENV_KEY_VALUES 中
 * - 检查是否以 HOST_DANGEROUS_OVERRIDE_ENV_PREFIXES 中的前缀开头
 */
bool is_dangerous_host_env_override_var_name(const std::string& raw_key);

/**
 * sanitize_host_exec_env - 清理主机执行环境变量
 *
 * 🎯 对齐 OpenClaw: sanitizeHostExecEnv(params?)
 *
 * @param base_env 基础环境变量（默认: 当前进程环境）
 * @param overrides 覆盖变量（可选）
 * @param block_path_overrides 是否阻止 PATH 覆盖（默认: true）
 * @return 清理后的环境变量 map
 *
 * 逻辑:
 * 1. 从 base_env 中过滤掉危险变量
 * 2. 合并 overrides（过滤危险覆盖变量）
 * 3. 如果 block_path_overrides=true，阻止 PATH 被覆盖
 */
std::map<std::string, std::string> sanitize_host_exec_env(
    const std::map<std::string, std::string>* base_env = nullptr,
    const std::map<std::string, std::string>* overrides = nullptr,
    bool block_path_overrides = true
);

/**
 * sanitize_system_run_env_overrides - 清理系统运行环境覆盖变量
 *
 * 🎯 对齐 OpenClaw: sanitizeSystemRunEnvOverrides(params?)
 *
 * @param overrides 覆盖变量（可选）
 * @param shell_wrapper 是否为 shell wrapper 模式（只允许特定变量）
 * @return 清理后的覆盖变量（nullopt 表示无覆盖）
 */
std::optional<std::map<std::string, std::string>> sanitize_system_run_env_overrides(
    const std::map<std::string, std::string>* overrides = nullptr,
    bool shell_wrapper = false
);

}  // namespace security
}  // namespace hunterclaw
