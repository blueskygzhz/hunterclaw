#pragma once

// message_tool.h
// Strictly aligned with OpenClaw: src/agents/tools/message-tool.ts
//
// Tool: message
// Actions: send | sendWithEffect | sendAttachment | reply | thread-reply |
//          broadcast | delete | react | pin | unpin | fetch | poll |
//          thread-create | thread-list | event-create | event-list |
//          event-delete | ban | kick | timeout | unban | role-add |
//          role-remove | channel-create | channel-delete | channel-edit |
//          presence | sticker | search | and more...

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/types.h"
#include <memory>
#include <string>
#include <optional>
#include <vector>

namespace hunterclaw {
namespace tools {

/**
 * @brief Options for message tool (aligned with OpenClaw createMessageTool options)
 */
struct MessageToolOptions {
    // Current agent account ID
    std::string agent_account_id;

    // Current agent session key
    std::string agent_session_key;

    // Current channel context
    std::string current_channel_id;
    std::string current_channel_provider;  // telegram | discord | slack | etc.
    std::string current_thread_ts;
    std::string current_message_id;

    // Reply mode: off | first | all
    std::string reply_to_mode;

    // Whether to require explicit target for send actions
    bool require_explicit_target = false;

    // Sandbox root for file access
    std::string sandbox_root;

    // Requester sender ID
    std::string requester_sender_id;

    // Config reference
    const core::Config* config = nullptr;
};

/**
 * @brief Create message tool
 *
 * Aligned with OpenClaw: createMessageTool(options?)
 *
 * Provides: send, delete, react, poll, pin, threads, events, moderation,
 *           channel management, presence, sticker, search, and more.
 */
std::shared_ptr<Tool> create_message_tool(const MessageToolOptions& opts = {});

} // namespace tools
} // namespace hunterclaw
