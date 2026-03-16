#pragma once

#include "hunterclaw/memory/compaction.h"
#include "hunterclaw/memory/memory_provider.h"
#include "hunterclaw/core/result.h"
#include <memory>
#include <functional>
#include <vector>
#include <string>
#include <memory>
#include <chrono>

namespace hunterclaw {
namespace memory {

// ============================================================================
// CompactionManager - 严格对齐 OpenClaw compaction 机制
// ============================================================================

// 注意：CompactionResult 和 CompactionStats 已在 compaction.h 中定义

/**
 * Compaction 参数 (对齐 OpenClaw CompactEmbeddedPiSessionParams)
 */
struct CompactionParams {
    // 核心参数
    std::vector<AgentMessage> messages;             // 待压缩的消息
    std::string model_id;                           // LLM 模型 ID
    std::string api_key;                            // LLM API key
    
    // Token 计数
    int current_token_count = 0;                    // 当前 token 数
    int context_window_tokens = 200000;             // 上下文窗口大小
    std::shared_ptr<size_t> transcript_size_bytes;    // Transcript 文件大小
    std::shared_ptr<int> compaction_count;            // Compaction 计数 (用于 flush 去重)
    
    // 触发参数
    bool force = false;                             // 强制压缩
    std::string trigger;                            // 触发原因: "overflow", "manual"
    
    // 回调
    std::function<core::Result<void>()> memory_flush_callback;  // Memory flush 回调
    std::function<core::Result<void>(const CompactionResult&)> post_compaction_sync_callback;  // Post-compaction sync
};

/**
 * Pre-compaction Memory Flush 参数 (对齐 OpenClaw shouldRunMemoryFlush)
 */
struct PreCompactionFlushParams {
    int current_token_count = 0;
    int context_window_tokens = 200000;
    size_t transcript_size_bytes = 0;
    int compaction_count = 0;
    std::function<core::Result<void>()> flush_callback;
};

// 注意：CompactionStats 已在 compaction.h 中定义

/**
 * CompactionManager - 完整的 compaction 生命周期管理
 * 
 * 严格对齐 OpenClaw:
 * - compactEmbeddedPiSessionDirect (compact.ts:475-1038)
 * - shouldRunMemoryFlush (memory-flush.ts:170-215)
 * - runPostCompactionSideEffects (compact.hooks.ts)
 * - syncManager.sync with post-compaction (manager-sync-ops.ts:956-990)
 */
class CompactionManager {
public:
    CompactionManager(
        const CompactionConfig& config,
        std::shared_ptr<hunterclaw::llm::LLMClient> llm_client = nullptr,
        std::shared_ptr<MemoryProvider> memory_provider = nullptr
    );
    
    /**
     * 检查是否需要压缩 (对齐 OpenClaw compact.ts:892-901)
     */
    bool should_compact(
        const std::vector<AgentMessage>& messages,
        int current_token_count,
        int context_window_tokens
    ) const;
    
    /**
     * 执行完整的 session compaction 流程 (对齐 OpenClaw compactEmbeddedPiSessionDirect)
     * 
     * 流程:
     * 1. 验证消息和阈值
     * 2. Pre-compaction memory flush
     * 3. 执行压缩 (summarize_in_stages)
     * 4. Post-compaction memory sync
     * 5. 触发 hooks
     */
    core::Result<CompactionResult> compact_session(
        const CompactionParams& params
    );
    
    /**
     * Trigger pre-compaction memory flush (对齐 OpenClaw memory-flush.ts)
     * 
     * 在压缩前运行一个 agentic turn 将重要记忆持久化到 memory/YYYY-MM-DD.md
     */
    core::Result<void> trigger_pre_compaction_flush(
        const PreCompactionFlushParams& params
    );
    
    /**
     * Post-compaction memory sync (对齐 OpenClaw manager-sync-ops.ts:956-990)
     * 
     * 只更新指定的 transcript files, 不触发 full reindex
     */
    core::Result<void> post_compaction_memory_sync(
        const std::vector<std::string>& session_files
    );
    
    /**
     * 检查是否已经为当前 compaction cycle 刷新过
     * (对齐 OpenClaw hasAlreadyFlushedForCurrentCompaction)
     */
    bool has_already_flushed_for_current_compaction(int compaction_count) const;
    
    /**
     * 获取 compaction 统计
     */
    CompactionStats get_stats() const;
    
    /**
     * 重置统计
     */
    void reset_stats();
    
    /**
     * 获取底层的 MemoryCompactor
     */
    MemoryCompactor& compactor() { return compactor_; }
    const MemoryCompactor& compactor() const { return compactor_; }
    
    /**
     * 获取底层的 MemoryFlusher
     */
    MemoryFlusher& flusher() { return flusher_; }
    const MemoryFlusher& flusher() const { return flusher_; }

private:
    CompactionConfig config_;
    MemoryCompactor compactor_;
    MemoryFlusher flusher_;
    std::shared_ptr<MemoryProvider> memory_provider_;
    
    // 统计
    struct InternalStats {
        int total_compactions = 0;
        int total_memory_flushes = 0;
        int total_failures = 0;
        int tokens_saved = 0;
        std::chrono::system_clock::time_point last_compaction;
    };
    InternalStats stats_;
    
    // Memory flush 去重
    std::chrono::system_clock::time_point last_memory_flush_;
    int last_memory_flush_compaction_count_ = -1;
};

} // namespace memory
} // namespace hunterclaw
