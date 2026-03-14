#pragma once

#include "hunterclaw/memory/provider.h"
#include "hunterclaw/sessions/session.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <memory>
#include <vector>

namespace hunterclaw {
namespace memory {

/**
 * 🎯 100% 对齐 OpenClaw: Session-Memory 集成
 * 
 * OpenClaw 的 Memory 系统与 Session 紧密集成:
 * 1. Session startup 自动召回相关记忆
 * 2. Session compaction 自动保存重要信息
 * 3. Session reset/delete 自动持久化对话
 * 4. Session-scoped memory 隔离 (多 session 不串话)
 * 
 * 对应 OpenClaw 的功能:
 * - `sessions.reset` → 保存对话到记忆
 * - `sessions.delete` → 归档 session 记忆
 * - `/new` 命令 → 召回之前的对话摘要
 * - Memory startup files (MEMORY.md) → 注入上下文
 * 
 * 使用示例:
 * 
 * // 1. Agent 启动时自动召回记忆
 * auto session = Session::create(session_id);
 * auto memory_mgr = SessionMemoryManager::create(session, memory_provider);
 * 
 * // 自动召回相关记忆并注入 System Prompt
 * auto startup_context = memory_mgr->get_startup_context();
 * // → 返回: <relevant_memories>...</relevant_memories>
 * 
 * // 2. 对话过程中自动存储
 * memory_mgr->on_user_message("记住我最喜欢的颜色是蓝色");
 * // → 自动提取并存储
 * 
 * memory_mgr->on_assistant_response("好的，我会记住的!");
 * // → 自动存储重要信息
 * 
 * // 3. Session 结束时自动持久化
 * memory_mgr->on_session_end();
 * // → 保存对话摘要到长期记忆
 * 
 * // 4. Session reset 时清理
 * memory_mgr->on_session_reset();
 * // → 归档当前对话，准备新会话
 */

// Session Memory Configuration
struct SessionMemoryConfig {
    // 🎯 OpenClaw: memory.enabled
    bool enabled = true;
    
    // 🎯 OpenClaw: memory.provider
    std::string provider_id = "default";
    
    // 🎯 OpenClaw: memory.config.contextWindow
    int context_window = 10;
    
    // 🎯 OpenClaw: memory.config.minScore
    double min_score = 0.7;
    
    // 🎯 OpenClaw: memory.autoCapture
    bool auto_capture = true;
    
    // 🎯 OpenClaw: memory.autoCaptureMaxChars
    int auto_capture_max_chars = 500;
    
    // 🎯 OpenClaw: memory.startupRecall
    bool startup_recall = true;
    
    // 🎯 OpenClaw: memory.compactionSave
    bool compaction_save = true;
    
    // 🎯 OpenClaw: memory.sessionScoped
    bool session_scoped = true;  // 是否隔离 session
};

// Session Memory Manager
class SessionMemoryManager {
public:
    /**
     * 创建 SessionMemoryManager
     * 
     * @param session 关联的 Session
     * @param provider Memory Provider
     * @param config 配置
     */
    static std::shared_ptr<SessionMemoryManager> create(
        std::shared_ptr<sessions::Session> session,
        std::shared_ptr<MemoryProvider> provider,
        const SessionMemoryConfig& config = SessionMemoryConfig()
    );
    
    virtual ~SessionMemoryManager() = default;
    
    // ========================================
    // 🎯 OpenClaw 核心功能
    // ========================================
    
    /**
     * 获取 Session Startup 上下文
     * 
     * 对齐 OpenClaw: Session startup 时自动召回相关记忆
     * 
     * @param query 可选的查询 (默认使用 session 历史摘要)
     * @return Startup 上下文字符串 (可注入 System Prompt)
     */
    virtual core::Result<std::string> get_startup_context(
        const std::string& query = ""
    ) = 0;
    
    /**
     * 处理用户消息 (自动捕获)
     * 
     * 对齐 OpenClaw: 自动提取并存储用户偏好、重要信息
     * 
     * @param message 用户消息
     */
    virtual core::Result<bool> on_user_message(const std::string& message) = 0;
    
    /**
     * 处理助手响应 (自动捕获)
     * 
     * 对齐 OpenClaw: 自动存储重要的助手回复
     * 
     * @param response 助手响应
     */
    virtual core::Result<bool> on_assistant_response(const std::string& response) = 0;
    
    /**
     * Session 结束回调
     * 
     * 对齐 OpenClaw: sessions.delete 时保存对话摘要
     */
    virtual core::Result<bool> on_session_end() = 0;
    
    /**
     * Session 重置回调
     * 
     * 对齐 OpenClaw: /new 或 sessions.reset 时归档
     */
    virtual core::Result<bool> on_session_reset() = 0;
    
    /**
     * Session Compaction 回调
     * 
     * 对齐 OpenClaw: compaction 时保存摘要到记忆
     * 
     * @param summary Compaction 生成的摘要
     */
    virtual core::Result<bool> on_session_compaction(const std::string& summary) = 0;
    
    // ========================================
    // 手动控制 API
    // ========================================
    
    /**
     * 手动添加记忆
     */
    virtual core::Result<std::string> add_memory(
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) = 0;
    
    /**
     * 搜索记忆 (Session-scoped)
     */
    virtual core::Result<std::vector<Memory>> search_memories(
        const std::string& query,
        const SearchOptions& options = SearchOptions()
    ) = 0;
    
    /**
     * 清空当前 Session 的记忆
     */
    virtual core::Result<bool> clear_session_memories() = 0;
    
    // ========================================
    // 统计信息
    // ========================================
    
    struct SessionMemoryStats {
        int64_t total_memories;          // 该 session 的记忆总数
        int64_t memories_added;          // 本次 session 新增
        int64_t memories_recalled;       // 本次 session 召回
        int64_t auto_captures;           // 自动捕获次数
        double avg_recall_time_ms;       // 平均召回时间
    };
    
    virtual SessionMemoryStats get_stats() const = 0;
    
protected:
    SessionMemoryManager() = default;
};

/**
 * 默认实现
 */
class DefaultSessionMemoryManager : public SessionMemoryManager {
public:
    DefaultSessionMemoryManager(
        std::shared_ptr<sessions::Session> session,
        std::shared_ptr<MemoryProvider> provider,
        const SessionMemoryConfig& config
    );
    
    core::Result<std::string> get_startup_context(
        const std::string& query = ""
    ) override;
    
    core::Result<bool> on_user_message(const std::string& message) override;
    core::Result<bool> on_assistant_response(const std::string& response) override;
    core::Result<bool> on_session_end() override;
    core::Result<bool> on_session_reset() override;
    core::Result<bool> on_session_compaction(const std::string& summary) override;
    
    core::Result<std::string> add_memory(
        const std::string& content,
        const std::map<std::string, std::string>& metadata
    ) override;
    
    core::Result<std::vector<Memory>> search_memories(
        const std::string& query,
        const SearchOptions& options
    ) override;
    
    core::Result<bool> clear_session_memories() override;
    
    SessionMemoryStats get_stats() const override;

private:
    std::shared_ptr<sessions::Session> session_;
    std::shared_ptr<MemoryProvider> provider_;
    SessionMemoryConfig config_;
    
    // 统计信息
    mutable SessionMemoryStats stats_;
    
    // 辅助方法
    bool should_capture(const std::string& text) const;
    std::string extract_important_info(const std::string& text) const;
    std::string build_session_query() const;
    std::string format_memories_context(const std::vector<Memory>& memories) const;
};

// ========================================
// 🎯 对齐 OpenClaw: Session Memory 钩子
// ========================================

/**
 * Session Memory Hooks (对齐 OpenClaw session-memory 钩子)
 * 
 * OpenClaw 在 Session 生命周期的关键点自动调用 Memory 相关钩子:
 * - before_session_start → 召回记忆
 * - after_user_message → 自动捕获
 * - before_compaction → 保存摘要
 * - before_reset → 归档对话
 * - after_session_end → 持久化
 * 
 * HunterClaw 通过 SessionMemoryHooks 实现相同功能。
 */
class SessionMemoryHooks {
public:
    /**
     * 注册 Memory Hooks 到 Session Manager
     * 
     * 自动将 Memory 功能集成到 Session 生命周期。
     * 
     * @param session_manager Session Manager
     * @param memory_provider Memory Provider
     * @param config Memory 配置
     */
    static core::Result<bool> register_hooks(
        sessions::SessionManager& session_manager,
        std::shared_ptr<MemoryProvider> memory_provider,
        const SessionMemoryConfig& config = SessionMemoryConfig()
    );
    
    /**
     * 注销 Memory Hooks
     */
    static void unregister_hooks(sessions::SessionManager& session_manager);
};

} // namespace memory
} // namespace hunterclaw
