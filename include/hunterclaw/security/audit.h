#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <unordered_set>
#include <functional>
#include <optional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace security {

// ============================================================================
// Security Audit Types - 安全审计类型
// ============================================================================

/**
 * SecurityAuditSeverity - 审计发现严重性
 * 
 * 对齐 OpenClaw: SecurityAuditSeverity = "info" | "warn" | "critical"
 */
enum class SecurityAuditSeverity {
    Info,       // 信息性发现 (informational)
    Warn,       // 警告级别 (warning)
    Critical    // 严重问题 (critical security issue)
};

/**
 * SecurityAuditFinding - 单个审计发现
 * 
 * 对齐 OpenClaw: SecurityAuditFinding
 * {
 *   checkId: string;           // 检查ID (唯一标识)
 *   severity: SecurityAuditSeverity;  // 严重性
 *   title: string;             // 发现标题
 *   detail: string;            // 详细描述
 *   remediation?: string;      // 修复建议 (可选)
 * }
 */
struct SecurityAuditFinding {
    std::string check_id;               // 检查ID (例: "fs.state_dir.perms_world_writable")
    SecurityAuditSeverity severity;     // 严重级别
    std::string title;                  // 标题 (例: "State dir is world-writable")
    std::string detail;                 // 详细描述
    std::optional<std::string> remediation;  // 修复建议

    nlohmann::json to_json() const;
    static SecurityAuditFinding from_json(const nlohmann::json& j);
};

/**
 * SecurityAuditSummary - 审计摘要统计
 * 
 * 对齐 OpenClaw: SecurityAuditSummary
 * {
 *   critical: number;  // 严重问题数
 *   warn: number;      // 警告数
 *   info: number;      // 信息数
 * }
 */
struct SecurityAuditSummary {
    int critical = 0;  // 严重问题计数
    int warn = 0;      // 警告计数
    int info = 0;      // 信息计数

    nlohmann::json to_json() const;
    static SecurityAuditSummary from_json(const nlohmann::json& j);
};

/**
 * GatewayProbeResult - Gateway 探测结果
 * 
 * 对齐 OpenClaw: SecurityAuditReport.deep.gateway
 */
struct GatewayProbeResult {
    bool attempted = false;           // 是否尝试探测
    std::optional<std::string> url;   // Gateway URL
    bool ok = false;                  // 是否成功
    std::optional<std::string> error; // 错误信息
    struct CloseInfo {
        int code;
        std::string reason;
    };
    std::optional<CloseInfo> close;   // 关闭信息

    nlohmann::json to_json() const;
    static GatewayProbeResult from_json(const nlohmann::json& j);
};

/**
 * SecurityAuditReport - 完整的安全审计报告
 * 
 * 对齐 OpenClaw: SecurityAuditReport
 * {
 *   ts: number;                    // 时间戳
 *   summary: SecurityAuditSummary; // 摘要统计
 *   findings: SecurityAuditFinding[];  // 发现列表
 *   deep?: {
 *     gateway?: GatewayProbeResult;  // Gateway 深度检查 (可选)
 *   };
 * }
 */
struct SecurityAuditReport {
    // 🎯 对齐 OpenClaw: ts: number (Unix ms timestamp)
    int64_t ts = 0;                             // 审计时间戳 (Unix 毫秒) — 对齐 OpenClaw 字段名 "ts"
    SecurityAuditSummary summary;               // 摘要统计
    std::vector<SecurityAuditFinding> findings; // 所有发现
    
    // Deep audit results (optional)
    // 🎯 对齐 OpenClaw: deep?: { gateway?: { attempted, url, ok, error, close } }
    struct DeepResults {
        std::optional<GatewayProbeResult> gateway;
    };
    std::optional<DeepResults> deep;

    nlohmann::json to_json() const;
    static SecurityAuditReport from_json(const nlohmann::json& j);
};

// ============================================================================
// Security Audit Options - 审计选项
// ============================================================================

/**
 * SecurityAuditOptions - 审计配置选项
 * 
 * 对齐 OpenClaw: SecurityAuditOptions
 */
struct SecurityAuditOptions {
    // Core config (required)
    // 🎯 对齐 OpenClaw: config: OpenClawConfig
    nlohmann::json config;              // OpenClaw 配置
    // 🎯 对齐 OpenClaw: sourceConfig?: OpenClawConfig
    std::optional<nlohmann::json> source_config;  // 原始配置 (用于对比)
    
    // Environment
    // 🎯 对齐 OpenClaw: env?: NodeJS.ProcessEnv
    std::map<std::string, std::string> env;  // 环境变量
    // 🎯 对齐 OpenClaw: platform?: NodeJS.Platform
    std::string platform = "linux";          // 平台 (linux, darwin, win32)
    
    // Audit depth control
    // 🎯 对齐 OpenClaw: deep?: boolean
    bool deep = false;                  // 是否执行深度审计 (包括 Gateway 探测)
    // 🎯 对齐 OpenClaw: includeFilesystem?: boolean
    bool include_filesystem = true;     // 是否检查文件系统权限
    // 🎯 对齐 OpenClaw: includeChannelSecurity?: boolean
    bool include_channel_security = true;  // 是否检查 Channel 安全性
    
    // Path overrides
    // 🎯 对齐 OpenClaw: stateDir?: string
    std::optional<std::string> state_dir;   // State 目录路径 (默认: ~/.openclaw/state)
    // 🎯 对齐 OpenClaw: configPath?: string
    std::optional<std::string> config_path; // Config 文件路径
    
    // Timeouts
    // 🎯 对齐 OpenClaw: deepTimeoutMs?: number (default 5000)
    int deep_timeout_ms = 5000;  // Deep audit 超时时间 (毫秒)
    
    // Caching (optional)
    // 🎯 对齐 OpenClaw: configSnapshot?: ConfigFileSnapshot | null
    std::optional<nlohmann::json> config_snapshot;  // 预加载的配置快照
    
    // 🎯 对齐 OpenClaw: codeSafetySummaryCache?: Map<string, Promise<unknown>>
    // In C++: optional shared cache for code-safety summaries across repeated deep audits
    // (DI for testing; nullptr = use internal cache)
    std::shared_ptr<std::map<std::string, std::string>> code_safety_summary_cache;
    
    // 🎯 对齐 OpenClaw: plugins?: ReturnType<typeof listChannelPlugins>
    // Dependency injection for tests — override channel plugin list
    std::optional<std::vector<std::string>> plugins_override;
};

// ============================================================================
// SecurityAuditor - 安全审计器 (核心类)
// ============================================================================

/**
 * SecurityAuditor - 执行安全审计检查
 * 
 * 对齐 OpenClaw: runSecurityAudit(options: SecurityAuditOptions)
 * 
 * 核心功能:
 * 1. 文件系统权限检查 (state_dir, config_path)
 * 2. Gateway 安全检查 (HTTP no-auth, session key override)
 * 3. 配置安全检查 (dangerous flags, secrets in config)
 * 4. Channel 安全检查 (Discord/Slack/Telegram 配置)
 * 5. 工具策略检查 (dangerous tools, HTTP deny list)
 * 6. 深度审计 (Gateway 探测, Docker 检查)
 */
class SecurityAuditor {
public:
    SecurityAuditor() = default;
    
    /**
     * run_audit - 执行完整的安全审计
     * 
     * @param options 审计选项
     * @return SecurityAuditReport 完整的审计报告
     * 
     * 对齐 OpenClaw: runSecurityAudit(options)
     */
    SecurityAuditReport run_audit(const SecurityAuditOptions& options);

private:
    // ========================================================================
    // Filesystem Checks - 文件系统检查
    // ========================================================================
    
    /**
     * collect_filesystem_findings - 收集文件系统安全问题
     * 
     * 检查项:
     * - State dir 权限 (world-writable, group-writable, symlink)
     * - Config file 权限 (world/group-writable, symlink)
     * - Include files 权限
     */
    std::vector<SecurityAuditFinding> collect_filesystem_findings(
        const SecurityAuditOptions& options
    );
    
    /**
     * check_path_permissions - 检查路径权限
     * 
     * @param path 要检查的路径
     * @param is_dir 是否为目录
     * @param check_name 检查名称前缀 (例: "fs.state_dir")
     * @return 发现列表
     */
    std::vector<SecurityAuditFinding> check_path_permissions(
        const std::string& path,
        bool is_dir,
        const std::string& check_name
    );
    
    // ========================================================================
    // Gateway Security Checks - Gateway 安全检查
    // ========================================================================
    
    /**
     * collect_gateway_findings - 收集 Gateway 安全问题
     * 
     * 检查项:
     * - HTTP gateway without auth (gateway.http.enabled without auth)
     * - Session key override (dangerous in multi-user setup)
     * - Tool deny list (dangerous tools allowed over HTTP)
     */
    std::vector<SecurityAuditFinding> collect_gateway_findings(
        const nlohmann::json& config
    );
    
    /**
     * probe_gateway_deep - 深度探测 Gateway 可达性
     * 
     * @param config Gateway 配置
     * @param timeout_ms 超时时间
     * @return Gateway 探测结果
     */
    GatewayProbeResult probe_gateway_deep(
        const nlohmann::json& config,
        int timeout_ms
    );
    
    // ========================================================================
    // Config Security Checks - 配置安全检查
    // ========================================================================
    
    /**
     * collect_config_security_findings - 收集配置安全问题
     * 
     * 检查项:
     * - Dangerous config flags (unsafe permissions, debug mode)
     * - Secrets in config files (plaintext passwords)
     * - Insecure channel configurations
     */
    std::vector<SecurityAuditFinding> collect_config_security_findings(
        const nlohmann::json& config,
        const std::optional<nlohmann::json>& source_config
    );
    
    /**
     * check_dangerous_flags - 检查危险的配置标志
     * 
     * @param config 配置对象
     * @return 发现列表
     */
    std::vector<SecurityAuditFinding> check_dangerous_flags(
        const nlohmann::json& config
    );
    
    /**
     * check_secrets_in_config - 检查配置中的明文密钥
     * 
     * @param config 配置对象
     * @return 发现列表
     */
    std::vector<SecurityAuditFinding> check_secrets_in_config(
        const nlohmann::json& config
    );
    
    // ========================================================================
    // Channel Security Checks - Channel 安全检查
    // ========================================================================
    
    /**
     * collect_channel_security_findings - 收集 Channel 安全问题
     * 
     * 检查项:
     * - Discord bot token exposure
     * - Slack token security
     * - Telegram bot API security
     * - Channel DM policy (direct message handling)
     */
    std::vector<SecurityAuditFinding> collect_channel_security_findings(
        const nlohmann::json& config
    );
    
    // ========================================================================
    // Tool Policy Checks - 工具策略检查
    // ========================================================================
    
    /**
     * collect_tool_policy_findings - 收集工具策略问题
     * 
     * 检查项:
     * - Dangerous tools enabled (exec, spawn, shell)
     * - HTTP gateway tool deny list
     * - File operation permissions
     */
    std::vector<SecurityAuditFinding> collect_tool_policy_findings(
        const nlohmann::json& config
    );
    
    // ========================================================================
    // Summary Helpers - 汇总辅助方法
    // ========================================================================
    
    /**
     * count_by_severity - 按严重性统计发现
     * 
     * @param findings 发现列表
     * @return 统计摘要
     */
    SecurityAuditSummary count_by_severity(
        const std::vector<SecurityAuditFinding>& findings
    ) const;
    
    /**
     * current_timestamp_ms - 获取当前时间戳 (毫秒)
     */
    int64_t current_timestamp_ms() const;
};

// ============================================================================
// Dangerous Tools Constants - 危险工具常量
// ============================================================================

/**
 * DEFAULT_GATEWAY_HTTP_TOOL_DENY - 默认的 HTTP Gateway 工具黑名单
 * 
 * 对齐 OpenClaw: dangerous-tools.ts DEFAULT_GATEWAY_HTTP_TOOL_DENY
 * 
 * 这些工具不应通过 HTTP Gateway 暴露,因为:
 * - sessions_spawn: 远程会话创建 (RCE 风险)
 * - sessions_send: 跨会话消息注入
 * - cron: 持久化自动化控制平面
 * - gateway: Gateway 配置修改
 * - whatsapp_login: 交互式登录 (需要终端)
 */
// 🎯 对齐 OpenClaw: export const DEFAULT_GATEWAY_HTTP_TOOL_DENY = [...] as const
inline const std::vector<std::string> DEFAULT_GATEWAY_HTTP_TOOL_DENY = {
    "sessions_spawn",   // Session orchestration — spawning agents remotely is RCE
    "sessions_send",    // Cross-session injection — message injection across sessions
    "cron",             // Persistent automation control plane
    "gateway",          // Gateway control plane — prevents gateway reconfiguration via HTTP
    "whatsapp_login"    // Interactive setup — requires terminal QR scan, hangs on HTTP
};

/**
 * DANGEROUS_ACP_TOOL_NAMES - ACP 危险工具名称列表
 * 
 * 对齐 OpenClaw: DANGEROUS_ACP_TOOL_NAMES (array)
 */
// 🎯 对齐 OpenClaw: export const DANGEROUS_ACP_TOOL_NAMES = [...] as const
inline const std::vector<std::string> DANGEROUS_ACP_TOOL_NAMES = {
    "exec", "spawn", "shell",
    "sessions_spawn", "sessions_send",
    "gateway",
    "fs_write", "fs_delete", "fs_move",
    "apply_patch"
};

/**
 * DANGEROUS_ACP_TOOLS - ACP (Automation Control Plane) 危险工具集合
 * 
 * 对齐 OpenClaw: DANGEROUS_ACP_TOOLS = new Set<string>(DANGEROUS_ACP_TOOL_NAMES)
 * 
 * 这些工具在自动化场景下需要显式用户批准
 */
// 🎯 对齐 OpenClaw: export const DANGEROUS_ACP_TOOLS = new Set<string>(DANGEROUS_ACP_TOOL_NAMES)
inline const std::unordered_set<std::string> DANGEROUS_ACP_TOOLS(
    DANGEROUS_ACP_TOOL_NAMES.begin(), DANGEROUS_ACP_TOOL_NAMES.end()
);

// ============================================================================
// External Content Security - 外部内容安全
// ============================================================================

/**
 * ExternalContentSource - 外部内容来源
 * 
 * 对齐 OpenClaw: external-content.ts
 */
enum class ExternalContentSource {
    Email,
    Webhook,
    API,
    Browser,
    ChannelMetadata,
    WebSearch,
    WebFetch,
    Unknown
};

/**
 * detect_suspicious_patterns - 检测可疑的提示注入模式
 * 
 * @param content 要检查的内容
 * @return 匹配的可疑模式列表
 * 
 * 对齐 OpenClaw: detectSuspiciousPatterns(content)
 */
std::vector<std::string> detect_suspicious_patterns(const std::string& content);

/**
 * wrap_external_content - 安全包装外部内容
 * 
 * @param content 外部内容
 * @param source 内容来源
 * @return 包装后的安全内容 (带边界标记和警告)
 * 
 * 对齐 OpenClaw: wrapExternalContent(content, source)
 * 
 * 添加:
 * - 唯一的边界标记 (防止伪造)
 * - 安全警告
 * - 来源标识
 */
std::string wrap_external_content(
    const std::string& content,
    ExternalContentSource source
);

// ============================================================================
// Utility Functions - 工具函数
// ============================================================================

/**
 * severity_to_string - 严重性转字符串
 */
std::string severity_to_string(SecurityAuditSeverity severity);

/**
 * string_to_severity - 字符串转严重性
 */
SecurityAuditSeverity string_to_severity(const std::string& severity_str);

/**
 * source_to_string - 内容来源转字符串
 */
std::string source_to_string(ExternalContentSource source);

}  // namespace security
}  // namespace hunterclaw
