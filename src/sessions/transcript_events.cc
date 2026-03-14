#include "hunterclaw/sessions/transcript_events.h"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace sessions {

namespace {
    // Helper to get logger - returns global default logger
    inline std::shared_ptr<spdlog::logger> log() {
        static auto logger = []() {
            auto l = spdlog::get("hunterclaw");
            if (!l) {
                // Create a simple stdout logger if none exists
                try {
                    l = spdlog::stdout_logger_mt("hunterclaw");
                } catch (...) {
                    // If it already exists, get it
                    l = spdlog::get("hunterclaw");
                }
            }
            return l;
        }();
        return logger;
    }
    
    /// Trim string (对齐 OpenClaw trim())
    std::string trim(const std::string& str) {
        if (str.empty()) {
            return str;
        }
        
        auto start = str.begin();
        while (start != str.end() && std::isspace(static_cast<unsigned char>(*start))) {
            ++start;
        }
        
        auto end = str.end();
        do {
            --end;
        } while (end > start && std::isspace(static_cast<unsigned char>(*end)));
        
        return std::string(start, end + 1);
    }
}

// ============================================================================
// SessionTranscriptEventEmitter Implementation
// ============================================================================

SessionTranscriptEventEmitter& SessionTranscriptEventEmitter::instance() {
    static SessionTranscriptEventEmitter instance;
    return instance;
}

std::function<void()> SessionTranscriptEventEmitter::on_session_transcript_update(
    SessionTranscriptListener listener
) {
    if (!listener) {
        log()->warn("[TranscriptEvents] Attempted to register null listener");
        return []() {};  // Return no-op unsubscribe
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 创建 listener entry
    auto entry = std::make_shared<ListenerEntry>();
    entry->listener = std::move(listener);
    entry->id = next_listener_id_++;
    
    // 添加到集合
    listeners_.insert(entry);
    
    log()->debug("[TranscriptEvents] Registered listener (id: {}, total: {})", 
                entry->id, listeners_.size());
    
    // 返回注销函数 (对齐 OpenClaw)
    // Capture weak_ptr to avoid circular reference
    std::weak_ptr<ListenerEntry> weak_entry = entry;
    size_t entry_id = entry->id;
    
    return [this, weak_entry, entry_id]() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Find and remove by id
        auto it = std::find_if(listeners_.begin(), listeners_.end(),
            [entry_id](const std::shared_ptr<ListenerEntry>& e) {
                return e->id == entry_id;
            });
        
        if (it != listeners_.end()) {
            listeners_.erase(it);
            log()->debug("[TranscriptEvents] Unregistered listener (id: {}, remaining: {})", 
                        entry_id, listeners_.size());
        }
    };
}

void SessionTranscriptEventEmitter::emit_session_transcript_update(
    const std::string& session_file
) {
    // 1. Trim session_file (对齐 OpenClaw)
    std::string trimmed = trim(session_file);
    
    // 2. Skip empty (对齐 OpenClaw early return)
    if (trimmed.empty()) {
        return;
    }
    
    log()->trace("[TranscriptEvents] Emitting update: {}", trimmed);
    
    // 3. 创建更新事件
    SessionTranscriptUpdate update(trimmed);
    
    // 4. Notify all listeners (线程安全)
    std::vector<std::shared_ptr<ListenerEntry>> current_listeners;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_listeners.reserve(listeners_.size());
        for (const auto& entry : listeners_) {
            current_listeners.push_back(entry);
        }
    }
    
    // 5. 调用所有监听器 (对齐 OpenClaw: try-catch ignore errors)
    for (const auto& entry : current_listeners) {
        try {
            entry->listener(update);
        } catch (const std::exception& e) {
            // Ignore listener errors (对齐 OpenClaw: catch { /* ignore */ })
            log()->warn("[TranscriptEvents] Listener error (id: {}): {}", 
                       entry->id, e.what());
        } catch (...) {
            log()->warn("[TranscriptEvents] Listener unknown error (id: {})", entry->id);
        }
    }
    
    log()->trace("[TranscriptEvents] Notified {} listeners", current_listeners.size());
}

size_t SessionTranscriptEventEmitter::listener_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return listeners_.size();
}

void SessionTranscriptEventEmitter::clear_all_listeners() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = listeners_.size();
    listeners_.clear();
    log()->info("[TranscriptEvents] Cleared all {} listeners", count);
}

} // namespace sessions
} // namespace hunterclaw
