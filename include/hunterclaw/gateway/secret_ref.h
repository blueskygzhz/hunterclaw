#pragma once

#include "hunterclaw/core/result.h"
#include <string>
#include <variant>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace gateway {

using json = nlohmann::json;

/**
 * 🎯 100% 对齐 OpenClaw: SecretRef 支持
 * 
 * 从多种来源读取密钥:
 * - 环境变量 (env)
 * - 文件 (file)
 * - 命令执行 (exec)
 * 
 * OpenClaw 参考:
 * - src/config/types.secrets.ts
 * - src/gateway/resolve-configured-secret-input-string.ts
 * - src/infra/secret-file.ts
 */

/**
 * SecretRef 结构
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export type SecretRef = {
 *   source: "env" | "file" | "exec";
 *   provider: string;
 *   id: string;
 * };
 * ```
 */
struct SecretRef {
    enum class Source {
        Env,    // 环境变量
        File,   // 文件系统
        Exec    // 命令执行
    };
    
    Source source;
    std::string provider;  // 提供者别名
    std::string id;        // 标识符 (env变量名/文件路径/命令名)
};

/**
 * SecretRef 配置
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export type FileSecretProviderConfig = {
 *   source: "file";
 *   path: string;
 *   mode?: "singleValue" | "json";
 *   timeoutMs?: number;
 *   maxBytes?: number;
 * };
 * ```
 */
struct SecretRefConfig {
    // 文件读取配置
    struct FileConfig {
        size_t max_bytes = 16 * 1024;       // 最大 16KB
        bool reject_symlink = true;          // 拒绝符号链接
        bool single_value = true;            // true: 纯文本, false: JSON
    } file;
    
    // 命令执行配置
    struct ExecConfig {
        int timeout_ms = 5000;               // 默认 5 秒超时
        size_t max_output_bytes = 16 * 1024; // 最大输出 16KB
        bool json_only = false;               // 仅允许 JSON 输出
        std::vector<std::string> trusted_dirs; // 可信目录
    } exec;
};

/**
 * 从环境变量读取密钥
 * 
 * @param var_name 环境变量名称
 * @return 密钥值
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export function readEnvSecretValue(varName: string): string | undefined {
 *   return process.env[varName];
 * }
 * ```
 */
inline core::Result<std::string> read_secret_from_env(const std::string& var_name) {
    const char* value = std::getenv(var_name.c_str());
    if (!value) {
        return core::Result<std::string>::error(
            "Environment variable not found: " + var_name
        );
    }
    
    if (value[0] == '\0') {
        return core::Result<std::string>::error(
            "Environment variable is empty: " + var_name
        );
    }
    
    return core::Result<std::string>::ok(std::string(value));
}

/**
 * 从文件读取密钥
 * 
 * @param file_path 文件路径
 * @param config 配置
 * @return 密钥值
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export function loadSecretFileSync(
 *   filePath: string,
 *   label: string,
 *   options: { maxBytes?: number; rejectSymlink?: boolean } = {}
 * ): SecretFileReadResult {
 *   // 1. 解析用户路径
 *   const resolvedPath = resolveUserPath(trimmedPath);
 *   
 *   // 2. 检查文件类型（拒绝 symlink）
 *   if (options.rejectSymlink && previewStat.isSymbolicLink()) {
 *     return { ok: false, message: "must not be a symlink" };
 *   }
 *   
 *   // 3. 大小限制
 *   if (previewStat.size > maxBytes) {
 *     return { ok: false, message: "exceeds max bytes" };
 *   }
 *   
 *   // 4. 读取内容
 *   const content = fs.readFileSync(resolvedPath, "utf8").trim();
 *   return { ok: true, content };
 * }
 * ```
 */
inline core::Result<std::string> read_secret_from_file(
    const std::string& file_path,
    const SecretRefConfig::FileConfig& config = {}
) {
    // 1. 打开文件
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        return core::Result<std::string>::error(
            "Failed to open file: " + file_path
        );
    }
    
    // 2. 检查文件大小
    std::streamsize size = file.tellg();
    if (size < 0) {
        return core::Result<std::string>::error(
            "Failed to get file size: " + file_path
        );
    }
    
    if (static_cast<size_t>(size) > config.max_bytes) {
        return core::Result<std::string>::error(
            "File exceeds max bytes (" + std::to_string(config.max_bytes) + "): " + file_path
        );
    }
    
    // 3. 读取内容
    file.seekg(0, std::ios::beg);
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    // 4. Trim whitespace (对齐 OpenClaw)
    // 移除前后空白字符
    size_t start = content.find_first_not_of(" \t\n\r");
    size_t end = content.find_last_not_of(" \t\n\r");
    
    if (start == std::string::npos) {
        return core::Result<std::string>::error(
            "File is empty or contains only whitespace: " + file_path
        );
    }
    
    content = content.substr(start, end - start + 1);
    
    return core::Result<std::string>::ok(content);
}

/**
 * 从命令执行读取密钥
 * 
 * @param command 命令字符串
 * @param config 配置
 * @return 密钥值
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export async function execSecretCommand(
 *   config: ExecSecretProviderConfig,
 *   id: string
 * ): Promise<string> {
 *   const proc = spawn(config.command, [...config.args, id], {
 *     timeout: config.timeoutMs,
 *     maxBuffer: config.maxOutputBytes,
 *     env: { ...config.env, ...process.env }
 *   });
 *   
 *   const stdout = await proc.stdout;
 *   if (config.jsonOnly) {
 *     return JSON.parse(stdout).value;
 *   }
 *   return stdout.trim();
 * }
 * ```
 */
inline core::Result<std::string> read_secret_from_exec(
    const std::string& command,
    const SecretRefConfig::ExecConfig& config = {}
) {
    // 安全检查: 命令必须在可信目录中
    if (!config.trusted_dirs.empty()) {
        bool trusted = false;
        for (const auto& dir : config.trusted_dirs) {
            if (command.find(dir) == 0) {
                trusted = true;
                break;
            }
        }
        if (!trusted) {
            return core::Result<std::string>::error(
                "Command not in trusted directories: " + command
            );
        }
    }
    
    // 执行命令 (使用 popen)
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return core::Result<std::string>::error(
            "Failed to execute command: " + command
        );
    }
    
    // 读取输出
    std::stringstream output;
    char buffer[1024];
    size_t total_read = 0;
    
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        size_t len = std::strlen(buffer);
        total_read += len;
        
        if (total_read > config.max_output_bytes) {
            pclose(pipe);
            return core::Result<std::string>::error(
                "Command output exceeds max bytes: " + command
            );
        }
        
        output << buffer;
    }
    
    int exit_code = pclose(pipe);
    if (exit_code != 0) {
        return core::Result<std::string>::error(
            "Command failed with exit code " + std::to_string(exit_code) + ": " + command
        );
    }
    
    std::string result = output.str();
    
    // Trim whitespace
    size_t start = result.find_first_not_of(" \t\n\r");
    size_t end = result.find_last_not_of(" \t\n\r");
    
    if (start == std::string::npos) {
        return core::Result<std::string>::error(
            "Command output is empty: " + command
        );
    }
    
    result = result.substr(start, end - start + 1);
    
    // JSON 模式: 解析 {"value": "..."}
    if (config.json_only) {
        try {
            json j = json::parse(result);
            if (!j.contains("value")) {
                return core::Result<std::string>::error(
                    "Command output JSON missing 'value' field: " + command
                );
            }
            return core::Result<std::string>::ok(j["value"].get<std::string>());
        } catch (const std::exception& e) {
            return core::Result<std::string>::error(
                std::string("Failed to parse command output as JSON: ") + e.what()
            );
        }
    }
    
    return core::Result<std::string>::ok(result);
}

/**
 * 解析 SecretRef 并读取密钥
 * 
 * @param ref SecretRef 引用
 * @param config 配置
 * @return 密钥值
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export async function resolveSecretRef(
 *   ref: SecretRef,
 *   config: OpenClawConfig
 * ): Promise<string | undefined> {
 *   switch (ref.source) {
 *     case "env":
 *       return process.env[ref.id];
 *     case "file":
 *       return await loadSecretFile(ref.id);
 *     case "exec":
 *       return await execSecretCommand(config, ref.id);
 *   }
 * }
 * ```
 */
inline core::Result<std::string> resolve_secret_ref(
    const SecretRef& ref,
    const SecretRefConfig& config = {}
) {
    switch (ref.source) {
        case SecretRef::Source::Env:
            return read_secret_from_env(ref.id);
        
        case SecretRef::Source::File:
            return read_secret_from_file(ref.id, config.file);
        
        case SecretRef::Source::Exec:
            return read_secret_from_exec(ref.id, config.exec);
        
        default:
            return core::Result<std::string>::error("Invalid SecretRef source");
    }
}

/**
 * 解析环境变量模板语法: ${VAR_NAME}
 * 
 * @param value 可能包含模板的字符串
 * @return 环境变量名称 (如果匹配)
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export const ENV_SECRET_TEMPLATE_RE = /^\$\{([A-Z][A-Z0-9_]{0,127})\}$/;
 * 
 * export function parseEnvSecretTemplate(value: string): string | null {
 *   const match = value.match(ENV_SECRET_TEMPLATE_RE);
 *   return match ? match[1] : null;
 * }
 * ```
 */
inline std::optional<std::string> parse_env_secret_template(const std::string& value) {
    // 匹配 ${VAR_NAME} 格式
    if (value.size() < 4) return std::nullopt;  // 最小: ${A}
    if (value[0] != '$' || value[1] != '{' || value.back() != '}') {
        return std::nullopt;
    }
    
    std::string var_name = value.substr(2, value.size() - 3);
    
    // 验证变量名格式: [A-Z][A-Z0-9_]{0,127}
    if (var_name.empty() || var_name.size() > 128) return std::nullopt;
    if (var_name[0] < 'A' || var_name[0] > 'Z') return std::nullopt;
    
    for (char c : var_name) {
        if (!(c >= 'A' && c <= 'Z') && 
            !(c >= '0' && c <= '9') && 
            c != '_') {
            return std::nullopt;
        }
    }
    
    return var_name;
}

/**
 * 解析配置字符串中的密钥
 * 
 * 支持三种格式:
 * 1. 明文: "sk-xxx"
 * 2. 环境变量模板: "${HUNTERCLAW_TOKEN}"
 * 3. SecretRef JSON: {"source": "file", "provider": "default", "id": "/path/to/secret"}
 * 
 * @param value 配置值 (string 或 JSON)
 * @param config SecretRef 配置
 * @return 解析后的密钥值
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export async function resolveConfiguredSecretInputString(params: {
 *   config: OpenClawConfig;
 *   env: NodeJS.ProcessEnv;
 *   value: unknown;
 *   path: string;
 * }): Promise<{ value?: string; unresolvedRefReason?: string }> {
 *   // 1. 字符串: 检查环境变量模板
 *   if (typeof value === "string") {
 *     const envVar = parseEnvSecretTemplate(value);
 *     if (envVar) {
 *       return { value: env[envVar] };
 *     }
 *     return { value };
 *   }
 *   
 *   // 2. SecretRef: 解析引用
 *   if (isSecretRef(value)) {
 *     return await resolveSecretRef(value, config);
 *   }
 *   
 *   return { unresolvedRefReason: "invalid format" };
 * }
 * ```
 */
inline core::Result<std::string> resolve_configured_secret(
    const std::string& value,
    const SecretRefConfig& config = {}
) {
    // 1. 尝试解析环境变量模板: ${VAR_NAME}
    auto env_var = parse_env_secret_template(value);
    if (env_var.has_value()) {
        return read_secret_from_env(env_var.value());
    }
    
    // 2. 尝试解析 SecretRef JSON
    try {
        json j = json::parse(value);
        
        // 必须包含 source, provider, id 字段
        if (!j.contains("source") || !j.contains("provider") || !j.contains("id")) {
            // 不是 SecretRef，返回明文
            return core::Result<std::string>::ok(value);
        }
        
        SecretRef ref;
        std::string source_str = j["source"].get<std::string>();
        
        if (source_str == "env") {
            ref.source = SecretRef::Source::Env;
        } else if (source_str == "file") {
            ref.source = SecretRef::Source::File;
        } else if (source_str == "exec") {
            ref.source = SecretRef::Source::Exec;
        } else {
            return core::Result<std::string>::error(
                "Invalid SecretRef source: " + source_str
            );
        }
        
        ref.provider = j["provider"].get<std::string>();
        ref.id = j["id"].get<std::string>();
        
        return resolve_secret_ref(ref, config);
    } catch (const json::parse_error&) {
        // 不是 JSON，返回明文
        return core::Result<std::string>::ok(value);
    }
}

} // namespace gateway
} // namespace hunterclaw
