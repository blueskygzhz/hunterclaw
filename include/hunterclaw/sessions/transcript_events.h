#ifndef HUNTERCLAW_SESSIONS_TRANSCRIPT_EVENTS_H
#define HUNTERCLAW_SESSIONS_TRANSCRIPT_EVENTS_H

#include <string>
#include <functional>
#include <set>
#include <mutex>
#include <memory>

namespace hunterclaw {
namespace sessions {

/**
 * Session Transcript Events System
 * 100% 对齐 OpenClaw sessions/transcript-events.ts
 * 
 * 功能:
 * 1. 会话转录更新事件
 * 2. 监听器注册与注销
 * 3. 线程安全的事件分发
 * 4. 自动清理失败监听器
 */

// ============================================================================
// Types (100% 对齐 OpenClaw)
// ============================================================================

/// 会话转录更新事件 (对齐 SessionTranscriptUpdate)
struct SessionTranscriptUpdate {
    std::string session_file;  ///< 会话文件路径
    
    SessionTranscriptUpdate() = default;
    explicit SessionTranscriptUpdate(std::string file)
        : session_file(std::move(file)) {}
};

/// 会话转录监听器类型 (对齐 SessionTranscriptListener)
using SessionTranscriptListener = std::function<void(const SessionTranscriptUpdate&)>;

// ============================================================================
// Session Transcript Event Emitter (100% 对齐 OpenClaw)
// ============================================================================

/**
 * Session Transcript Event Emitter
 * 
 * 核心功能:
 * 1. ✅ onSessionTranscriptUpdate(listener) - 注册监听器,返回注销函数
 * 2. ✅ emitSessionTranscriptUpdate(sessionFile) - 发出转录更新事件
 * 3. ✅ Thread-Safe - 线程安全的监听器管理
 * 4. ✅ Auto Cleanup - 自动清理失败的监听器
 * 
 * 对齐 OpenClaw:
 * - SESSION_TRANSCRIPT_LISTENERS (全局监听器集合)
 * - onSessionTranscriptUpdate (注册)
 * - emitSessionTranscriptUpdate (发射)
 */
class SessionTranscriptEventEmitter {
public:
    /// 获取全局单例
    static SessionTranscriptEventEmitter& instance();
    
    /**
     * 注册会话转录监听器
     * 
     * 对齐 OpenClaw: onSessionTranscriptUpdate
     * 
     * @param listener 监听器函数
     * @return 注销函数 (调用以移除监听器)
     * 
     * 示例:
     * ```cpp
     * auto unsubscribe = SessionTranscriptEventEmitter::instance()
     *     .on_session_transcript_update([](const SessionTranscriptUpdate& update) {
     *         std::cout << "Session updated: " << update.session_file << std::endl;
     *     });
     * 
     * // ... later ...
     * unsubscribe();  // 移除监听器
     * ```
     */
    std::function<void()> on_session_transcript_update(SessionTranscriptListener listener);
    
    /**
     * 发出会话转录更新事件
     * 
     * 对齐 OpenClaw: emitSessionTranscriptUpdate
     * 
     * 行为:
     * 1. ✅ Trim session_file (去除空白)
     * 2. ✅ Skip empty (跳过空字符串)
     * 3. ✅ Notify all listeners (通知所有监听器)
     * 4. ✅ Ignore listener errors (忽略监听器异常)
     * 
     * @param session_file 会话文件路径
     * 
     * 示例:
     * ```cpp
     * SessionTranscriptEventEmitter::instance()
     *     .emit_session_transcript_update("/path/to/session-123.json");
     * ```
     */
    void emit_session_transcript_update(const std::string& session_file);
    
    /**
     * 获取当前监听器数量 (调试用)
     * @return 监听器数量
     */
    size_t listener_count() const;
    
    /**
     * 清空所有监听器 (调试/测试用)
     */
    void clear_all_listeners();

private:
    SessionTranscriptEventEmitter() = default;
    ~SessionTranscriptEventEmitter() = default;
    
    // 禁止拷贝/移动
    SessionTranscriptEventEmitter(const SessionTranscriptEventEmitter&) = delete;
    SessionTranscriptEventEmitter& operator=(const SessionTranscriptEventEmitter&) = delete;
    SessionTranscriptEventEmitter(SessionTranscriptEventEmitter&&) = delete;
    SessionTranscriptEventEmitter& operator=(SessionTranscriptEventEmitter&&) = delete;
    
    /// 监听器 wrapper (支持注销)
    struct ListenerEntry {
        SessionTranscriptListener listener;
        size_t id;  ///< 唯一标识
    };
    
    mutable std::mutex mutex_;
    std::set<std::shared_ptr<ListenerEntry>> listeners_;
    size_t next_listener_id_ = 0;
};

// ============================================================================
// Convenience Functions (全局函数接口)
// ============================================================================

/**
 * 注册会话转录监听器 (全局函数)
 * 
 * 对齐 OpenClaw: onSessionTranscriptUpdate
 * @see SessionTranscriptEventEmitter::on_session_transcript_update
 */
inline std::function<void()> on_session_transcript_update(SessionTranscriptListener listener) {
    return SessionTranscriptEventEmitter::instance().on_session_transcript_update(std::move(listener));
}

/**
 * 发出会话转录更新事件 (全局函数)
 * 
 * 对齐 OpenClaw: emitSessionTranscriptUpdate
 * @see SessionTranscriptEventEmitter::emit_session_transcript_update
 */
inline void emit_session_transcript_update(const std::string& session_file) {
    SessionTranscriptEventEmitter::instance().emit_session_transcript_update(session_file);
}

} // namespace sessions
} // namespace hunterclaw

#endif // HUNTERCLAW_SESSIONS_TRANSCRIPT_EVENTS_H
