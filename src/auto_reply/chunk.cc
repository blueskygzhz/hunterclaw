// Aligned with openclaw: src/auto-reply/chunk.ts
// Reply text chunking for platform message limits.
// Supports "length" mode (split at limit) and "newline" mode (prefer paragraph breaks).

#include "hunterclaw/auto_reply/chunk.h"
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Constants (aligned with openclaw)
// ============================================================================

static const int DEFAULT_CHUNK_LIMIT = 4000;

// ============================================================================
// resolve_text_chunk_limit
// Aligned with openclaw: resolveTextChunkLimit (chunk.ts)
//
// Priority: channel account override → channel override → fallback (4000)
// ============================================================================

int resolve_text_chunk_limit(
    const ChunkConfig& config,
    const std::string& channel,
    const std::string& account_id,
    int fallback_limit
) {
    const int fallback = fallback_limit > 0 ? fallback_limit : DEFAULT_CHUNK_LIMIT;

    if (channel.empty()) return fallback;

    // Check channel-specific config
    auto ch_it = config.channels.find(channel);
    if (ch_it == config.channels.end()) return fallback;
    const auto& ch_config = ch_it->second;

    // Check account-specific override
    if (!account_id.empty()) {
        auto acc_it = ch_config.accounts.find(account_id);
        if (acc_it != ch_config.accounts.end() && acc_it->second.text_chunk_limit > 0) {
            return acc_it->second.text_chunk_limit;
        }
    }

    // Channel-level override
    if (ch_config.text_chunk_limit > 0) return ch_config.text_chunk_limit;

    return fallback;
}

// ============================================================================
// resolve_chunk_mode
// Aligned with openclaw: resolveChunkMode (chunk.ts)
// ============================================================================

ChunkMode resolve_chunk_mode(
    const ChunkConfig& config,
    const std::string& channel,
    const std::string& account_id
) {
    if (channel.empty()) return ChunkMode::Length;

    auto ch_it = config.channels.find(channel);
    if (ch_it == config.channels.end()) return ChunkMode::Length;
    const auto& ch_config = ch_it->second;

    // Account-specific override
    if (!account_id.empty()) {
        auto acc_it = ch_config.accounts.find(account_id);
        if (acc_it != ch_config.accounts.end() && acc_it->second.chunk_mode.has_value()) {
            return *acc_it->second.chunk_mode;
        }
    }

    return ch_config.chunk_mode.value_or(ChunkMode::Length);
}

// ============================================================================
// is_inside_fence
// Detect if position is inside a markdown code fence (``` or ~~~).
// Aligned with openclaw: findFenceSpanAt (markdown/fences.ts)
// ============================================================================

static bool is_inside_fence(const std::string& text, size_t pos) {
    // Simple fence detection: count opening ``` before pos
    int fence_count = 0;
    size_t i = 0;
    while (i < text.size() && i < pos) {
        if (text.substr(i, 3) == "```" || text.substr(i, 3) == "~~~") {
            fence_count++;
            i += 3;
        } else {
            ++i;
        }
    }
    return (fence_count % 2) == 1;  // odd count = inside fence
}

// ============================================================================
// find_best_break
// Find the best position to break text near the limit.
// Prefers: paragraph break (\n\n) > line break (\n) > space > hard limit.
// ============================================================================

static size_t find_best_break(const std::string& text, size_t limit, ChunkMode mode) {
    if (text.size() <= limit) return text.size();

    // In "newline" mode, prefer paragraph boundaries (blank lines)
    if (mode == ChunkMode::Newline) {
        // Look for \n\n within the limit
        size_t para_break = text.rfind("\n\n", limit);
        if (para_break != std::string::npos && para_break > limit / 4) {
            // Don't break inside a code fence
            if (!is_inside_fence(text, para_break)) {
                return para_break + 2;  // include the \n\n
            }
        }
    }

    // Look for \n near the limit (prefer breaking on line boundary)
    size_t line_break = text.rfind('\n', limit);
    if (line_break != std::string::npos && line_break > limit / 2) {
        if (!is_inside_fence(text, line_break)) {
            return line_break + 1;
        }
    }

    // Look for space near the limit
    size_t space_break = text.rfind(' ', limit);
    if (space_break != std::string::npos && space_break > limit * 3 / 4) {
        return space_break + 1;
    }

    // Hard break at limit
    return limit;
}

// ============================================================================
// chunk_reply_text
// Aligned with openclaw: chunkText / chunkTextByBreakResolver (chunk.ts)
//
// Splits text into chunks that fit within the limit, respecting:
//   - Paragraph boundaries (in "newline" mode)
//   - Code fence boundaries
//   - Word boundaries (prefer space breaks)
// ============================================================================

std::vector<std::string> chunk_reply_text(
    const std::string& text,
    int limit,
    ChunkMode mode
) {
    if (limit <= 0) limit = DEFAULT_CHUNK_LIMIT;

    std::vector<std::string> chunks;

    if (text.empty()) return chunks;

    if (static_cast<int>(text.size()) <= limit) {
        chunks.push_back(text);
        return chunks;
    }

    std::string remaining = text;
    bool in_fence = false;
    std::string fence_marker;

    while (!remaining.empty()) {
        if (static_cast<int>(remaining.size()) <= limit) {
            chunks.push_back(remaining);
            break;
        }

        size_t break_pos = find_best_break(remaining, static_cast<size_t>(limit), mode);

        // Check if we're breaking inside a code fence
        std::string chunk = remaining.substr(0, break_pos);
        remaining = remaining.substr(break_pos);

        // Count fences in this chunk to track state
        size_t fence_count = 0;
        size_t pos = 0;
        while ((pos = chunk.find("```", pos)) != std::string::npos) {
            fence_count++;
            pos += 3;
        }

        if (in_fence) {
            // We were inside a fence — prepend fence opener
            chunk = "```\n" + chunk;
        }

        if (fence_count % 2 == 1) {
            // Odd number of fences → we're now inside/outside a fence
            in_fence = !in_fence;
            if (in_fence) {
                // Close the fence at end of chunk
                chunk += "\n```";
            }
        }

        // Trim trailing whitespace from chunk
        size_t e = chunk.find_last_not_of(" \t\r\n");
        if (e != std::string::npos) {
            chunk = chunk.substr(0, e + 1);
        }

        if (!chunk.empty()) {
            chunks.push_back(chunk);
        }

        // Trim leading whitespace from remaining
        size_t s = remaining.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) {
            remaining.clear();
        } else if (s > 0) {
            remaining = remaining.substr(s);
        }
    }

    return chunks;
}

} // namespace auto_reply
} // namespace hunterclaw
