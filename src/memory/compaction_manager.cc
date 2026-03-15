#include "hunterclaw/memory/compaction_manager.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>

namespace hunterclaw {
namespace memory {

namespace {
    auto log() {
        auto logger = spdlog::get("memory");
        if (!logger) {
            // 创建默认的 memory logger
            logger = spdlog::stdout_logger_mt("memory");
        }
        return logger;
    }
}

// ============================================================================
// CompactionManager Implementation (严格对齐 OpenClaw)
// ============================================================================

CompactionManager::CompactionManager(
    const CompactionConfig& config,
    std::shared_ptr<hunterclaw::llm::LLMClient> llm_client,
    std::shared_ptr<MemoryProvider> memory_provider
) : config_(config),
    compactor_(config, llm_client),
    flusher_(MemoryFlusher::FlushConfig()),  // 使用默认配置
    memory_provider_(memory_provider),
    stats_() {}

bool CompactionManager::should_compact(
    const std::vector<AgentMessage>& messages,
    int current_token_count,
    int context_window_tokens
) const {
    // 严格对齐 OpenClaw compact.ts:892-901
    // 1. 检查是否有真实对话内容
    bool has_real_content = false;
    for (const auto& msg : messages) {
        if (msg.role == "user" || msg.role == "assistant" || msg.role == "toolResult") {
            has_real_content = true;
            break;
        }
    }
    
    if (!has_real_content) {
        log()->info("[compaction] skipping — no real conversation messages");
        return false;
    }
    
    // 2. 检查 token 阈值 (对齐 OpenClaw shouldRunMemoryFlush)
    if (!config_.auto_compress) {
        return false;
    }
    
    int reserve_tokens = config_.reserve_tokens_floor;
    int soft_threshold = config_.soft_threshold_tokens;
    int threshold = std::max(0, context_window_tokens - reserve_tokens - soft_threshold);
    
    if (threshold <= 0) {
        return false;
    }
    
    return current_token_count >= threshold;
}

core::Result<void> CompactionManager::trigger_pre_compaction_flush(
    const PreCompactionFlushParams& params
) {
    // 严格对齐 OpenClaw memory-flush.ts
    
    if (!flusher_.config().enabled) {
        log()->debug("[compaction] memory flush disabled");
        return core::Result<void>::Ok();
    }
    
    // 1. 检查是否需要刷新 (token 阈值)
    bool token_threshold_met = flusher_.should_flush(
        params.current_token_count,
        params.context_window_tokens,
        config_.reserve_tokens_floor,
        params.transcript_size_bytes
    );
    
    if (!token_threshold_met) {
        log()->debug("[compaction] memory flush: below threshold");
        return core::Result<void>::Ok();
    }
    
    // 2. 检查是否已经为当前 compaction cycle 刷新过
    if (has_already_flushed_for_current_compaction(params.compaction_count)) {
        log()->debug("[compaction] memory flush: already flushed for current cycle");
        return core::Result<void>::Ok();
    }
    
    // 3. 执行 memory flush (通过回调)
    log()->info("[compaction] triggering pre-compaction memory flush");
    
    if (params.flush_callback) {
        auto result = params.flush_callback();
        if (!result.is_ok()) {
            log()->error("[compaction] memory flush failed: {}", result.error);
            return core::Result<void>::Err(result.error);
        }
    }
    
    // 4. 更新统计
    last_memory_flush_ = std::chrono::system_clock::now();
    last_memory_flush_compaction_count_ = params.compaction_count;
    stats_.total_memory_flushes++;
    
    log()->info("[compaction] pre-compaction memory flush completed");
    
    return core::Result<void>::Ok();
}

bool CompactionManager::has_already_flushed_for_current_compaction(
    int compaction_count
) const {
    // 严格对齐 OpenClaw memory-flush.ts:222-228
    return last_memory_flush_compaction_count_ == compaction_count;
}

core::Result<CompactionResult> CompactionManager::compact_session(
    const CompactionParams& params
) {
    // 严格对齐 OpenClaw compact.ts:compactEmbeddedPiSessionDirect
    
    CompactionResult result;
    result.compacted = false;
    
    // 1. 验证消息
    if (params.messages.empty()) {
        result.reason = "no messages to compact";
        return core::Result<CompactionResult>::Ok(result);
    }
    
    // 2. 检查是否需要压缩
    if (!should_compact(params.messages, params.current_token_count, params.context_window_tokens)) {
        result.reason = "below threshold";
        return core::Result<CompactionResult>::Ok(result);
    }
    
    // 3. Pre-compaction memory flush (对齐 OpenClaw)
    PreCompactionFlushParams flush_params;
    flush_params.current_token_count = params.current_token_count;
    flush_params.context_window_tokens = params.context_window_tokens;
    flush_params.transcript_size_bytes = params.transcript_size_bytes ? *params.transcript_size_bytes : 0;
    flush_params.compaction_count = params.compaction_count ? *params.compaction_count : stats_.total_compactions;
    flush_params.flush_callback = params.memory_flush_callback;
    
    auto flush_result = trigger_pre_compaction_flush(flush_params);
    if (!flush_result.is_ok()) {
        // 警告但不失败整个 compaction
        log()->warn("[compaction] pre-compaction flush failed: {}", flush_result.error);
    }
    
    // 4. 执行压缩 (summarize_in_stages)
    auto compact_start = std::chrono::system_clock::now();
    
    auto compact_result = compactor_.summarize_in_stages(
        params.messages,
        params.model_id,
        params.api_key
    );
    
    if (!compact_result.is_ok()) {
        result.reason = "compaction failed: " + compact_result.error;
        stats_.total_failures++;
        return core::Result<CompactionResult>::Ok(result);
    }
    
    // 5. 填充结果 (对齐 OpenClaw compact.ts:1009-1019)
    const auto& compaction_output = compact_result.value;
    
    result.compacted = true;
    result.summary = compaction_output.summary;
    result.compressed_messages = compaction_output.compressed_messages;
    result.tokens_before = compaction_output.stats.tokens_before;
    result.tokens_after = compaction_output.stats.tokens_after;
    result.messages_before = compaction_output.stats.messages_before;
    result.messages_after = compaction_output.stats.messages_after;
    result.tokens_saved = compaction_output.stats.tokens_saved;
    result.compression_ratio = compaction_output.stats.compression_ratio;
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now() - compact_start
    ).count();
    
    // 6. 更新统计
    stats_.total_compactions++;
    stats_.tokens_saved += result.tokens_saved;
    stats_.last_compaction = std::chrono::system_clock::now();
    
    // 7. Post-compaction sync (对齐 OpenClaw manager-sync-ops.ts:956-990)
    if (params.post_compaction_sync_callback) {
        auto sync_result = params.post_compaction_sync_callback(result);
        if (!sync_result.is_ok()) {
            log()->warn("[compaction] post-compaction sync failed: {}", sync_result.error);
        }
    }
    
    log()->info(
        "[compaction] completed: messages {}->{}, tokens {}->{} (saved: {}), ratio: {:.2f}x, duration: {}ms",
        result.messages_before,
        result.messages_after,
        result.tokens_before,
        result.tokens_after,
        result.tokens_saved,
        result.compression_ratio,
        result.duration_ms
    );
    
    return core::Result<CompactionResult>::Ok(result);
}

core::Result<void> CompactionManager::post_compaction_memory_sync(
    const std::vector<std::string>& session_files
) {
    // 严格对齐 OpenClaw manager-sync-ops.ts:953-990
    // Post-compaction refreshes should only update the explicit transcript files
    // and leave broader reindex/dirty-work decisions to the regular sync path
    
    if (!memory_provider_) {
        log()->warn("[compaction] no memory provider for post-compaction sync");
        return core::Result<void>::Ok();
    }
    
    if (session_files.empty()) {
        log()->debug("[compaction] no session files for post-compaction sync");
        return core::Result<void>::Ok();
    }
    
    log()->info("[compaction] triggering post-compaction memory sync for {} session files", 
               session_files.size());
    
    // 对齐 OpenClaw: syncSessionFiles with needsFullReindex=false
    // TODO: 实现 memory_provider 的 sync_session_files 接口
    // 暂时记录日志
    for (const auto& file : session_files) {
        log()->debug("[compaction] should sync session file: {}", file);
    }
    
    return core::Result<void>::Ok();
}

CompactionStats CompactionManager::get_stats() const {
    CompactionStats merged;
    merged.total_compactions = stats_.total_compactions;
    merged.total_memory_flushes = stats_.total_memory_flushes;
    merged.total_failures = stats_.total_failures;
    merged.tokens_saved = stats_.tokens_saved;
    merged.last_compaction = stats_.last_compaction;
    merged.last_memory_flush = last_memory_flush_;
    return merged;
}

void CompactionManager::reset_stats() {
    stats_ = InternalStats{};
    last_memory_flush_ = std::chrono::system_clock::time_point{};
    last_memory_flush_compaction_count_ = -1;
}

} // namespace memory
} // namespace hunterclaw
