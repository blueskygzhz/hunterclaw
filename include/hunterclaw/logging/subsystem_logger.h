#ifndef HUNTERCLAW_LOGGING_SUBSYSTEM_LOGGER_H
#define HUNTERCLAW_LOGGING_SUBSYSTEM_LOGGER_H

#include <string>
#include <memory>
#include <functional>
#include <map>
#include <optional>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace logging {

/**
 * Logging Subsystem System
 * 100% 对齐 OpenClaw logging/subsystem.ts
 * 
 * 功能:
 * 1. 分层子系统日志记录 (subsystem/child hierarchy)
 * 2. Log Level 控制 (trace/debug/info/warn/error/fatal)
 * 3. 文件与控制台双输出
 * 4. 子系统颜色与格式化
 */

// ============================================================================
// Types (100% 对齐 OpenClaw)
// ============================================================================

/// Log Level (对齐 OpenClaw LogLevel)
enum class LogLevel {
    Silent,  ///< silent - 完全静默
    Fatal,   ///< fatal - 致命错误
    Error,   ///< error - 错误
    Warn,    ///< warn - 警告
    Info,    ///< info - 信息
    Debug,   ///< debug - 调试
    Trace    ///< trace - 追踪
};

/// Log Target (对齐 OpenClaw target parameter)
enum class LogTarget {
    Any,      ///< any - 任意目标 (文件或控制台)
    Console,  ///< console - 仅控制台
    File      ///< file - 仅文件
};

// ============================================================================
// SubsystemLogger Interface (100% 对齐 OpenClaw)
// ============================================================================

/**
 * Subsystem Logger
 * 
 * 核心功能:
 * 1. ✅ isEnabled(level, target) - 检查日志级别是否启用
 * 2. ✅ trace/debug/info/warn/error/fatal - 6 个日志方法
 * 3. ✅ raw(message) - 原始控制台输出
 * 4. ✅ child(name) - 创建子日志器
 * 5. ✅ subsystem - 子系统名称
 * 
 * 对齐 OpenClaw:
 * - SubsystemLogger interface
 * - isEnabled(level, target) 方法
 * - 6 个日志级别方法
 * - raw() 方法 (绕过格式化直接输出)
 * - child() 方法 (创建子系统)
 */
class SubsystemLogger {
public:
    using Ptr = std::shared_ptr<SubsystemLogger>;
    
    virtual ~SubsystemLogger() = default;
    
    /**
     * 获取子系统名称
     * @return 子系统名称 (e.g., "gateway", "gateway/rpc")
     */
    virtual std::string subsystem() const = 0;
    
    /**
     * 检查日志级别是否启用
     * 
     * 对齐 OpenClaw: isEnabled(level, target)
     * 
     * @param level 日志级别
     * @param target 目标 (any/console/file)
     * @return 是否启用
     */
    virtual bool is_enabled(
        LogLevel level,
        LogTarget target = LogTarget::Any
    ) const = 0;
    
    /**
     * 日志方法 (对齐 OpenClaw: trace/debug/info/warn/error/fatal)
     * 
     * @param message 日志消息
     * @param meta 元数据 (可选)
     */
    virtual void trace(
        const std::string& message,
        const std::map<std::string, std::string>& meta = {}
    ) = 0;
    
    virtual void debug(
        const std::string& message,
        const std::map<std::string, std::string>& meta = {}
    ) = 0;
    
    virtual void info(
        const std::string& message,
        const std::map<std::string, std::string>& meta = {}
    ) = 0;
    
    virtual void warn(
        const std::string& message,
        const std::map<std::string, std::string>& meta = {}
    ) = 0;
    
    virtual void error(
        const std::string& message,
        const std::map<std::string, std::string>& meta = {}
    ) = 0;
    
    virtual void fatal(
        const std::string& message,
        const std::map<std::string, std::string>& meta = {}
    ) = 0;
    
    /**
     * 原始输出 (绕过格式化)
     * 
     * 对齐 OpenClaw: raw(message)
     * 
     * @param message 原始消息
     */
    virtual void raw(const std::string& message) = 0;
    
    /**
     * 创建子系统日志器
     * 
     * 对齐 OpenClaw: child(name)
     * 
     * 示例:
     * ```cpp
     * auto gateway = create_subsystem_logger("gateway");
     * auto rpc = gateway->child("rpc");
     * rpc->info("RPC request received");  // [gateway/rpc] RPC request received
     * ```
     * 
     * @param name 子系统名称
     * @return 子日志器
     */
    virtual Ptr child(const std::string& name) = 0;
};

// ============================================================================
// Subsystem Logger Factory (对齐 OpenClaw)
// ============================================================================

/**
 * 创建子系统日志器
 * 
 * 对齐 OpenClaw: getSubsystemLogger(subsystem) / getChildLogger(parent, name)
 * 
 * 示例:
 * ```cpp
 * auto gateway = create_subsystem_logger("gateway");
 * gateway->info("Gateway started");
 * 
 * auto rpc = gateway->child("rpc");
 * rpc->debug("Handling RPC call");
 * ```
 * 
 * @param subsystem 子系统名称 (e.g., "gateway", "channels/discord")
 * @return SubsystemLogger 实例
 */
SubsystemLogger::Ptr create_subsystem_logger(const std::string& subsystem);

// ============================================================================
// Log Level Utilities (对齐 OpenClaw)
// ============================================================================

/**
 * LogLevel to string
 * @param level 日志级别
 * @return 字符串 (e.g., "info", "debug")
 */
std::string to_string(LogLevel level);

/**
 * String to LogLevel
 * @param str 字符串
 * @return 日志级别 (无效则返回 nullopt)
 */
std::optional<LogLevel> log_level_from_string(const std::string& str);

/**
 * LogLevel to numeric priority
 * 
 * 对齐 OpenClaw: levelToMinLevel
 * 
 * 优先级:
 * - fatal: 0 (最高)
 * - error: 1
 * - warn: 2
 * - info: 3
 * - debug: 4
 * - trace: 5 (最低)
 * - silent: INFINITY
 * 
 * @param level 日志级别
 * @return 优先级数值
 */
int level_to_min_level(LogLevel level);

/**
 * 规范化 LogLevel
 * 
 * 对齐 OpenClaw: normalizeLogLevel
 * 
 * @param level_str 日志级别字符串
 * @param fallback 默认值
 * @return 规范化的日志级别
 */
LogLevel normalize_log_level(const std::string& level_str, LogLevel fallback = LogLevel::Info);

} // namespace logging
} // namespace hunterclaw

#endif // HUNTERCLAW_LOGGING_SUBSYSTEM_LOGGER_H
