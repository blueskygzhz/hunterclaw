#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/json.h"
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace hunterclaw {
namespace agents {

/**
 * @brief ACP (Agent Communication Protocol) 元数据
 * 
 * 对齐 OpenClaw:
 * - src/agents/acp-spawn.ts
 * - src/types/gateway-protocol.ts
 * 
 * ACP 协议定义了 Agent 的运行时行为:
 * - Label 映射 (UI 显示名称)
 * - Scope 隔离 (workspace/memory/tools)
 * - Lifecycle Hooks
 * - Resource Limits
 */

/**
 * @brief ACP 标签映射
 * 
 * 用于 UI 显示的标签映射 (Key → Label)
 */
struct AcpLabelMapping {
    std::map<std::string, std::string> labels;  // 标签映射表
    
    /**
     * @brief 获取标签
     * @param key 键名
     * @return 标签 (如果没有映射则返回 key 本身)
     */
    std::string get_label(const std::string& key) const {
        auto it = labels.find(key);
        return it != labels.end() ? it->second : key;
    }
    
    /**
     * @brief 添加标签映射
     */
    void add_label(const std::string& key, const std::string& label) {
        labels[key] = label;
    }
};

/**
 * @brief ACP 作用域配置
 * 
 * 定义 Agent 实例的隔离范围
 */
struct AcpScope {
    bool isolated_workspace = true;    // 是否独立 workspace
    bool isolated_memory = true;        // 是否独立 memory
    bool isolated_tools = false;        // 是否独立 tools (通常共享)
    
    std::optional<std::string> workspace_override;  // 自定义 workspace 路径
    std::optional<std::string> memory_override;     // 自定义 memory 配置
};

/**
 * @brief ACP 生命周期 Hook
 */
struct AcpLifecycleHooks {
    std::optional<std::string> on_start;      // 启动时执行的脚本
    std::optional<std::string> on_message;    // 收到消息时执行的脚本
    std::optional<std::string> on_end;        // 结束时执行的脚本
};

/**
 * @brief ACP 资源限制
 */
struct AcpResourceLimits {
    std::optional<size_t> max_memory_mb;     // 最大内存 (MB)
    std::optional<size_t> max_disk_mb;       // 最大磁盘 (MB)
    std::optional<int> max_concurrent_runs;  // 最大并发运行数
    std::optional<int> max_turns_per_run;    // 每次运行最大轮数
    std::optional<int> timeout_seconds;      // 超时时间 (秒)
};

/**
 * @brief ACP 运行时选项
 * 
 * 传递给 Agent 实例的运行时参数
 */
struct AcpRuntimeOptions {
    std::optional<std::string> model;                // 使用的模型 (覆盖配置)
    std::optional<int> max_turns;                    // 最大轮数
    std::optional<int> temperature;                  // 温度参数
    std::map<std::string, std::string> env_vars;     // 环境变量
    std::vector<std::string> enabled_tools;          // 启用的工具 (覆盖配置)
    std::map<std::string, core::JsonValue> context;  // 上下文数据
};

/**
 * @brief ACP 元数据 (完整)
 * 
 * 所有 ACP 协议相关的元数据
 */
struct SessionAcpMeta {
    // 标签映射
    AcpLabelMapping labels;
    
    // 作用域配置
    AcpScope scope;
    
    // 生命周期 Hook
    AcpLifecycleHooks lifecycle;
    
    // 资源限制
    AcpResourceLimits limits;
    
    // 运行时选项
    AcpRuntimeOptions runtime;
    
    // 额外元数据
    std::map<std::string, core::JsonValue> metadata;
    
    /**
     * @brief 从 JSON 解析
     */
    static core::Result<SessionAcpMeta> from_json(const core::JsonValue& json);
    
    /**
     * @brief 转换为 JSON
     */
    core::JsonValue to_json() const;
    
    /**
     * @brief 合并元数据 (other 覆盖 this)
     */
    void merge(const SessionAcpMeta& other);
};

/**
 * @brief ACP 消息封装
 * 
 * 用于 Agent 之间通信的消息格式
 */
struct AcpMessage {
    std::string message_id;                         // 消息 ID
    std::string from_agent_id;                      // 发送方 Agent ID
    std::string to_agent_id;                        // 接收方 Agent ID
    std::string content;                            // 消息内容
    std::map<std::string, core::JsonValue> meta;    // 元数据
    int64_t timestamp_ms;                           // 时间戳 (毫秒)
    
    /**
     * @brief 从 JSON 解析
     */
    static core::Result<AcpMessage> from_json(const core::JsonValue& json);
    
    /**
     * @brief 转换为 JSON
     */
    core::JsonValue to_json() const;
};

/**
 * @brief ACP 协议工具类
 */
class AcpProtocol {
public:
    /**
     * @brief 创建默认 ACP 元数据
     */
    static SessionAcpMeta create_default_meta();
    
    /**
     * @brief 验证 ACP 元数据
     */
    static core::Result<void> validate_meta(const SessionAcpMeta& meta);
    
    /**
     * @brief 创建 ACP 消息
     */
    static AcpMessage create_message(
        const std::string& from_agent,
        const std::string& to_agent,
        const std::string& content,
        const std::map<std::string, core::JsonValue>& meta = {}
    );
    
    /**
     * @brief 解析 Label → Key 映射
     * 
     * @param labels 标签映射
     * @param label 标签名
     * @return 对应的 Key (如果没有则返回 label 本身)
     */
    static std::string resolve_key_from_label(
        const AcpLabelMapping& labels,
        const std::string& label
    );
};

} // namespace agents
} // namespace hunterclaw
