#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace secrets {

// ============================================================================
// Secret Reference Types - 密钥引用类型
// ============================================================================

/**
 * SecretRefSource - 密钥来源
 * 
 * 对齐 OpenClaw: SecretRefSource
 */
enum class SecretRefSource {
    File,       // 文件密钥提供者
    Exec,       // 执行命令密钥提供者
    Env,        // 环境变量密钥提供者
    Unknown
};

/**
 * SecretRef - 密钥引用
 * 
 * 对齐 OpenClaw: SecretRef
 * {
 *   source: SecretRefSource;  // 密钥来源
 *   provider: string;         // 提供者名称
 *   refId: string;            // 引用ID
 * }
 */
struct SecretRef {
    SecretRefSource source;     // 密钥来源
    std::string provider;       // 提供者名称 (例: "default", "env", "1password")
    std::string ref_id;         // 引用ID (例: "api_key", "DB_PASSWORD")
    
    // Helper to create unique key
    std::string key() const;
    
    nlohmann::json to_json() const;
    static SecretRef from_json(const nlohmann::json& j);
};

/**
 * SecretRefValue - 解析后的密钥值
 * 
 * 可以是 string, number, boolean 或 JSON object
 */
using SecretRefValue = nlohmann::json;

// ============================================================================
// Secret Provider Configuration - 密钥提供者配置
// ============================================================================

/**
 * FileSecretProviderConfig - 文件密钥提供者配置
 * 
 * 对齐 OpenClaw: FileSecretProviderConfig
 * {
 *   type: "file";
 *   path: string;              // 密钥文件路径
 *   format?: "json" | "env";   // 文件格式
 * }
 */
struct FileSecretProviderConfig {
    std::string type = "file";
    std::string path;                           // 文件路径 (支持 ~/ 展开)
    std::string format = "json";                // 格式: "json" 或 "env"
    std::optional<int> max_bytes;               // 最大文件大小 (字节)
    std::optional<int> timeout_ms;              // 读取超时 (毫秒)
    
    nlohmann::json to_json() const;
    static FileSecretProviderConfig from_json(const nlohmann::json& j);
};

/**
 * ExecSecretProviderConfig - 执行命令密钥提供者配置
 * 
 * 对齐 OpenClaw: ExecSecretProviderConfig
 * {
 *   type: "exec";
 *   command: string;           // 执行命令
 *   args?: string[];           // 命令参数
 *   format?: "json" | "env";   // 输出格式
 * }
 */
struct ExecSecretProviderConfig {
    std::string type = "exec";
    std::string command;                        // 执行命令 (例: "op", "vault")
    std::vector<std::string> args;              // 命令参数
    std::string format = "json";                // 输出格式: "json" 或 "env"
    std::optional<int> timeout_ms;              // 执行超时 (毫秒)
    std::optional<int> max_output_bytes;        // 最大输出大小 (字节)
    
    nlohmann::json to_json() const;
    static ExecSecretProviderConfig from_json(const nlohmann::json& j);
};

/**
 * SecretProviderConfig - 统一的密钥提供者配置
 * 
 * 对齐 OpenClaw: SecretProviderConfig
 */
struct SecretProviderConfig {
    std::string type;  // "file" 或 "exec"
    std::optional<FileSecretProviderConfig> file_config;
    std::optional<ExecSecretProviderConfig> exec_config;
    
    bool is_file() const { return type == "file"; }
    bool is_exec() const { return type == "exec"; }
    
    nlohmann::json to_json() const;
    static SecretProviderConfig from_json(const nlohmann::json& j);
};

// ============================================================================
// Secret Resolution - 密钥解析
// ============================================================================

/**
 * SecretResolverWarning - 密钥解析警告
 * 
 * 对齐 OpenClaw: SecretResolverWarning
 * {
 *   code: string;      // 警告代码
 *   path: string;      // 配置路径
 *   message: string;   // 警告消息
 * }
 */
struct SecretResolverWarning {
    std::string code;       // 警告代码 (例: "SECRETS_REF_IGNORED_INACTIVE_SURFACE")
    std::string path;       // 配置路径 (例: "gateway.http.auth.password")
    std::string message;    // 警告消息
    
    nlohmann::json to_json() const;
    static SecretResolverWarning from_json(const nlohmann::json& j);
};

/**
 * SecretResolutionResult - 密钥解析结果
 * 
 * @param success 是否成功
 * @param value 解析的值 (成功时)
 * @param error 错误消息 (失败时)
 */
struct SecretResolutionResult {
    bool success;
    std::optional<SecretRefValue> value;
    std::optional<std::string> error;
    
    static SecretResolutionResult ok(const SecretRefValue& val) {
        return {true, val, std::nullopt};
    }
    
    static SecretResolutionResult fail(const std::string& err) {
        return {false, std::nullopt, err};
    }
};

/**
 * SecretRefResolveCache - 密钥解析缓存
 * 
 * 对齐 OpenClaw: SecretRefResolveCache
 * 
 * 缓存已解析的密钥引用,避免重复解析
 */
class SecretRefResolveCache {
public:
    SecretRefResolveCache() = default;
    
    /**
     * get_resolved - 获取已解析的密钥
     * 
     * @param ref_key 密钥引用 key
     * @return 解析结果 (如果已缓存)
     */
    std::optional<SecretResolutionResult> get_resolved(const std::string& ref_key) const;
    
    /**
     * set_resolved - 缓存解析结果
     * 
     * @param ref_key 密钥引用 key
     * @param result 解析结果
     */
    void set_resolved(const std::string& ref_key, const SecretResolutionResult& result);
    
    /**
     * clear - 清空缓存
     */
    void clear();

private:
    std::map<std::string, SecretResolutionResult> resolved_by_ref_key_;
};

// ============================================================================
// Auth Profile Store - 认证配置存储
// ============================================================================

/**
 * AuthProfileStore - 认证配置存储
 * 
 * 对齐 OpenClaw: AuthProfileStore
 * 
 * 存储 Agent 的认证配置 (API keys, tokens, etc.)
 */
struct AuthProfileStore {
    std::string agent_dir;                      // Agent 目录路径
    std::map<std::string, nlohmann::json> profiles;  // 认证配置 map
    
    nlohmann::json to_json() const;
    static AuthProfileStore from_json(const nlohmann::json& j);
};

// ============================================================================
// Prepared Secrets Runtime Snapshot - 预准备的密钥运行时快照
// ============================================================================

/**
 * PreparedSecretsRuntimeSnapshot - 预准备的密钥运行时快照
 * 
 * 对齐 OpenClaw: PreparedSecretsRuntimeSnapshot
 * {
 *   sourceConfig: OpenClawConfig;        // 原始配置
 *   config: OpenClawConfig;              // 解析后的配置 (密钥已替换)
 *   authStores: Array<{
 *     agentDir: string;
 *     store: AuthProfileStore;
 *   }>;
 *   warnings: SecretResolverWarning[];   // 解析警告
 * }
 */
struct PreparedSecretsRuntimeSnapshot {
    nlohmann::json source_config;           // 原始配置 (包含 secret 引用)
    nlohmann::json config;                  // 解析后的配置 (secret 已替换为实际值)
    
    struct AuthStoreEntry {
        std::string agent_dir;
        AuthProfileStore store;
    };
    std::vector<AuthStoreEntry> auth_stores;  // 认证配置存储列表
    
    std::vector<SecretResolverWarning> warnings;  // 解析警告
    
    nlohmann::json to_json() const;
    static PreparedSecretsRuntimeSnapshot from_json(const nlohmann::json& j);
};

// ============================================================================
// Secrets Runtime Manager - 密钥运行时管理器
// ============================================================================

/**
 * SecretsRuntimeManager - 密钥运行时管理器
 * 
 * 对齐 OpenClaw: secrets/runtime.ts
 * 
 * 核心功能:
 * 1. prepareSecretsRuntimeSnapshot - 准备密钥运行时快照
 * 2. activateSecretsRuntimeSnapshot - 激活快照
 * 3. getActiveSecretsRuntimeSnapshot - 获取活跃快照
 * 4. clearSecretsRuntimeSnapshot - 清空快照
 * 
 * 工作流程:
 * 1. Prepare: 扫描配置,收集所有 secret 引用
 * 2. Resolve: 从 secret providers 解析实际值
 * 3. Activate: 将解析后的配置激活为运行时配置
 * 4. Refresh: 支持热更新 (配置文件改变时自动重新解析)
 */
class SecretsRuntimeManager {
public:
    SecretsRuntimeManager() = default;
    
    /**
     * prepare_snapshot - 准备密钥运行时快照
     * 
     * @param config OpenClaw 配置
     * @param env 环境变量
     * @param agent_dirs Agent 目录列表 (可选)
     * @param cache 解析缓存 (可选)
     * @return 准备好的快照
     * 
     * 对齐 OpenClaw: prepareSecretsRuntimeSnapshot(params)
     * 
     * 步骤:
     * 1. 克隆原始配置
     * 2. 扫描配置,收集 secret 引用 (例: ${secret:api_key})
     * 3. 从 secret providers 解析实际值
     * 4. 替换配置中的 secret 引用为实际值
     * 5. 加载 auth profile stores
     * 6. 返回快照 (包含原始配置和解析后配置)
     */
    PreparedSecretsRuntimeSnapshot prepare_snapshot(
        const nlohmann::json& config,
        const std::map<std::string, std::string>& env = {},
        const std::vector<std::string>& agent_dirs = {},
        std::shared_ptr<SecretRefResolveCache> cache = nullptr
    );
    
    /**
     * activate_snapshot - 激活密钥运行时快照
     * 
     * @param snapshot 要激活的快照
     * 
     * 对齐 OpenClaw: activateSecretsRuntimeSnapshot(snapshot)
     * 
     * 将快照设置为活跃状态,后续的配置查询将使用解析后的配置
     */
    void activate_snapshot(const PreparedSecretsRuntimeSnapshot& snapshot);
    
    /**
     * get_active_snapshot - 获取活跃的密钥运行时快照
     * 
     * @return 活跃快照 (如果有)
     * 
     * 对齐 OpenClaw: getActiveSecretsRuntimeSnapshot()
     */
    std::optional<PreparedSecretsRuntimeSnapshot> get_active_snapshot() const;
    
    /**
     * clear_snapshot - 清空活跃的密钥运行时快照
     * 
     * 对齐 OpenClaw: clearSecretsRuntimeSnapshot()
     */
    void clear_snapshot();
    
    /**
     * resolve_secret_refs - 解析密钥引用列表
     * 
     * @param refs 要解析的密钥引用列表
     * @param config 配置对象
     * @param env 环境变量
     * @param cache 解析缓存
     * @return 解析结果 map (ref.key() -> value)
     * 
     * 对齐 OpenClaw: resolveSecretRefValues(refs, options)
     */
    std::map<std::string, SecretResolutionResult> resolve_secret_refs(
        const std::vector<SecretRef>& refs,
        const nlohmann::json& config,
        const std::map<std::string, std::string>& env = {},
        std::shared_ptr<SecretRefResolveCache> cache = nullptr
    );

private:
    // ========================================================================
    // Secret Reference Collection - 密钥引用收集
    // ========================================================================
    
    /**
     * collect_secret_refs_from_config - 从配置中收集所有密钥引用
     * 
     * @param config 配置对象
     * @param path_prefix 路径前缀 (用于嵌套对象)
     * @param refs 输出: 收集的密钥引用
     * @param warnings 输出: 警告列表
     */
    void collect_secret_refs_from_config(
        const nlohmann::json& config,
        const std::string& path_prefix,
        std::vector<SecretRef>& refs,
        std::vector<SecretResolverWarning>& warnings
    );
    
    /**
     * parse_secret_ref_string - 解析密钥引用字符串
     * 
     * @param value 字符串值 (例: "${secret:api_key}")
     * @return 解析的密钥引用 (如果是有效引用)
     * 
     * 格式:
     * - ${secret:refId}               // 使用默认 provider
     * - ${secret:provider:refId}      // 指定 provider
     * - ${file:path/to/file#key}      // 文件密钥
     * - ${exec:command#key}           // 执行命令密钥
     */
    std::optional<SecretRef> parse_secret_ref_string(const std::string& value);
    
    // ========================================================================
    // Secret Resolution - 密钥解析
    // ========================================================================
    
    /**
     * resolve_file_secret_provider - 解析文件密钥提供者
     * 
     * @param provider_name 提供者名称
     * @param provider_config 提供者配置
     * @param refs 要解析的引用列表
     * @param cache 缓存
     * @return 解析结果 map
     */
    std::map<std::string, SecretResolutionResult> resolve_file_secret_provider(
        const std::string& provider_name,
        const FileSecretProviderConfig& provider_config,
        const std::vector<SecretRef>& refs,
        std::shared_ptr<SecretRefResolveCache> cache
    );
    
    /**
     * resolve_exec_secret_provider - 解析执行命令密钥提供者
     * 
     * @param provider_name 提供者名称
     * @param provider_config 提供者配置
     * @param refs 要解析的引用列表
     * @param cache 缓存
     * @return 解析结果 map
     */
    std::map<std::string, SecretResolutionResult> resolve_exec_secret_provider(
        const std::string& provider_name,
        const ExecSecretProviderConfig& provider_config,
        const std::vector<SecretRef>& refs,
        std::shared_ptr<SecretRefResolveCache> cache
    );
    
    /**
     * resolve_env_secret_provider - 解析环境变量密钥提供者
     * 
     * @param refs 要解析的引用列表
     * @param env 环境变量
     * @return 解析结果 map
     */
    std::map<std::string, SecretResolutionResult> resolve_env_secret_provider(
        const std::vector<SecretRef>& refs,
        const std::map<std::string, std::string>& env
    );
    
    // ========================================================================
    // Config Replacement - 配置替换
    // ========================================================================
    
    /**
     * replace_secret_refs_in_config - 在配置中替换密钥引用为实际值
     * 
     * @param config 配置对象 (会被原地修改)
     * @param resolved_refs 已解析的密钥 map (ref.key() -> value)
     * @param warnings 输出: 警告列表 (替换失败的引用)
     */
    void replace_secret_refs_in_config(
        nlohmann::json& config,
        const std::map<std::string, SecretResolutionResult>& resolved_refs,
        std::vector<SecretResolverWarning>& warnings
    );
    
    // ========================================================================
    // Active Snapshot State - 活跃快照状态
    // ========================================================================
    
    std::optional<PreparedSecretsRuntimeSnapshot> active_snapshot_;  // 当前活跃的快照
};

// ============================================================================
// Utility Functions - 工具函数
// ============================================================================

/**
 * source_to_string - 密钥来源转字符串
 */
std::string source_to_string(SecretRefSource source);

/**
 * string_to_source - 字符串转密钥来源
 */
SecretRefSource string_to_source(const std::string& source_str);

}  // namespace secrets
}  // namespace hunterclaw
