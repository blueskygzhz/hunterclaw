#ifndef HUNTERCLAW_SESSIONS_SESSION_MANAGER_H
#define HUNTERCLAW_SESSIONS_SESSION_MANAGER_H

#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>
#include "hunterclaw/core/result.h"
#include "hunterclaw/routing/router.h"
#include "hunterclaw/agents/types.h"  // ✅ 包含完整定义

namespace hunterclaw {
namespace sessions {

// ============================================================================
// ACP 集成数据结构 (对齐 OpenClaw ACP 协议)
// ============================================================================

// ACP 身份状态
enum class AcpIdentityState {
    Pending,   // 等待解析
    Resolved   // 已解析
};

// ACP 会话身份 (对齐 OpenClaw SessionAcpIdentity)
struct SessionAcpIdentity {
    AcpIdentityState state = AcpIdentityState::Pending;
    std::optional<std::string> acp_record_id;     // ACP 记录ID
    std::optional<std::string> acp_session_id;    // ACP 会话ID
    std::optional<std::string> agent_session_id;  // Agent 会话ID
    std::string source;                            // 来源: ensure/status/event
    uint64_t last_updated_at = 0;                  // 最后更新时间
};

// ACP 运行时选项 (对齐 OpenClaw AcpSessionRuntimeOptions)
struct AcpSessionRuntimeOptions {
    std::string mode;         // persistent/oneshot
    std::string model;        // 模型名称
    std::string cwd;          // 工作目录
    std::optional<int> max_turns;  // 最大轮次
};

// ACP 会话元数据 (对齐 OpenClaw SessionAcpMeta)
struct SessionAcpMeta {
    std::string backend;                           // ACP 后端标识
    std::string agent;                             // Agent 标识
    std::string runtime_session_name;              // 运行时会话名
    std::optional<SessionAcpIdentity> identity;    // ACP 身份
    AcpSessionRuntimeOptions runtime_options;      // 运行时选项
    std::string state;                             // idle/running/error
    uint64_t last_activity_at = 0;                 // 最后活动时间
};

// ============================================================================
// 会话数据结构
// ============================================================================

// 会话元数据 (对齐 OpenClaw SessionMetadata)
struct SessionMetadata {
    std::optional<std::string> provider;           // 来源平台
    std::optional<std::string> channel;            // 频道
    std::optional<std::string> account_id;         // 账号ID
    std::optional<std::string> conversation_label; // 会话标签
    std::optional<std::string> group_subject;      // 群组主题
    std::optional<std::string> sender_name;        // 发送者名称
    std::optional<std::string> sender_username;    // 发送者用户名
};

// 会话条目 (对齐 OpenClaw SessionEntry - 完整版)
struct SessionEntry {
    std::string session_id;        // 会话ID (UUID)
    std::string session_key;       // 会话key (唯一标识)
    std::string agent_id;          // Agent ID
    uint64_t created_at = 0;       // 创建时间戳
    uint64_t updated_at = 0;       // 更新时间戳

    // Token 统计 (对齐 OpenClaw)
    int input_tokens = 0;
    int output_tokens = 0;
    int total_tokens = 0;
    bool total_tokens_fresh = false;  // ✅ 对齐 OpenClaw: totalTokensFresh

    // 运行状态 (对齐 OpenClaw)
    bool system_sent = false;         // ✅ 对齐 OpenClaw: systemSent
    bool aborted_last_run = false;    // ✅ 对齐 OpenClaw: abortedLastRun

    // 基础元数据
    SessionMetadata metadata;

    // ACP 集成 (可选 - 支持 ACP 协议)
    std::optional<SessionAcpMeta> acp;

    // 会话标识 (对齐 OpenClaw)
    std::optional<std::string> label;          // 会话标签 (用户友好名称)
    std::optional<std::string> display_name;   // 显示名称
    std::optional<std::string> chat_type;      // 聊天类型: dm/group/channel
    std::optional<std::string> subject;        // ✅ 对齐 OpenClaw: subject
    std::optional<std::string> channel;        // ✅ 对齐 OpenClaw: channel
    std::optional<std::string> group_channel;  // ✅ 对齐 OpenClaw: groupChannel
    std::optional<std::string> space;          // ✅ 对齐 OpenClaw: space

    // 模型配置 (对齐 OpenClaw)
    std::optional<std::string> model;           // ✅ 对齐 OpenClaw: model (runtime)
    std::optional<std::string> model_provider;  // ✅ 对齐 OpenClaw: modelProvider
    std::optional<int> context_tokens;          // ✅ 对齐 OpenClaw: contextTokens
    std::optional<std::string> provider_override; // ✅ 对齐 OpenClaw: providerOverride
    std::optional<std::string> model_override;    // ✅ 对齐 OpenClaw: modelOverride
    std::optional<std::string> fallback_notice_selected_model; // ✅ 对齐 OpenClaw: fallbackNoticeSelectedModel

    // 行为配置 (对齐 OpenClaw)
    std::optional<std::string> thinking_level;  // ✅ "low"|"medium"|"high"|"xhigh"
    std::optional<bool>        fast_mode;        // ✅ 对齐 OpenClaw: fastMode
    std::optional<std::string> verbose_level;   // ✅ 对齐 OpenClaw: verboseLevel
    std::optional<std::string> reasoning_level; // ✅ "on"|"off"|"stream"
    std::optional<std::string> elevated_level;  // ✅ "on"|"off"|"ask"|"full"
    std::optional<std::string> send_policy;     // ✅ "allow"|"deny"
    std::optional<std::string> response_usage;  // ✅ "tokens"|"full"

    // 派生/路由字段 (对齐 OpenClaw)
    std::optional<std::string> spawned_by;      // ✅ 对齐 OpenClaw: spawnedBy
    std::optional<std::string> last_channel;    // ✅ 对齐 OpenClaw: lastChannel
    std::optional<std::string> last_to;         // ✅ 对齐 OpenClaw: lastTo
    std::optional<std::string> last_account_id; // ✅ 对齐 OpenClaw: lastAccountId
    std::optional<std::string> last_thread_id;  // ✅ 对齐 OpenClaw: lastThreadId

    // TTS 配置
    std::optional<std::string> tts_auto;  // "off"|"always"|"inbound"|"tagged"

    // 会话历史 (简化版，实际可能需要独立存储)
    std::vector<std::string> messages;
};

// 会话维护模式 (对齐 OpenClaw)
enum class SessionMaintenanceMode {
    Warn,    // 警告但允许
    Enforce  // 强制执行限制
};

// 会话配置 (对齐 OpenClaw session config)
struct SessionConfig {
    routing::DmScope dm_scope = routing::DmScope::PerChannelPeer;  // 默认安全模式
    
    // 会话重置 (✅ 对齐 OpenClaw: 默认禁用自动重置)
    bool daily_reset = false;          // ✅ 默认禁用每日重置
    int daily_reset_hour = 4;          // 本地时间 4:00 AM (如果启用)
    std::optional<int> idle_minutes;   // ✅ 默认不设置空闲超时 (nullopt = 禁用)
    
    // 会话维护
    SessionMaintenanceMode maintenance_mode = SessionMaintenanceMode::Warn;
    int prune_after_days = 365;        // ✅ 对齐 OpenClaw: 1年后清理 (默认30→365)
    int max_entries = 10000;           // ✅ 对齐 OpenClaw: 增大限制 (默认500→10000)
    
    // Per-Agent Storage (对齐 OpenClaw)
    bool per_agent_storage = true;     // 是否按 Agent 分离存储
    
    // Device-based Sessions (对齐 OpenClaw)
    bool device_based_sessions = true; // ✅ 启用基于 Device ID 的会话
};

// ============================================================================
// Session Mapper (对齐 OpenClaw SessionMapper)
// ============================================================================

// Session 标签/Key 映射器
class SessionMapper {
public:
    // 解析 label → session_key (对齐 OpenClaw sessions.resolve)
    static core::Result<std::string> resolve_session_key(
        const std::string& label_or_key,
        bool require_existing = false
    );
    
    // 从 session_key 提取 label
    static std::optional<std::string> extract_label(const std::string& session_key);
};

// 会话管理器接口 (对齐 OpenClaw)
class SessionManager {
public:
    using Ptr = std::shared_ptr<SessionManager>;
    
    virtual ~SessionManager() = default;
    
    // 获取或创建会话 (支持 label 解析)
    virtual core::Result<SessionEntry> get_or_create_session(
        const std::string& label_or_key,  // 支持 label 或 key
        const std::string& agent_id,
        const SessionMetadata& metadata
    ) = 0;
    
    // 更新会话
    virtual core::Result<bool> update_session(const SessionEntry& session) = 0;
    
    // 删除会话 (支持 label 解析)
    virtual core::Result<bool> delete_session(const std::string& label_or_key) = 0;
    
    // 通过 label 或 key 获取会话
    virtual core::Result<SessionEntry> get_session(const std::string& label_or_key) = 0;
    
    // 列出所有会话
    virtual core::Result<std::vector<SessionEntry>> list_sessions(
        const std::optional<std::string>& agent_id = std::nullopt
    ) = 0;
    
    // 会话维护 (清理过期)
    virtual core::Result<int> prune_sessions() = 0;
    
    // 检查会话是否需要重置
    virtual bool should_reset(const SessionEntry& session) const = 0;
    
    // ✅ 获取 Agent 配置 (对齐 message_processor.cc 需求)
    // 注意: 这是临时 API，实际应该从独立的 AgentRegistry 获取
    virtual core::Result<hunterclaw::agents::AgentConfig> get_agent_config(
        const std::string& agent_id
    ) = 0;
    
    // 工厂方法
    static Ptr create(const std::string& storage_dir, const SessionConfig& config);
};

// 文件存储实现
class FileSessionManager : public SessionManager {
public:
    FileSessionManager(const std::string& storage_dir, const SessionConfig& config);
    
    core::Result<SessionEntry> get_or_create_session(
        const std::string& label_or_key,
        const std::string& agent_id,
        const SessionMetadata& metadata
    ) override;
    
    core::Result<bool> update_session(const SessionEntry& session) override;
    
    core::Result<bool> delete_session(const std::string& label_or_key) override;
    
    core::Result<SessionEntry> get_session(const std::string& label_or_key) override;
    
    core::Result<std::vector<SessionEntry>> list_sessions(
        const std::optional<std::string>& agent_id
    ) override;
    
    core::Result<int> prune_sessions() override;
    
    bool should_reset(const SessionEntry& session) const override;
    
    // ✅ 获取 Agent 配置
    core::Result<hunterclaw::agents::AgentConfig> get_agent_config(
        const std::string& agent_id
    ) override;
    
private:
    std::string storage_dir_;
    SessionConfig config_;
    
    // 辅助方法
    std::string resolve_storage_path(const std::string& agent_id) const;
    std::string get_session_file_path(
        const std::string& session_key,
        const std::string& agent_id
    ) const;
    core::Result<bool> write_session(const SessionEntry& session);
    core::Result<SessionEntry> read_session(const std::string& file_path);
    bool is_idle(const SessionEntry& session) const;
    bool is_past_daily_reset(const SessionEntry& session) const;
};

} // namespace sessions
} // namespace hunterclaw

#endif // HUNTERCLAW_SESSIONS_SESSION_MANAGER_H
