#pragma once

#include "hunterclaw/memory/provider.h"
#include "hunterclaw/memory/storage.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace memory {

using json = nlohmann::json;

// ========================================
// Session Memory Configuration
// 对齐 OpenClaw ResolvedQmdSessionConfig
// ========================================

struct SessionMemoryConfig {
    bool enabled = false;
    std::optional<std::string> export_dir;  // 导出目录 (对齐 exportDir)
    std::optional<int> retention_days;      // 保留天数 (对齐 retentionDays)
};

// ========================================
// Session Message Structure
// 对齐 OpenClaw session-files.ts
// ========================================

struct SessionMessage {
    int64_t timestamp;           // Unix timestamp (ms)
    std::string role;            // "user" | "assistant" | "system"
    json content;                // string | array of content blocks
    std::string agent_id;        // Optional agent ID
};

// ========================================
// SessionMemoryProvider
// 严格对齐 OpenClaw session memory 功能
// 参考: src/memory/builtin-manager.ts
// ========================================

class SessionMemoryProvider {
public:
    SessionMemoryProvider(
        const std::string& provider_id,
        std::shared_ptr<MemoryStorage> storage,
        const SessionMemoryConfig& config
    );
    
    virtual ~SessionMemoryProvider() = default;
    
    // ========================================
    // MemoryProvider 接口实现
    // ========================================
    
    // Provider identification
    std::string provider_id() const { return provider_id_; }
    std::string provider_name() const { return "SessionMemoryProvider"; }
    std::string provider_version() const { return "1.0.0"; }
    
    // Memory CRUD operations
    core::Result<std::string> add_memory(
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    );
    
    core::Result<std::vector<MemorySearchResult>> search_memories(
        const std::string& query,
        const SearchOptions& options = {}
    );
    
    core::Result<bool> remove_memory(const std::string& id);
    
    core::Result<bool> update_memory(
        const std::string& id,
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    );
    
    core::Result<bool> clear_memories();
    
    // ========================================
    // Session Memory 专属接口 (对齐 OpenClaw)
    // ========================================
    
    // 导出 session 消息到文件
    // 对齐: exportSessionMessagesBatch (session-files.ts:274)
    core::Result<void> export_session_messages(
        const std::string& session_key,
        const std::vector<SessionMessage>& messages
    );
    
    // Delta 触发机制: 增量更新 session 数据
    // 对齐: triggerDeltaUpdate (builtin-manager.ts:1036)
    core::Result<void> trigger_delta_update(
        const std::string& session_key,
        const std::vector<SessionMessage>& new_messages
    );
    
    // 清理过期 session 文件
    // 对齐: pruneOldSessionFiles (builtin-manager.ts:1085)
    core::Result<void> prune_old_sessions();
    
    // 获取配置
    const SessionMemoryConfig& config() const { return config_; }

private:
    std::string provider_id_;
    std::shared_ptr<MemoryStorage> storage_;
    SessionMemoryConfig config_;
    
    // 生成 session 文件名
    // 对齐: sessionKeyToFilename (session-files.ts)
    std::filesystem::path generate_session_filename(const std::string& session_key) const;
};

} // namespace memory
} // namespace hunterclaw
