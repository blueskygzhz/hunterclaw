#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <optional>

namespace hunterclaw {
namespace auto_reply {

// ═══════════════════════════════════════════════════════════════
// ✅ 严格对齐 OpenClaw: src/config/types.agent-defaults.ts
// 心跳配置 (Heartbeat Config)
// ═══════════════════════════════════════════════════════════════

struct HeartbeatConfig {
    // 心跳提示词 (默认: "Check for any important alerts...")
    std::optional<std::string> prompt;
    
    // 使用的模型 (可覆盖 agent 默认模型)
    std::optional<std::string> model;
    
    // 最大确认字符数 (默认: 300)
    int ack_max_chars = 300;
    
    // 直接消息策略: "allow" | "block" (默认: "allow")
    enum class DirectPolicy {
        Allow,
        Block
    };
    DirectPolicy direct_policy = DirectPolicy::Allow;
    
    // 可见性配置
    bool show_alerts = true;   // 显示警报 (默认: true)
    bool show_ok = false;      // 显示 OK 消息 (默认: false)
    bool use_indicator = true; // 使用指示器而非发送消息 (默认: true)
    
    // 心跳间隔 (默认: 30 分钟)
    std::chrono::seconds interval{1800};  // 30m
};

// ═══════════════════════════════════════════════════════════════
// ✅ 严格对齐 OpenClaw: src/web/auto-reply/types.ts
// 渠道配置 (Channel Config)
// ═══════════════════════════════════════════════════════════════

struct ChannelConfig {
    // ACK 反应 (例如: "👀", "✓")
    std::string ack_reaction;
    
    // 消息前缀
    std::string message_prefix;
    
    // 白名单 (allowFrom)
    std::vector<std::string> allow_from;
    
    // 黑名单 (denyFrom)
    std::vector<std::string> deny_from;
    
    // 群组白名单
    std::vector<std::string> group_allow_from;
    
    // 群组策略: "allow" | "block" | "mention-only"
    enum class GroupPolicy {
        Allow,
        Block,
        MentionOnly
    };
    GroupPolicy group_policy = GroupPolicy::MentionOnly;
    
    // 文本分块限制 (字节)
    int text_chunk_limit = 2000;
    
    // 分块模式: "auto" | "disabled"
    enum class ChunkMode {
        Auto,
        Disabled
    };
    ChunkMode chunk_mode = ChunkMode::Auto;
    
    // 媒体文件大小限制 (MB)
    int media_max_mb = 16;
    
    // 是否禁用流式分块
    bool block_streaming = false;
    
    // 是否发送已读回执
    bool send_read_receipts = true;
    
    // 群组历史记录限制
    int history_limit = 10;
};

// ═══════════════════════════════════════════════════════════════
// ✅ 严格对齐 OpenClaw: src/auto-reply/types.ts
// 消息分发配置 (Dispatch Config)
// ═══════════════════════════════════════════════════════════════

struct DispatchConfig {
    // Echo 检测窗口 (默认: 5 分钟)
    std::chrono::seconds echo_window{300};
    
    // 是否启用去重
    bool enable_deduplication = true;
    
    // 去重缓存大小
    int dedup_cache_size = 1000;
    
    // 是否启用命令授权检查
    bool enable_command_auth = true;
    
    // 是否启用 TTS (文本转语音)
    bool enable_tts = false;
    
    // TTS 提供商: "openai" | "elevenlabs" | "azure"
    std::string tts_provider = "openai";
    
    // 是否启用 ACP (Agent Context Protocol)
    bool enable_acp = true;
    
    // 重试策略
    struct RetryPolicy {
        int max_attempts = 3;
        std::chrono::milliseconds initial_delay{1000};
        std::chrono::milliseconds max_delay{10000};
        double backoff_multiplier = 2.0;
    };
    RetryPolicy retry_policy;
};

// ═══════════════════════════════════════════════════════════════
// ✅ 严格对齐 OpenClaw: src/web/auto-reply/monitor.ts
// 监控配置 (Monitor Config)
// ═══════════════════════════════════════════════════════════════

struct MonitorConfig {
    // 消息超时 (默认: 30 分钟)
    std::chrono::milliseconds message_timeout{1800000};  // 30 min
    
    // 看门狗检查间隔 (默认: 1 分钟)
    std::chrono::milliseconds watchdog_check_interval{60000};  // 1 min
    
    // 重连策略
    struct ReconnectPolicy {
        int max_attempts = 5;
        std::chrono::milliseconds initial_delay{5000};
        std::chrono::milliseconds max_delay{300000};  // 5 min
        double backoff_multiplier = 2.0;
    };
    ReconnectPolicy reconnect_policy;
    
    // 是否启用自动重连
    bool enable_auto_reconnect = true;
    
    // 是否记录详细日志
    bool verbose_logging = false;
};

// ═══════════════════════════════════════════════════════════════
// ✅ 完整的 AutoReply 配置
// ═══════════════════════════════════════════════════════════════

struct AutoReplyConfig {
    // 心跳配置
    HeartbeatConfig heartbeat;
    
    // 渠道配置
    ChannelConfig channel;
    
    // 分发配置
    DispatchConfig dispatch;
    
    // 监控配置
    MonitorConfig monitor;
    
    // 是否启用 auto_reply
    bool enabled = true;
    
    // Agent ID
    std::string agent_id;
    
    // 工作区路径
    std::string workspace_path;
};

} // namespace auto_reply
} // namespace hunterclaw
