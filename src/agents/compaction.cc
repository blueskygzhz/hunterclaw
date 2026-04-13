// compaction.cc
// Aligned with openclaw:
//   src/agents/compaction.ts  (compactHistory / splitMessagesByTokenShare / buildCompactionSummarizationInstructions)
//
// Provides:
//   - CompactionConfig
//   - estimate_messages_tokens
//   - split_messages_by_token_share
//   - build_compaction_summarization_instructions
//   - compact_history
//   - CompactionResult

#include "hunterclaw/react/react_engine.h"
#include "hunterclaw/llm/llm_client.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <optional>
#include <numeric>

namespace hunterclaw {
namespace agents {

namespace {

auto log() {
    static auto logger = spdlog::get("compaction");
    if (!logger) logger = spdlog::stdout_logger_mt("compaction");
    return logger;
}

} // anonymous namespace

// ============================================================================
// Constants — 对齐 OpenClaw compaction.ts
// ============================================================================

// 🎯 对齐 OpenClaw: BASE_CHUNK_RATIO = 0.4
static constexpr double BASE_CHUNK_RATIO = 0.4;
// 🎯 对齐 OpenClaw: MIN_CHUNK_RATIO = 0.15
static constexpr double MIN_CHUNK_RATIO = 0.15;
// 🎯 对齐 OpenClaw: SAFETY_MARGIN = 1.2 (20% buffer for estimation inaccuracy)
static constexpr double SAFETY_MARGIN = 1.2;
static const std::string DEFAULT_SUMMARY_FALLBACK = "No prior history.";
static constexpr int DEFAULT_PARTS = 2;

// 🎯 对齐 OpenClaw: IDENTIFIER_PRESERVATION_INSTRUCTIONS
static const std::string IDENTIFIER_PRESERVATION_INSTRUCTIONS =
    "Preserve all opaque identifiers exactly as written (no shortening or reconstruction), "
    "including UUIDs, hashes, IDs, tokens, API keys, hostnames, IPs, ports, URLs, and file names.";

// 🎯 对齐 OpenClaw: MERGE_SUMMARIES_INSTRUCTIONS
static const std::string MERGE_SUMMARIES_INSTRUCTIONS =
    "Merge these partial summaries into a single cohesive summary.\n\n"
    "MUST PRESERVE:\n"
    "- Active tasks and their current status (in-progress, blocked, pending)\n"
    "- Batch operation progress (e.g., '5/17 items completed')\n"
    "- The last thing the user requested and what was being done about it\n"
    "- Decisions made and their rationale\n"
    "- TODOs, open questions, and constraints\n"
    "- Any commitments or follow-ups promised\n\n"
    "PRIORITIZE recent context over older history. The agent needs to know\n"
    "what it was doing, not just what was discussed.";

// ============================================================================
// CompactionConfig — 对齐 OpenClaw compaction params
// ============================================================================

struct CompactionConfig {
    int context_tokens = 200000;           // Context window size
    double base_chunk_ratio = BASE_CHUNK_RATIO;
    double min_chunk_ratio = MIN_CHUNK_RATIO;
    double safety_margin = SAFETY_MARGIN;
    int default_parts = DEFAULT_PARTS;
    std::string identifier_policy = "strict"; // "strict" | "custom" | "off"
    std::optional<std::string> custom_instructions;
};

// ============================================================================
// CompactionResult — 对齐 OpenClaw compaction result
// ============================================================================

struct CompactionResult {
    bool compacted = false;
    std::string summary;                    // Generated summary text
    int messages_before = 0;
    int messages_after = 0;
    int tokens_before = 0;
    int tokens_after = 0;
    int tokens_saved = 0;
    std::string reason;                     // Failure reason (if not compacted)
};

// ============================================================================
// 🎯 对齐 OpenClaw: estimateMessagesTokens(messages)
// Simple token estimation: ~4 chars per token (rough approximation)
// ============================================================================

int estimate_messages_tokens(const std::vector<react::LLMMessage>& messages) {
    int total = 0;
    for (const auto& msg : messages) {
        // Role overhead: ~4 tokens
        total += 4;
        // Content: ~4 chars per token
        total += static_cast<int>(msg.content.size() / 4);
        // Tool call overhead (approximate)
        // Note: tool_calls field may not be directly accessible on LLMMessage
        // Use content size as proxy for tool call overhead
    }
    return total;
}

// ============================================================================
// 🎯 对齐 OpenClaw: splitMessagesByTokenShare(messages, parts)
// Split messages into roughly equal token-share chunks,
// respecting tool call / tool result pairing boundaries.
// ============================================================================

std::vector<std::vector<react::LLMMessage>> split_messages_by_token_share(
    const std::vector<react::LLMMessage>& messages,
    int parts
) {
    if (messages.empty()) return {};

    int normalized_parts = std::max(1, std::min(parts, static_cast<int>(messages.size())));
    if (normalized_parts <= 1) return {messages};

    int total_tokens = estimate_messages_tokens(messages);
    int target_tokens = total_tokens / normalized_parts;

    std::vector<std::vector<react::LLMMessage>> chunks;
    std::vector<react::LLMMessage> current;
    int current_tokens = 0;

    for (const auto& msg : messages) {
        int msg_tokens = estimate_messages_tokens({msg});
        current.push_back(msg);
        current_tokens += msg_tokens;

        // Check if we should split (respecting tool call boundaries)
        bool at_safe_boundary = true;
        // Don't split in the middle of a tool call sequence
        if (msg.role == llm::LLMMessageRole::Assistant && !msg.tool_calls.empty()) {
            at_safe_boundary = false; // Next message should be tool result
        }
        if (msg.role == llm::LLMMessageRole::Tool) {
            at_safe_boundary = true; // Safe to split after tool result
        }

        if (current_tokens >= target_tokens &&
            at_safe_boundary &&
            static_cast<int>(chunks.size()) < normalized_parts - 1) {
            chunks.push_back(std::move(current));
            current = {};
            current_tokens = 0;
        }
    }

    if (!current.empty()) {
        chunks.push_back(std::move(current));
    }

    return chunks;
}

// ============================================================================
// 🎯 对齐 OpenClaw: buildCompactionSummarizationInstructions
// ============================================================================

std::optional<std::string> build_compaction_summarization_instructions(
    const std::optional<std::string>& custom_instructions,
    const std::string& identifier_policy
) {
    std::string identifier_part;
    if (identifier_policy == "off") {
        // No identifier preservation
    } else if (identifier_policy == "custom") {
        // Would use custom instructions if provided
        identifier_part = IDENTIFIER_PRESERVATION_INSTRUCTIONS;
    } else {
        // "strict" (default)
        identifier_part = IDENTIFIER_PRESERVATION_INSTRUCTIONS;
    }

    bool has_custom = custom_instructions.has_value() &&
                      !custom_instructions->empty();

    if (identifier_part.empty() && !has_custom) {
        return std::nullopt;
    }
    if (!has_custom) {
        return identifier_part;
    }
    if (identifier_part.empty()) {
        return "Additional focus:\n" + *custom_instructions;
    }
    return identifier_part + "\n\nAdditional focus:\n" + *custom_instructions;
}

// ============================================================================
// 🎯 对齐 OpenClaw: compact_history — 主压缩函数
// ============================================================================

CompactionResult compact_history(
    std::vector<react::LLMMessage>& messages,
    std::shared_ptr<llm::LLMClient> llm_client,
    const CompactionConfig& config
) {
    CompactionResult result;
    result.messages_before = static_cast<int>(messages.size());
    result.tokens_before = estimate_messages_tokens(messages);

    if (messages.size() <= 2) {
        result.reason = "Too few messages to compact";
        return result;
    }

    // 🎯 对齐 OpenClaw: determine target token count
    int max_tokens = static_cast<int>(config.context_tokens * config.base_chunk_ratio);
    int min_tokens = static_cast<int>(config.context_tokens * config.min_chunk_ratio);

    if (result.tokens_before <= max_tokens) {
        result.reason = "Messages within token budget (" +
            std::to_string(result.tokens_before) + " <= " +
            std::to_string(max_tokens) + ")";
        return result;
    }

    log()->info("compact_history: {} messages, {} tokens (max={}), starting compaction",
        messages.size(), result.tokens_before, max_tokens);

    // 🎯 对齐 OpenClaw: split into chunks and summarize
    auto chunks = split_messages_by_token_share(messages, config.default_parts);
    if (chunks.empty()) {
        result.reason = "No chunks produced";
        return result;
    }

    // Build summarization instructions
    auto summarize_instructions = build_compaction_summarization_instructions(
        config.custom_instructions, config.identifier_policy);

    // Keep the most recent chunk, summarize older chunks
    if (chunks.size() <= 1) {
        result.reason = "Only one chunk, cannot compact further";
        return result;
    }

    // Summarize all but the last chunk
    std::vector<std::string> summaries;
    for (size_t i = 0; i < chunks.size() - 1; ++i) {
        std::ostringstream chunk_text;
        for (const auto& msg : chunks[i]) {
            std::string role_str;
            switch (msg.role) {
                case llm::LLMMessageRole::User: role_str = "User"; break;
                case llm::LLMMessageRole::Assistant: role_str = "Assistant"; break;
                case llm::LLMMessageRole::System: role_str = "System"; break;
                case llm::LLMMessageRole::Tool: role_str = "Tool"; break;
            }
            chunk_text << "[" << role_str << "]: " << msg.content << "\n";
        }

        // 🎯 对齐 OpenClaw: generateSummary via LLM
        std::string summarize_prompt =
            "Summarize the following conversation chunk concisely, preserving key information.\n";
        if (summarize_instructions.has_value()) {
            summarize_prompt += *summarize_instructions + "\n";
        }
        summarize_prompt += "\n---\n" + chunk_text.str() + "\n---\n";
        summarize_prompt += "\nProvide a concise summary:";

        if (llm_client) {
            try {
                auto summary_result = llm_client->generate(summarize_prompt);
                if (summary_result.is_ok()) {
                    summaries.push_back(summary_result.value.value_or(DEFAULT_SUMMARY_FALLBACK));
                } else {
                    summaries.push_back(DEFAULT_SUMMARY_FALLBACK);
                    log()->warn("compact_history: summary generation failed for chunk {}: {}",
                        i, summary_result.error);
                }
            } catch (const std::exception& e) {
                summaries.push_back(DEFAULT_SUMMARY_FALLBACK);
                log()->warn("compact_history: summary exception for chunk {}: {}", i, e.what());
            }
        } else {
            // Fallback: simple truncation if no LLM client
            std::string truncated = chunk_text.str();
            if (truncated.size() > 500) {
                truncated = truncated.substr(0, 500) + "... [truncated]";
            }
            summaries.push_back(truncated);
        }
    }

    // 🎯 对齐 OpenClaw: merge summaries if multiple
    std::string merged_summary;
    if (summaries.size() == 1) {
        merged_summary = summaries[0];
    } else if (summaries.size() > 1) {
        if (llm_client) {
            std::ostringstream merge_prompt;
            merge_prompt << MERGE_SUMMARIES_INSTRUCTIONS << "\n\n";
            for (size_t i = 0; i < summaries.size(); ++i) {
                merge_prompt << "--- Summary " << (i + 1) << " ---\n"
                             << summaries[i] << "\n\n";
            }
            merge_prompt << "Merged summary:";

            try {
                auto merge_result = llm_client->generate(merge_prompt.str());
                if (merge_result.is_ok()) {
                    merged_summary = merge_result.value.value_or(summaries.back());
                } else {
                    merged_summary = summaries.back();
                }
            } catch (...) {
                merged_summary = summaries.back();
            }
        } else {
            // Concatenate summaries
            std::ostringstream ss;
            for (size_t i = 0; i < summaries.size(); ++i) {
                if (i > 0) ss << "\n\n";
                ss << summaries[i];
            }
            merged_summary = ss.str();
        }
    }

    // 🎯 对齐 OpenClaw: replace old messages with summary + recent chunk
    std::vector<react::LLMMessage> compacted;

    // Add summary as system message
    react::LLMMessage summary_msg;
    summary_msg.role = llm::LLMMessageRole::User;
    summary_msg.content = "[Conversation Summary]\n" + merged_summary;
    compacted.push_back(summary_msg);

    // Add the most recent chunk
    const auto& recent_chunk = chunks.back();
    for (const auto& msg : recent_chunk) {
        compacted.push_back(msg);
    }

    // Update messages in-place
    messages = std::move(compacted);

    result.compacted = true;
    result.summary = merged_summary;
    result.messages_after = static_cast<int>(messages.size());
    result.tokens_after = estimate_messages_tokens(messages);
    result.tokens_saved = result.tokens_before - result.tokens_after;

    log()->info("compact_history: compacted {} -> {} messages, {} -> {} tokens (saved {})",
        result.messages_before, result.messages_after,
        result.tokens_before, result.tokens_after, result.tokens_saved);

    return result;
}

} // namespace agents
} // namespace hunterclaw
