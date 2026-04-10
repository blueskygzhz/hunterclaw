#include "hunterclaw/memory/manager_sync_control.h"
#include <regex>
#include <algorithm>

// 严格对齐 OpenClaw memory/manager-sync-control.ts (194 行)

namespace hunterclaw {
namespace memory {

// 对齐 manager-sync-control.ts:37-68
bool is_memory_readonly_db_error(const std::string& error_message) {
    static const std::regex readonly_pattern(
        "attempt to write a readonly database|database is read-only|SQLITE_READONLY",
        std::regex::icase);
    return std::regex_search(error_message, readonly_pattern);
}

// 对齐 manager-sync-control.ts:70-84
std::string extract_memory_error_reason(const std::exception& err) {
    std::string msg = err.what();
    auto s = msg.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return "unknown error";
    auto e = msg.find_last_not_of(" \t\n\r");
    return msg.substr(s, e - s + 1);
}

// 对齐 manager-sync-control.ts:126-172
void MemoryTargetedSessionSyncQueue::enqueue(const std::vector<std::string>& session_files) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& sf : session_files) {
        std::string trimmed = sf;
        auto s = trimmed.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        auto e = trimmed.find_last_not_of(" \t");
        trimmed = trimmed.substr(s, e - s + 1);
        if (!trimmed.empty()) queued_.insert(trimmed);
    }
}

std::vector<std::string> MemoryTargetedSessionSyncQueue::drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result(queued_.begin(), queued_.end());
    queued_.clear();
    return result;
}

bool MemoryTargetedSessionSyncQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queued_.empty();
}

void MemoryTargetedSessionSyncQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    queued_.clear();
}

}  // namespace memory
}  // namespace hunterclaw
