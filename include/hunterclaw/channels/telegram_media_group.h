#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/core/json.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <chrono>
#include <thread>
#include <mutex>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Telegram Media Group (Album) Handler
// ============================================================================

struct TelegramMediaItem {
    enum Type {
        Photo,
        Video,
        Document,
        Audio
    };
    
    Type type;
    std::string file_id;
    std::string file_unique_id;
    std::string caption;
    int64_t file_size = 0;
    
    // Photo specific
    std::vector<std::pair<int, int>> photo_sizes;  // (width, height)
    
    // Video specific
    int duration_seconds = 0;
    int width = 0;
    int height = 0;
    
    // Document specific
    std::string mime_type;
    std::string file_name;
};

struct TelegramMessage {
    int64_t message_id;
    int64_t chat_id;
    int64_t from_user_id;
    std::string media_group_id;
    TelegramMediaItem media;
    int64_t timestamp;
};

// Media group entry (buffer)
struct MediaGroupEntry {
    std::vector<TelegramMessage> messages;
    std::chrono::steady_clock::time_point first_arrival;
    std::unique_ptr<std::thread> timer_thread;
    bool processing = false;
};

// Media group handler
class TelegramMediaGroupHandler {
public:
    struct Config {
        int timeout_ms = 500;  // 500ms window to collect all items
        int max_group_size = 10;  // Telegram allows up to 10 items
        bool skip_oversized_items = true;  // Skip items exceeding size limit
        size_t max_item_size = 20 * 1024 * 1024;  // 20MB
    };
    
    using ProcessGroupCallback = std::function<void(const std::vector<TelegramMessage>&)>;
    
    TelegramMediaGroupHandler(const Config& config, ProcessGroupCallback on_group_complete);
    ~TelegramMediaGroupHandler();
    
    // Handle incoming message (may be part of media group)
    void handle_message(const TelegramMessage& message);
    
    // Flush all pending groups (on shutdown)
    void flush_all();
    
    // Get statistics
    struct Stats {
        int total_groups_processed = 0;
        int total_items_processed = 0;
        int groups_in_buffer = 0;
    };
    Stats get_stats() const;
    
private:
    Config config_;
    ProcessGroupCallback on_group_complete_;
    
    std::map<std::string, std::shared_ptr<MediaGroupEntry>> buffer_;
    std::mutex mutex_;
    
    Stats stats_;
    
    void schedule_group_timeout(const std::string& media_group_id);
    void process_group(const std::string& media_group_id);
    bool is_oversized(const TelegramMediaItem& item) const;
};

// ============================================================================
// Media Group Builder (for sending)
// ============================================================================

class MediaGroupBuilder {
public:
    MediaGroupBuilder() = default;
    
    // Add photo
    MediaGroupBuilder& add_photo(
        const std::string& file_id,
        const std::string& caption = ""
    );
    
    // Add video
    MediaGroupBuilder& add_video(
        const std::string& file_id,
        const std::string& caption = ""
    );
    
    // Add document
    MediaGroupBuilder& add_document(
        const std::string& file_id,
        const std::string& caption = ""
    );
    
    // Build JSON array for sendMediaGroup API
    core::JsonArray build() const;
    
    // Get item count
    size_t size() const { return items_.size(); }
    
    // Clear all items
    void clear() { items_.clear(); }
    
private:
    std::vector<core::JsonObject> items_;
};

// ============================================================================
// Telegram Media Group API Client
// ============================================================================

class TelegramMediaGroupAPI {
public:
    TelegramMediaGroupAPI(const std::string& bot_token, const std::string& base_url);
    
    // Send media group
    core::Result<std::vector<int64_t>> send_media_group(
        int64_t chat_id,
        const core::JsonArray& media,
        const std::string& caption = ""  // Overall caption (optional)
    );
    
    // Send media group with builder
    core::Result<std::vector<int64_t>> send_media_group(
        int64_t chat_id,
        const MediaGroupBuilder& builder
    );
    
private:
    std::string bot_token_;
    std::string base_url_;
};

// ============================================================================
// Helper Functions
// ============================================================================

namespace media_group_utils {

// Extract media item from Telegram update JSON
core::Result<TelegramMediaItem> extract_media_from_message(const core::JsonObject& message);

// Check if message is part of media group
inline bool is_media_group_message(const core::JsonObject& message) {
    return message.count("media_group_id") > 0 && 
           !message.at("media_group_id").is_null();
}

// Group messages by media_group_id
std::map<std::string, std::vector<TelegramMessage>> group_messages(
    const std::vector<TelegramMessage>& messages
);

// Sort media group by message_id (preserve order)
void sort_media_group(std::vector<TelegramMessage>& messages);

}  // namespace media_group_utils

}  // namespace channels
}  // namespace hunterclaw
