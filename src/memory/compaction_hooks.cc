#include "hunterclaw/memory/compaction_hooks.h"
#include "hunterclaw/memory/compaction.h"
#include <spdlog/spdlog.h>
#include <random>
#include <iomanip>
#include <sstream>

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
// CompactionHooks Implementation
// ============================================================================

void CompactionHooks::trigger_before_compaction(const CompactionHookEvent& event) const {
    for (const auto& callback : before_callbacks_) {
        try {
            callback(event);
        } catch (const std::exception& e) {
            log()->warn("[compaction-hooks] before_compaction hook failed: {}", e.what());
        }
    }
}

void CompactionHooks::trigger_after_compaction(const CompactionHookEvent& event) const {
    for (const auto& callback : after_callbacks_) {
        try {
            callback(event);
        } catch (const std::exception& e) {
            log()->warn("[compaction-hooks] after_compaction hook failed: {}", e.what());
        }
    }
}

// ============================================================================
// CompactionDiagnostics Implementation
// ============================================================================

std::string CompactionDiagnostics::generate_diag_id() {
    // 对齐 OpenClaw createCompactionDiagId: `cmp-${Date.now().toString(36)}-${token}`
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
    
    // Base36 conversion (simplified)
    std::ostringstream oss;
    oss << "cmp-" << std::hex << ms << "-";
    
    // Random token (4 chars)
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 35);
    
    const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    for (int i = 0; i < 4; ++i) {
        oss << chars[dis(gen)];
    }
    
    return oss.str();
}

CompactionMetrics CompactionDiagnostics::summarize_messages(
    const std::vector<AgentMessage>& messages
) {
    // 严格对齐 OpenClaw summarizeCompactionMessages (compact.ts:199-230)
    
    CompactionMetrics metrics;
    metrics.messages_count = static_cast<int>(messages.size());
    
    std::vector<CompactionMetrics::Contributor> contributors;
    
    for (const auto& msg : messages) {
        int chars = static_cast<int>(msg.content.length());
        metrics.history_text_chars += chars;
        
        if (msg.role == "toolResult") {
            metrics.tool_result_chars += chars;
        }
        
        // 估算 tokens (4 chars ≈ 1 token)
        metrics.est_tokens += chars / 4 + 10;  // +10 for metadata
        
        // 记录 contributor
        CompactionMetrics::Contributor contrib;
        contrib.role = msg.role;
        contrib.chars = chars;
        if (msg.name) {
            contrib.tool_name = *msg.name;
        }
        contributors.push_back(contrib);
    }
    
    // Top 3 contributors (对齐 OpenClaw: toSorted + slice(0, 3))
    std::sort(contributors.begin(), contributors.end(), 
              [](const auto& a, const auto& b) { return a.chars > b.chars; });
    
    metrics.top_contributors.clear();
    for (size_t i = 0; i < std::min(size_t(3), contributors.size()); ++i) {
        metrics.top_contributors.push_back(contributors[i]);
    }
    
    return metrics;
}

std::string CompactionDiagnostics::classify_reason(const std::string& reason) {
    // 严格对齐 OpenClaw classifyCompactionReason (compact.ts:232-261)
    
    std::string lower = reason;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower.empty()) {
        return "unknown";
    }
    if (lower.find("nothing to compact") != std::string::npos) {
        return "no_compactable_entries";
    }
    if (lower.find("below threshold") != std::string::npos) {
        return "below_threshold";
    }
    if (lower.find("already compacted") != std::string::npos) {
        return "already_compacted_recently";
    }
    if (lower.find("still exceeds target") != std::string::npos) {
        return "live_context_still_exceeds_target";
    }
    if (lower.find("guard") != std::string::npos) {
        return "guard_reject";
    }
    if (lower.find("timeout") != std::string::npos) {
        return "timeout";
    }
    if (lower.find("error") != std::string::npos || lower.find("fail") != std::string::npos) {
        return "error";
    }
    
    return "other";
}

void CompactionDiagnostics::log_compaction_start(
    const std::string& diag_id,
    const std::string& session_key,
    const std::string& trigger,
    const std::string& provider,
    const std::string& model_id,
    int attempt,
    int max_attempts,
    const CompactionMetrics& pre_metrics
) {
    // 对齐 OpenClaw [compaction-diag] start (compact.ts:881-889)
    
    log()->debug(
        "[compaction-diag] start sessionKey={} diagId={} trigger={} provider={}/{} "
        "attempt={} maxAttempts={} pre.messages={} pre.historyTextChars={} "
        "pre.toolResultChars={} pre.estTokens={}",
        session_key, diag_id, trigger, provider, model_id,
        attempt, max_attempts,
        pre_metrics.messages_count, 
        pre_metrics.history_text_chars,
        pre_metrics.tool_result_chars,
        pre_metrics.est_tokens
    );
    
    // Log top contributors
    if (!pre_metrics.top_contributors.empty()) {
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < pre_metrics.top_contributors.size(); ++i) {
            const auto& c = pre_metrics.top_contributors[i];
            if (i > 0) oss << ",";
            oss << "{\"role\":\"" << c.role << "\",\"chars\":" << c.chars;
            if (!c.tool_name.empty()) {
                oss << ",\"tool\":\"" << c.tool_name << "\"";
            }
            oss << "}";
        }
        oss << "]";
        
        log()->debug("[compaction-diag] contributors diagId={} top={}", 
                    diag_id, oss.str());
    }
}

void CompactionDiagnostics::log_compaction_end(
    const std::string& diag_id,
    const std::string& session_key,
    const std::string& trigger,
    const std::string& provider,
    const std::string& model_id,
    int attempt,
    int max_attempts,
    const std::string& outcome,
    const std::string& reason,
    int64_t duration_ms,
    bool retrying,
    const CompactionMetrics& pre_metrics,
    const CompactionMetrics& post_metrics
) {
    // 对齐 OpenClaw [compaction-diag] end (compact.ts:951-963)
    
    log()->debug(
        "[compaction-diag] end sessionKey={} diagId={} trigger={} provider={}/{} "
        "attempt={} maxAttempts={} outcome={} reason={} durationMs={} retrying={} "
        "post.messages={} post.historyTextChars={} post.toolResultChars={} post.estTokens={} "
        "delta.messages={} delta.historyTextChars={} delta.toolResultChars={} delta.estTokens={}",
        session_key, diag_id, trigger, provider, model_id,
        attempt, max_attempts, outcome, reason, duration_ms, retrying,
        post_metrics.messages_count,
        post_metrics.history_text_chars,
        post_metrics.tool_result_chars,
        post_metrics.est_tokens,
        post_metrics.messages_count - pre_metrics.messages_count,
        post_metrics.history_text_chars - pre_metrics.history_text_chars,
        post_metrics.tool_result_chars - pre_metrics.tool_result_chars,
        post_metrics.est_tokens - pre_metrics.est_tokens
    );
}

} // namespace memory
} // namespace hunterclaw
