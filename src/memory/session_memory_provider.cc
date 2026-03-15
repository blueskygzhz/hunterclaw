#include "hunterclaw/memory/session_memory_provider.h"
#include "hunterclaw/memory/session_files.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

namespace {
    auto log() {
        auto logger = spdlog::get("memory");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("memory");
        }
        return logger;
    }
}

// ========================================
// SessionMemoryProvider Implementation
// 严格对齐 OpenClaw 的 session memory 功能
// ========================================

SessionMemoryProvider::SessionMemoryProvider(
    const std::string& provider_id,
    std::shared_ptr<MemoryStorage> storage,
    const SessionMemoryConfig& config
) : provider_id_(provider_id),
    storage_(storage),
    config_(config) {
    
    log()->info("SessionMemoryProvider created: id={}, enabled={}, export_dir={}, retention_days={}",
                provider_id_, config_.enabled, 
                config_.export_dir.value_or("none"),
                config_.retention_days.value_or(0));
}

// ========================================
// MemoryProvider 接口实现
// ========================================

core::Result<std::string> SessionMemoryProvider::add_memory(
    const std::string& content,
    const std::map<std::string, std::string>& metadata
) {
    if (!config_.enabled) {
        return core::Result<std::string>::Ok("");  // 静默跳过
    }
    
    log()->debug("Session memory add_memory called with content: {}", content);
    // Session memory 主要用于导出，不需要添加 memory
    // 返回空 ID
    return core::Result<std::string>::Ok("");
}

core::Result<std::vector<MemorySearchResult>> SessionMemoryProvider::search_memories(
    const std::string& query,
    const SearchOptions& options
) {
    if (!config_.enabled) {
        return core::Result<std::vector<MemorySearchResult>>::Err("Session memory is disabled");
    }
    
    // 简单实现：返回空结果（session memory 主要用于导出，不用于搜索）
    log()->debug("Session memory search_memories called for query: {}", query);
    return core::Result<std::vector<MemorySearchResult>>::Ok(std::vector<MemorySearchResult>{});
}

core::Result<bool> SessionMemoryProvider::remove_memory(const std::string& id) {
    if (!config_.enabled) {
        return core::Result<bool>::Ok(false);
    }
    
    log()->debug("Session memory remove_memory called for id: {}", id);
    // Session memory 不支持删除
    return core::Result<bool>::Ok(false);
}

core::Result<bool> SessionMemoryProvider::update_memory(
    const std::string& id,
    const std::string& content,
    const std::map<std::string, std::string>& metadata
) {
    if (!config_.enabled) {
        return core::Result<bool>::Ok(false);
    }
    
    log()->debug("Session memory update_memory called for id: {}", id);
    // Session memory 不支持更新
    return core::Result<bool>::Ok(false);
}

core::Result<bool> SessionMemoryProvider::clear_memories() {
    if (!config_.enabled) {
        return core::Result<bool>::Ok(false);
    }
    
    log()->debug("Session memory clear_memories called");
    // Session memory 不支持清空
    return core::Result<bool>::Ok(false);
}

// ========================================
// Session 数据导出 (对齐 OpenClaw)
// ========================================

core::Result<void> SessionMemoryProvider::export_session_messages(
    const std::string& session_key,
    const std::vector<SessionMessage>& messages
) {
    if (!config_.enabled || !config_.export_dir) {
        return core::Result<void>::Ok();  // 静默跳过
    }
    
    // 对齐 OpenClaw: exportSessionMessagesBatch
    // src/memory/session-files.ts:274-340
    
    // 1. 生成导出文件名
    fs::path export_dir = config_.export_dir.value();
    fs::path session_file = generate_session_filename(session_key);
    fs::path full_path = export_dir / session_file;
    
    // 2. 确保目录存在
    if (!fs::exists(export_dir)) {
        fs::create_directories(export_dir);
    }
    
    // 3. 写入 JSONL 格式 (对齐 OpenClaw)
    try {
        std::ofstream ofs(full_path, std::ios::app);
        if (!ofs) {
            return core::Result<void>::Err("Failed to open session file: " + full_path.string());
        }
        
        for (const auto& msg : messages) {
            json line = {
                {"timestamp", msg.timestamp},
                {"role", msg.role},
                {"content", msg.content}
            };
            
            if (!msg.agent_id.empty()) {
                line["agentId"] = msg.agent_id;
            }
            
            ofs << line.dump() << "\n";
        }
        
        ofs.close();
        log()->debug("Exported {} messages to {}", messages.size(), full_path.string());
        
        return core::Result<void>::Ok();
    } catch (const std::exception& e) {
        return core::Result<void>::Err(std::string("Session export failed: ") + e.what());
    }
}

// ========================================
// Delta 触发机制 (对齐 OpenClaw)
// ========================================

core::Result<void> SessionMemoryProvider::trigger_delta_update(
    const std::string& session_key,
    const std::vector<SessionMessage>& new_messages
) {
    // 对齐 OpenClaw: src/memory/builtin-manager.ts:1036-1083
    // triggerDeltaUpdate 方法
    
    if (!config_.enabled) {
        return core::Result<void>::Ok();
    }
    
    log()->info("Triggering delta update for session: {} ({} new messages)",
                  session_key, new_messages.size());
    
    // 1. 导出消息到文件
    auto export_result = export_session_messages(session_key, new_messages);
    if (!export_result.is_ok()) {
        log()->warn("Delta export failed: {}", export_result.error);
    }
    
    // 2. 索引新消息到数据库
    for (const auto& msg : new_messages) {
        // 提取文本内容
        std::string text = extract_session_text(msg.content);
        if (text.empty()) {
            continue;
        }
        
        // 创建 Memory 对象
        Memory memory;
        memory.content = text;
        memory.metadata = {
            {"source", "sessions"},
            {"session_key", session_key},
            {"role", msg.role},
            {"timestamp", std::to_string(msg.timestamp)},
            {"path", "sessions/" + session_key + ".jsonl"}
        };
        
        if (!msg.agent_id.empty()) {
            memory.metadata["agent_id"] = msg.agent_id;
        }
        
        // 生成 embedding (异步)
        // TODO: 实现异步 embedding 生成
        // 对齐 OpenClaw: this.embed([memory])
    }
    
    log()->info("Delta update completed for session: {}", session_key);
    return core::Result<void>::Ok();
}

// ========================================
// Session 数据清理 (对齐 OpenClaw)
// ========================================

core::Result<void> SessionMemoryProvider::prune_old_sessions() {
    if (!config_.enabled || !config_.retention_days || !config_.export_dir) {
        return core::Result<void>::Ok();
    }
    
    // 对齐 OpenClaw: pruneOldSessionFiles
    // src/memory/builtin-manager.ts:1085-1125
    
    int retention_days = config_.retention_days.value();
    fs::path export_dir = config_.export_dir.value();
    
    log()->info("Pruning sessions older than {} days from {}", 
                  retention_days, export_dir.string());
    
    // 计算截止时间
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24 * retention_days);
    auto cutoff_time_t = std::chrono::system_clock::to_time_t(cutoff);
    
    int deleted_count = 0;
    
    try {
        if (!fs::exists(export_dir)) {
            return core::Result<void>::Ok();
        }
        
        for (const auto& entry : fs::directory_iterator(export_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            
            // 检查文件修改时间
            auto ftime = fs::last_write_time(entry);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            auto ftime_t = std::chrono::system_clock::to_time_t(sctp);
            
            if (ftime_t < cutoff_time_t) {
                fs::remove(entry);
                deleted_count++;
                log()->debug("Pruned old session file: {}", entry.path().filename().string());
            }
        }
        
        log()->info("Pruned {} old session files", deleted_count);
        return core::Result<void>::Ok();
        
    } catch (const std::exception& e) {
        return core::Result<void>::Err(std::string("Session pruning failed: ") + e.what());
    }
}

// ========================================
// 辅助方法
// ========================================

fs::path SessionMemoryProvider::generate_session_filename(const std::string& session_key) const {
    // 对齐 OpenClaw: sessionKeyToFilename
    // 简化: 直接使用 session_key (替换特殊字符)
    std::string safe_key = session_key;
    std::replace(safe_key.begin(), safe_key.end(), ':', '-');
    std::replace(safe_key.begin(), safe_key.end(), '/', '-');
    return fs::path(safe_key + ".jsonl");
}

}  // namespace memory
}  // namespace hunterclaw