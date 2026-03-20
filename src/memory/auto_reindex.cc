#include <optional>
#include "hunterclaw/memory/auto_reindex.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include "hunterclaw/core/json.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <random>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace memory {

// 严格对齐 OpenClaw src/memory/manager-sync-ops.ts
// 实现完整的 atomic reindex 逻辑：临时数据库、状态恢复、安全交换

// ============================================================================
// Utility Functions
// ============================================================================

// 生成随机 UUID (简化版)
static std::string gene

rate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static const char* hex = "0123456789abcdef";
    
    std::string uuid = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    for (char& c : uuid) {
        if (c == 'x') {
            c = hex[dis(gen)];
        } else if (c == 'y') {
            c = hex[(dis(gen) & 0x3) | 0x8];
        }
    }
    return uuid;
}

// ============================================================================
// AutoReindexDetector Implementation (对齐 manager-sync-ops.ts:278-327)
// ============================================================================

AutoReindexDetector::AutoReindexDetector(const std::string& state_file)
    : state_file_(state_file) {}

core::Result<ReindexReason> AutoReindexDetector::check_reindex_needed(
    const MemoryProviderInfo& current_info) {
    
    auto load_result = load_state();
    if (!load_result.is_ok()) {
        // No saved state - first time initialization
        return core::Result<ReindexReason>::Ok(ReindexReason{ReindexReason::None, "", ""});
    }
    
    const MemoryProviderInfo& old_info = load_result.get();
    ReindexReason reason = compare_provider_info(old_info, current_info);
    
    return core::Result<ReindexReason>::Ok(reason);
}

core::Result<bool> AutoReindexDetector::save_state(const MemoryProviderInfo& info) {
    try {
        json state = {
            {"provider", info.provider},
            {"model", info.model},
            {"output_dimensions", info.output_dimensions},
            {"endpoint", info.endpoint}
        };
        
        std::ofstream file(state_file_);
        if (!file) {
            return core::Result<bool>::Err("Failed to open state file for writing");
        }
        
        file << state.dump(2);
        file.close();
        
        return core::Result<bool>::Ok(true);
        
    } catch (const std::exception& e) {
        return core::Result<bool>::Err(
            std::string("Failed to save state: ") + e.what()
        );
    }
}

core::Result<MemoryProviderInfo> AutoReindexDetector::load_state() {
    try {
        std::ifstream file(state_file_);
        if (!file) {
            return core::Result<MemoryProviderInfo>::Err("State file not found");
        }
        
        json state;
        file >> state;
        
        MemoryProviderInfo info;
        info.provider = state.value("provider", "");
        info.model = state.value("model", "");
        info.output_dimensions = state.value("output_dimensions", -1);
        info.endpoint = state.value("endpoint", "");
        
        return core::Result<MemoryProviderInfo>::Ok(info);
        
    } catch (const std::exception& e) {
        return core::Result<MemoryProviderInfo>::Err(
            std::string("Failed to load state: ") + e.what()
        );
    }
}

core::Result<bool> AutoReindexDetector::clear_state() {
    if (std::remove(state_file_.c_str()) != 0) {
        return core::Result<bool>::Err("Failed to delete state file");
    }
    return core::Result<bool>::Ok(true);
}

ReindexReason AutoReindexDetector::compare_provider_info(
    const MemoryProviderInfo& old_info,
    const MemoryProviderInfo& new_info) {
    
    ReindexReason reason;
    
    // Check provider change (highest priority)
    if (old_info.provider != new_info.provider) {
        reason.type = ReindexReason::ProviderChanged;
        reason.old_value = old_info.provider;
        reason.new_value = new_info.provider;
        return reason;
    }
    
    // Check model change
    if (old_info.model != new_info.model) {
        reason.type = ReindexReason::ModelChanged;
        reason.old_value = old_info.model;
        reason.new_value = new_info.model;
        return reason;
    }
    
    // Check dimensions change (Gemini specific)
    if (old_info.output_dimensions != new_info.output_dimensions) {
        // Only trigger reindex if dimensions are actually set (not default -1)
        if (new_info.output_dimensions > 0) {
            reason.type = ReindexReason::DimensionsChanged;
            reason.old_value = std::to_string(old_info.output_dimensions);
            reason.new_value = std::to_string(new_info.output_dimensions);
            return reason;
        }
    }
    
    // Check endpoint change
    if (old_info.endpoint != new_info.endpoint) {
        reason.type = ReindexReason::EndpointChanged;
        reason.old_value = old_info.endpoint;
        reason.new_value = new_info.endpoint;
        return reason;
    }
    
    // No changes
    reason.type = ReindexReason::None;
    return reason;
}

// ============================================================================
// MemoryManagerWithAutoReindex Implementation (对齐 manager-sync-ops.ts:1148-1305)
// ============================================================================

MemoryManagerWithAutoReindex::MemoryManagerWithAutoReindex(
    const std::string& data_dir,
    std::shared_ptr<AutoReindexDetector> detector)
    : data_dir_(data_dir), detector_(detector) {}

core::Result<bool> MemoryManagerWithAutoReindex::initialize(
    const MemoryProviderInfo& provider_info) {
    
    current_provider_ = provider_info;
    
    // Check if reindex is needed
    auto reindex_result = detector_->check_reindex_needed(provider_info);
    if (!reindex_result.is_ok()) {
        return core::Result<bool>::Err(reindex_result.error);
    }
    
    const ReindexReason& reason = reindex_result.get();
    
    if (reason.type != ReindexReason::None) {
        // Log reindex reason
        // TODO: Use proper logging system
        // Trigger automatic reindex (safe by default)
        auto reindex_success = run_safe_reindex();
        if (!reindex_success.is_ok()) {
            return core::Result<bool>::Err(
                "Auto-reindex failed: " + reindex_success.error
            );
        }
    }
    
    // Save current state
    auto save_result = detector_->save_state(provider_info);
    if (!save_result.is_ok()) {
        return core::Result<bool>::Err(
            "Failed to save provider state: " + save_result.error
        );
    }
    
    // Update status
    current_status_.provider = provider_info.provider;
    current_status_.model = provider_info.model;
    current_status_.output_dimensions = provider_info.output_dimensions;
    
    return core::Result<bool>::Ok(true);
}

MemoryStatus MemoryManagerWithAutoReindex::status() const {
    return current_status_;
}

core::Result<bool> MemoryManagerWithAutoReindex::reindex(bool force) {
    if (!force && current_status_.reindex_in_progress) {
        return core::Result<bool>::Err("Reindex already in progress");
    }
    
    current_status_.reindex_in_progress = true;
    
    // Choose safe or unsafe reindex based on environment variable
    const char* unsafe_env = std::getenv("HUNTERCLAW_TEST_MEMORY_UNSAFE_REINDEX");
    auto result = (unsafe_env && std::string(unsafe_env) == "1") 
                  ? run_unsafe_reindex()
                  : run_safe_reindex();
    
    current_status_.reindex_in_progress = false;
    
    return result;
}

core::Result<bool> MemoryManagerWithAutoReindex::sync(const std::string& reason) {
    // Incremental sync (对齐 OpenClaw manager-sync-ops.ts)
    return do_incremental_sync();
}

bool MemoryManagerWithAutoReindex::needs_reindex(const MemoryProviderInfo& new_provider) {
    auto result = detector_->check_reindex_needed(new_provider);
    if (!result.is_ok()) {
        return false;
    }
    const ReindexReason& reason = result.get();
    return reason.type != ReindexReason::None;
}

// ============================================================================
// Atomic Reindex Implementation (对齐 manager-sync-ops.ts:1148-1252)
// ============================================================================
//
// Atomic Reindex 算法 (Safe Mode):
// ═══════════════════════════════════════════════════════════════════════════
//
// 目标: 在重建索引过程中，如果失败则保持原索引不变，确保数据一致性
//
// 流程:
// 1. 创建临时数据库 (index.sqlite.tmp-{uuid})
// 2. 保存原数据库的状态 (fts/vector 可用性、dimensions 等)
// 3. 切换到临时数据库进行所有操作:
//    a. 从原数据库复制 embedding cache (seed_embedding_cache)
//    b. 重新扫描和分块文件
//    c. 生成新的 embeddings
//    d. 写入新的元数据
// 4. 关闭两个数据库连接
// 5. 原子性交换文件:
//    a. 原文件 -> 备份文件 (index.sqlite.backup-{uuid})
//    b. 临时文件 -> 原文件
//    c. 删除备份文件
// 6. 重新打开原数据库（现在包含新索引）
//
// 失败处理:
// - 任何步骤失败都会:
//   1. 关闭临时数据库
//   2. 删除临时文件
//   3. 恢复原数据库状态
//   4. 抛出错误
//
// 对齐 OpenClaw:
// - manager-sync-ops.ts:1148-1252 (runSafeReindex)
// - manager-sync-ops.ts:328-338 (swapIndexFiles)
// - manager-sync-ops.ts:278-327 (seedEmbeddingCache)
//
// ═══════════════════════════════════════════════════════════════════════════

core::Result<bool> MemoryManagerWithAutoReindex::run_safe_reindex() {
    // 对齐 OpenClaw manager-sync-ops.ts:1148-1252
    // 实现原子性重建索引：临时数据库 -> 交换 -> 失败回滚
    
    if (db_path_.empty()) {
        return core::Result<bool>::Err("Database path not configured");
    }
    
    if (!storage_) {
        return core::Result<bool>::Err("Storage not initialized");
    }
    
    // 1. 创建临时数据库路径 (对齐:1156)
    std::string temp_db_path = db_path_ + ".tmp-" + generate_uuid();
    
    // 2. 打开临时数据库 (对齐:1157)
    SQLiteVectorStorage::Config temp_config{
        .db_path = temp_db_path,
        .dimensions = current_provider_.output_dimensions > 0 
                     ? current_provider_.output_dimensions : 1536,
        .enable_fts = true,
        .enable_vector = true
    };
    
    auto temp_storage_result = SQLiteVectorStorage::create(temp_config);
    if (!temp_storage_result.is_ok()) {
        return core::Result<bool>::Err(
            "Failed to create temp database: " + temp_storage_result.error
        );
    }
    auto temp_storage = temp_storage_result.get();
    
    // 3. 保存原始状态 (对齐:1159-1170)
    struct OriginalState {
        bool fts_available;
        std::string fts_error;
        bool vector_available;
        std::string vector_load_error;
        int vector_dims;
        std::shared_ptr<SQLiteVectorStorage> storage;
    } original_state{
        .fts_available = current_status_.fts_available,
        .fts_error = current_status_.fts_error,
        .vector_available = current_status_.vector_available,
        .vector_load_error = current_status_.vector_load_error,
        .vector_dims = current_provider_.output_dimensions,
        .storage = storage_
    };
    
    // 4. 切换到临时数据库 (对齐:1186-1192)
    auto original_storage = storage_;
    storage_ = temp_storage;
    
    current_status_.vector_available = false;
    current_status_.vector_dims = -1;
    
    // Lambda for restoring original state
    auto restore_original_state = [&]() {
        storage_ = original_storage;
        current_status_.fts_available = original_state.fts_available;
        current_status_.fts_error = original_state.fts_error;
        current_status_.vector_available = original_state.vector_available;
        current_status_.vector_load_error = original_state.vector_load_error;
        current_status_.vector_dims = original_state.vector_dims;
    };
    
    try {
        // 5. 从原始数据库种子缓存 (对齐:1191, 278-327)
        auto seed_result = seed_embedding_cache(original_storage.get(), temp_storage.get());
        if (!seed_result.is_ok()) {
            throw std::runtime_error("Failed to seed embedding cache: " + seed_result.error);
        }
        
        // 6. 同步内存文件 (对齐:1192-1199)
        bool should_sync_memory = true;  // 简化：总是同步
        if (should_sync_memory && sync_callback_) {
            auto sync_result = sync_callback_(SyncParams{
                .reason = "reindex",
                .force = true
            });
            if (!sync_result.is_ok()) {
                throw std::runtime_error("Failed to sync memory files: " + sync_result.error);
            }
        }
        
        // 7. 同步会话文件 (对齐 manager-sync-ops.ts:1201-1210)
        // 对齐 openclaw: shouldSyncSessions && syncSessionFiles()
        // 当 sources 包含 sessions 时，同步 session 文件
        if (sync_callback_) {
            SyncParams session_params;
            session_params.reason = "reindex-sessions";
            session_params.force = true;
            // session_files 为空表示同步所有 session 文件
            sync_callback_(session_params);
        }
        
        // 8. 写入新的元数据 (对齐:1212-1228)
        MemoryIndexMeta next_meta{
            .model = current_provider_.model.empty() ? "fts-only" : current_provider_.model,
            .provider = current_provider_.provider.empty() ? "none" : current_provider_.provider,
        .provider_key = current_provider_.provider.empty() ? std::nullopt
                          : std::make_optional(current_provider_.provider + ":" + current_provider_.model),
            .sources = std::nullopt,
            .scope_hash = std::nullopt,
            .chunk_tokens = 512,
            .chunk_overlap = 128,
            .vector_dims = current_provider_.output_dimensions > 0 
                          ? std::optional<int>(current_provider_.output_dimensions) 
                          : std::nullopt
        };
        
        auto write_meta_result = write_meta(next_meta, temp_storage.get());
        if (!write_meta_result.is_ok()) {
            throw std::runtime_error("Failed to write metadata: " + write_meta_result.error);
        }
        
        // 9. 关闭数据库 (对齐:1232-1234)
        temp_storage.reset();  // 关闭临时数据库
        original_storage.reset();  // 关闭原数据库
        
        // 10. 原子性交换索引文件 (对齐:1237)
        auto swap_result = swap_index_files(db_path_, temp_db_path);
        if (!swap_result.is_ok()) {
            // 交换失败，重新打开原数据库
            auto reopen_result = SQLiteVectorStorage::create(SQLiteVectorStorage::Config{
                .db_path = db_path_,
                .dimensions = original_state.vector_dims,
                .enable_fts = true,
                .enable_vector = true
            });
            if (reopen_result.is_ok()) {
                storage_ = reopen_result.get();
            }
            return core::Result<bool>::Err("Failed to swap index files: " + swap_result.error);
        }
        
        // 11. 重新打开数据库 (对齐:1239-1244)
        auto reopen_result = SQLiteVectorStorage::create(SQLiteVectorStorage::Config{
            .db_path = db_path_,
            .dimensions = next_meta.vector_dims.value_or(1536),
            .enable_fts = true,
            .enable_vector = true
        });
        
        if (!reopen_result.is_ok()) {
            return core::Result<bool>::Err(
                "Failed to reopen database after swap: " + reopen_result.error
            );
        }
        
        storage_ = reopen_result.get();
        current_status_.vector_dims = next_meta.vector_dims.value_or(-1);
        current_status_.vector_available = next_meta.vector_dims.has_value();
        
    } catch (const std::exception& e) {
        // 12. 失败回滚 (对齐:1245-1251)
        temp_storage.reset();  // 关闭临时数据库
        remove_index_files(temp_db_path);  // 删除临时文件
        restore_original_state();  // 恢复原状态
        
        return core::Result<bool>::Err(
            std::string("Reindex failed: ") + e.what()
        );
    }
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> MemoryManagerWithAutoReindex::run_unsafe_reindex() {
    // 对齐 OpenClaw manager-sync-ops.ts:1254-1301
    // 
    // Unsafe Reindex (性能优化版本):
    // ═══════════════════════════════════════════════════════════════════
    // 用途: 测试环境，跳过临时数据库和原子交换，减少文件系统和 SQLite 开销
    // 
    // 流程:
    // 1. 直接在原数据库上清空所有数据 (reset_index)
    // 2. 重新扫描和索引文件
    // 3. 写入新元数据
    // 
    // 警告:
    // - 如果失败，原索引数据丢失
    // - 仅用于隔离的测试环境 (per-test HOME)
    // - 通过环境变量 HUNTERCLAW_TEST_MEMORY_UNSAFE_REINDEX=1 启用
    // 
    // 对齐: manager-sync-ops.ts:1260
    // ═══════════════════════════════════════════════════════════════════
    
    if (!storage_) {
        return core::Result<bool>::Err("Storage not initialized");
    }
    
    // 1. 重置索引 (对齐:1262, 1305-1315)
    auto reset_result = reset_index();
    if (!reset_result.is_ok()) {
        return core::Result<bool>::Err("Failed to reset index: " + reset_result.error);
    }
    
    // 2. 同步内存和会话文件 (对齐:1264-1283)
    bool should_sync_memory = true;  // 简化：总是同步
    if (should_sync_memory && sync_callback_) {
        auto sync_result = sync_callback_(SyncParams{
            .reason = "reindex",
            .force = true
        });
        if (!sync_result.is_ok()) {
            return core::Result<bool>::Err("Failed to sync memory files: " + sync_result.error);
        }
    }
    
    // 同步会话文件（对齐 openclaw: shouldSyncSessions && syncSessionFiles()）
    if (sync_callback_) {
        SyncParams session_params;
        session_params.reason = "sync-sessions";
        session_params.force = false;
        sync_callback_(session_params);
    }
    
    // 3. 写入元数据 (对齐:1285-1298)
    MemoryIndexMeta next_meta{
        .model = current_provider_.model.empty() ? "fts-only" : current_provider_.model,
        .provider = current_provider_.provider.empty() ? "none" : current_provider_.provider,
        .provider_key = std::nullopt,
        .sources = std::nullopt,
        .scope_hash = std::nullopt,
        .chunk_tokens = 512,
        .chunk_overlap = 128,
        .vector_dims = current_provider_.output_dimensions > 0 
                      ? std::optional<int>(current_provider_.output_dimensions) 
                      : std::nullopt
    };
    
    auto write_result = write_meta(next_meta, storage_.get());
    if (!write_result.is_ok()) {
        return core::Result<bool>::Err("Failed to write metadata: " + write_result.error);
    }

    // 清理过期缓存 (对齐 pruneEmbeddingCacheIfNeeded)
    if (storage_) {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        // 保留最近 30 天的缓存
        int64_t cutoff = now_ms - 30LL * 24 * 60 * 60 * 1000;
        storage_->cleanup_old_cache(cutoff);
    }
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> MemoryManagerWithAutoReindex::reset_index() {
    // 对齐 OpenClaw manager-sync-ops.ts:1305-1315
    // 清空所有表数据，保留结构
    
    if (!storage_) {
        return core::Result<bool>::Err("Storage not initialized");
    }
    
    // 清空所有chunks
    auto clear_result = storage_->clear_all();
    if (!clear_result.is_ok()) {
        return core::Result<bool>::Err("Failed to clear chunks: " + clear_result.error);
    }
    
    // 重置向量维度
    current_status_.vector_dims = -1;
    current_status_.vector_available = false;
    
    return core::Result<bool>::Ok(true);
}

// ============================================================================
// File Operations (对齐 manager-sync-ops.ts:328-361)
// ============================================================================

core::Result<bool> MemoryManagerWithAutoReindex::swap_index_files(
    const std::string& target_path,
    const std::string& temp_path) {
    // 对齐 OpenClaw manager-sync-ops.ts:328-338
    //
    // 原子性文件交换:
    // ═══════════════════════════════════════════════════════════════════
    // 步骤:
    // 1. 原文件 -> 备份位置 (.backup-{uuid})
    // 2. 临时文件 -> 原文件位置
    //    - 如果失败: 备份 -> 原位置 (回滚)
    // 3. 删除备份
    //
    // SQLite 文件:
    // - .sqlite      : 主数据库文件
    // - .sqlite-wal  : Write-Ahead Log
    // - .sqlite-shm  : Shared Memory
    //
    // 所有三个文件都必须一起移动才能保证一致性
    // ═══════════════════════════════════════════════════════════════════
    
    std::string backup_path = target_path + ".backup-" + generate_uuid();
    
    // 1. 移动原文件到备份 (对齐:330)
    auto move_to_backup = move_index_files(target_path, backup_path);
    if (!move_to_backup.is_ok()) {
        return core::Result<bool>::Err("Failed to backup original index: " + move_to_backup.error);
    }
    
    // 2. 移动临时文件到目标位置 (对齐:331-335)
    auto move_temp = move_index_files(temp_path, target_path);
    if (!move_temp.is_ok()) {
        // 回滚：恢复备份
        move_index_files(backup_path, target_path);
        return core::Result<bool>::Err("Failed to move temp index: " + move_temp.error);
    }
    
    // 3. 删除备份 (对齐:337)
    remove_index_files(backup_path);
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> MemoryManagerWithAutoReindex::move_index_files(
    const std::string& source_base,
    const std::string& target_base) {
    // 对齐 OpenClaw manager-sync-ops.ts:340-353
    // SQLite 有 3 个文件：.db, .db-wal, .db-shm
    
    const std::vector<std::string> suffixes = {"", "-wal", "-shm"};
    
    for (const auto& suffix : suffixes) {
        std::string source = source_base + suffix;
        std::string target = target_base + suffix;
        
        int result = std::rename(source.c_str(), target.c_str());
        if (result != 0 && errno != ENOENT) {
            return core::Result<bool>::Err(
                std::string("Failed to rename ") + source + " to " + target + 
                ": " + std::strerror(errno)
            );
        }
    }
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> MemoryManagerWithAutoReindex::remove_index_files(
    const std::string& base_path) {
    // 对齐 OpenClaw manager-sync-ops.ts:355-358
    
    const std::vector<std::string> suffixes = {"", "-wal", "-shm"};
    
    for (const auto& suffix : suffixes) {
        std::string path = base_path + suffix;
        std::remove(path.c_str());  // 忽略错误（文件可能不存在）
    }
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> MemoryManagerWithAutoReindex::seed_embedding_cache(
    SQLiteVectorStorage* source_storage, 
    SQLiteVectorStorage* target_storage) {
    // 对齐 OpenClaw manager-sync-ops.ts:278-327
    // 从原始数据库复制 embedding cache 到临时数据库
    
    if (!source_storage || !target_storage) {
        return core::Result<bool>::Err("Invalid storage pointers");
    }
    
    // 简化实现：暂时跳过 embedding cache 复制
    // TODO: 实现完整的 cache 复制逻辑
    // 1. 需要在 SQLiteVectorStorage 中添加 get_all_cached_embeddings() 方法
    // 2. 需要添加批量 cache_embedding() 方法
    // 
    // 不复制 cache 的影响：
    // - 重建索引时需要重新生成所有 embeddings
    // - 性能下降，但功能完整
    
    // 软失败：返回成功，继续 reindex
    return core::Result<bool>::Ok(true);
}

core::Result<bool> MemoryManagerWithAutoReindex::write_meta(
    const MemoryIndexMeta& meta,
    SQLiteVectorStorage* storage) {
    // 对齐 OpenClaw manager-sync-ops.ts:1230
    // 写入索引元数据到数据库
    
    if (!storage) {
        return core::Result<bool>::Err("Storage not initialized");
    }
    
    try {
        // 构建元数据 JSON
        json meta_json = {
            {"model", meta.model},
            {"provider", meta.provider},
            {"chunk_tokens", meta.chunk_tokens},
            {"chunk_overlap", meta.chunk_overlap}
        };
        
        if (meta.provider_key.has_value()) {
            meta_json["provider_key"] = meta.provider_key.value();
        }
        
        if (meta.vector_dims.has_value()) {
            meta_json["vector_dims"] = meta.vector_dims.value();
        }
        
        if (meta.sources.has_value()) {
            json sources_arr = json::array();
            for (const auto& source : meta.sources.value()) {
                sources_arr.push_back(static_cast<int>(source));
            }
            meta_json["sources"] = sources_arr;
        }
        
        if (meta.scope_hash.has_value()) {
            meta_json["scope_hash"] = meta.scope_hash.value();
        }
        
        // 写入元数据表 (对齐 openclaw: db.run("INSERT OR REPLACE INTO meta ..."))
        if (storage_) {
            std::string meta_str = meta_json.dump();
            // Escape single quotes for SQL
            std::string escaped;
            for (char c : meta_str) {
                if (c == '\'') escaped += "''";
                else escaped += c;
            }
            storage_->exec_sql(
                "INSERT OR REPLACE INTO meta (key, value) VALUES ('index_meta', '" + escaped + "')"
            );
        }
        
        return core::Result<bool>::Ok(true);
        
    } catch (const std::exception& e) {
        return core::Result<bool>::Err(
            std::string("Failed to write metadata: ") + e.what()
        );
    }
}

core::Result<bool> MemoryManagerWithAutoReindex::do_incremental_sync() {
    // TODO: 实现增量同步
    // 1. 检测变更的文件 (mtime 检查)
    // 2. 仅重新分块变更的文件
    // 3. 更新 embeddings
    // 4. 更新数据库
    
    return core::Result<bool>::Ok(true);
}

}  // namespace memory
}  // namespace hunterclaw
