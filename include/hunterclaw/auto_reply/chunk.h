// Aligned with openclaw: src/auto-reply/chunk.ts
#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// ChunkMode (aligned with openclaw: ChunkMode)
// ============================================================================

enum class ChunkMode {
    Length,   // "length" — split only when exceeding limit (default)
    Newline   // "newline" — prefer breaking on paragraph boundaries
};

// ============================================================================
// ChunkConfig (aligned with openclaw channel chunk config)
// ============================================================================

struct ChannelChunkConfig {
    int text_chunk_limit = 0;
    std::optional<ChunkMode> chunk_mode;
    struct AccountOverride {
        int text_chunk_limit = 0;
        std::optional<ChunkMode> chunk_mode;
    };
    std::map<std::string, AccountOverride> accounts;
};

struct ChunkConfig {
    std::map<std::string, ChannelChunkConfig> channels;
};

// ============================================================================
// Functions
// ============================================================================

// Resolve text chunk limit for a channel/account
int resolve_text_chunk_limit(
    const ChunkConfig& config,
    const std::string& channel,
    const std::string& account_id = "",
    int fallback_limit = 4000
);

// Resolve chunk mode for a channel/account
ChunkMode resolve_chunk_mode(
    const ChunkConfig& config,
    const std::string& channel,
    const std::string& account_id = ""
);

// Split text into chunks respecting platform limits
std::vector<std::string> chunk_reply_text(
    const std::string& text,
    int limit = 4000,
    ChunkMode mode = ChunkMode::Length
);

} // namespace auto_reply
} // namespace hunterclaw
