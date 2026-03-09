#ifndef HUNTERCLAW_SESSIONS_SESSION_MANAGER_H
#define HUNTERCLAW_SESSIONS_SESSION_MANAGER_H

#include <string>
#include <memory>
#include <optional>
#include <unordered_map>
#include "hunterclaw/core/result.h"
#include "hunterclaw/routing/router.h"

namespace hunterclaw {
namespace sessions {

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

// 会话条目 (对齐 OpenClaw SessionEntry)
struct SessionEntry {
    std::string session_id;        // 会话ID (UUID)
    std::string session_key;       // 会话key (唯一标识)
    std::string agent_id;          // Agent ID
    uint64_t created_at;           // 创建时间戳
    uint64_t updated_at;           // 更新时间戳
    
    // Token 统计
    int input_tokens = 0;
    int output_tokens = 0;
    int total_tokens = 0;
    int context_tokens = 0;
    
    // 元数据
    SessionMetadata metadata;
    
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
    
    // 会话重置
    bool daily_reset = true;
    int daily_reset_hour = 4;          // 本地时间 4:00 AM
    std::optional<int> idle_minutes;   // 空闲超时
    
    // 会话维护
    SessionMaintenanceMode maintenance_mode = SessionMaintenanceMode::Warn;
    int prune_after_days = 30;         // 清理旧会话 (天)
    int max_entries = 500;             // 最大会话数
};

// 会话管理器接口 (对齐 OpenClaw)
class SessionManager {
public:
    using Ptr = std::shared_ptr<SessionManager>;
    
    virtual ~SessionManager() = default;
    
    // 获取或创建会话
    virtual core::Result<SessionEntry> get_or_create_session(
        const std::string& session_key,
        const std::string& agent_id,
        const SessionMetadata& metadata
    ) = 0;
    
    // 更新会话
    virtual core::Result<bool> update_session(const SessionEntry& session) = 0;
    
    // 删除会话
    virtual core::Result<bool> delete_session(const std::string& session_key) = 0;
    
    // 列出所有会话
    virtual core::Result<std::vector<SessionEntry>> list_sessions(
        const std::optional<std::string>& agent_id = std::nullopt
    ) = 0;
    
    // 会话维护 (清理过期)
    virtual core::Result<int> prune_sessions() = 0;
    
    // 检查会话是否需要重置
    virtual bool should_reset(const SessionEntry& session) const = 0;
    
    // 工厂方法
    static Ptr create(const std::string& storage_dir, const SessionConfig& config);
};

// 文件存储实现
class FileSessionManager : public SessionManager {
public:
    FileSessionManager(const std::string& storage_dir, const SessionConfig& config);
    
    core::Result<SessionEntry> get_or_create_session(
        const std::string& session_key,
        const std::string& agent_id,
        const SessionMetadata& metadata
    ) override;
    
    core::Result<bool> update_session(const SessionEntry& session) override;
    
    core::Result<bool> delete_session(const std::string& session_key) override;
    
    core::Result<std::vector<SessionEntry>> list_sessions(
        const std::optional<std::string>& agent_id
    ) override;
    
    core::Result<int> prune_sessions() override;
    
    bool should_reset(const SessionEntry& session) const override;
    
private:
    std::string storage_dir_;
    SessionConfig config_;
    
    // 辅助方法
    std::string get_session_file_path(const std::string& session_key) const;
    core::Result<bool> write_session(const SessionEntry& session);
    core::Result<SessionEntry> read_session(const std::string& file_path);
    bool is_idle(const SessionEntry& session) const;
    bool is_past_daily_reset(const SessionEntry& session) const;
};

} // namespace sessions
} // namespace hunterclaw

#endif // HUNTERCLAW_SESSIONS_SESSION_MANAGER_H
