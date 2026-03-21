#include "hunterclaw/auto_reply/delta_trigger.h"
#include "hunterclaw/memory/session_memory_provider.h"
#include "hunterclaw/memory/session_files.h"
#include "hunterclaw/core/logging.h"
#include <chrono>
#include <fstream>
#include <filesystem>

namespace hunterclaw {
namespace auto_reply {

// ========================================
// DeltaTrigger Implementation
// Aligns with OpenClaw session delta mechanism
// Reference: src/memory/manager-sync-ops.ts::updateSessionDelta
//
// OpenClaw writes session messages as JSONL entries to session files.
// There is no separate trigger_delta_update API — session state is tracked
// via sessionsDirty / sessionDeltas in MemoryIndexManager (manager.ts).
// ========================================

DeltaTrigger::DeltaTrigger(
    std::shared_ptr<memory::SessionMemoryProvider> session_provider
) : session_provider_(session_provider),
    logger_(core::get_logger("delta_trigger")) {}

core::Result<void> DeltaTrigger::trigger_on_message_batch(
    const std::string& session_key,
    const std::vector<Message>& messages
) {
    if (!session_provider_ || !session_provider_->config().enabled) {
        // Session memory not enabled, silently skip
        return core::Result<void>::Ok();
    }

    logger_->debug("Delta trigger: processing {} messages for session {}",
                   messages.size(), session_key);

    // Align with OpenClaw: session messages are appended as JSONL entries
    // to the session file. MemoryIndexManager picks them up on next sync.
    const auto& cfg = session_provider_->config();
    if (!cfg.export_dir.has_value()) {
        return core::Result<void>::Ok();
    }

    std::string session_file = cfg.export_dir.value() + "/" + session_key + ".jsonl";

    // Ensure directory exists
    std::filesystem::create_directories(cfg.export_dir.value());

    auto now = std::chrono::system_clock::now();
    auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    std::ofstream ofs(session_file, std::ios::app);
    if (!ofs.is_open()) {
        return core::Result<void>::Err("Failed to open session file: " + session_file);
    }

    for (const auto& msg : messages) {
        // Serialize each message as a JSONL entry (aligns with OpenClaw session format)
        nlohmann::json entry;
        entry["type"] = "message";
        entry["role"] = msg.role;
        entry["content"] = msg.content;
        entry["timestamp"] = timestamp_ms;
        if (msg.agent_id.has_value()) {
            entry["agentId"] = msg.agent_id.value();
        }
        ofs << entry.dump() << "\n";
    }

    return core::Result<void>::Ok();
}

core::Result<void> DeltaTrigger::trigger_on_single_message(
    const std::string& session_key,
    const std::string& role,
    const json& content,
    const std::optional<std::string>& agent_id
) {
    Message msg;
    msg.role = role;
    msg.content = content;
    msg.agent_id = agent_id;

    return trigger_on_message_batch(session_key, {msg});
}

// ========================================
// Integration Hook: called in auto-reply flow
// ========================================

core::Result<void> trigger_delta_after_reply(
    const std::string& session_key,
    const std::string& user_message,
    const std::string& assistant_message,
    std::shared_ptr<memory::SessionMemoryProvider> session_provider
) {
    if (!session_provider) {
        return core::Result<void>::Ok();
    }

    DeltaTrigger trigger(session_provider);

    // Align with OpenClaw: batch trigger user + assistant messages
    std::vector<Message> messages;

    Message user_msg;
    user_msg.role = "user";
    user_msg.content = user_message;
    messages.push_back(user_msg);

    Message assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = assistant_message;
    messages.push_back(assistant_msg);

    return trigger.trigger_on_message_batch(session_key, messages);
}

} // namespace auto_reply
} // namespace hunterclaw
