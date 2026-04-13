// Aligned with openclaw: src/auto-reply/reply/conversation-label-generator.ts (87行)
// 会话标签自动生成 — 使用 LLM 简单补全生成简短标签

#include "hunterclaw/auto_reply/conversation_label_generator.h"
#include "hunterclaw/llm/llm_client.h"
#include "hunterclaw/llm/model_selection.h"
#include "hunterclaw/config/config_system.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <sstream>
#include <thread>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// generateConversationLabel
// 对齐 OC: generateConversationLabel (conversation-label-generator.ts:25-86)
// ============================================================================

static constexpr int DEFAULT_MAX_LABEL_LENGTH = 128;
static constexpr int TIMEOUT_MS = 15000;

std::optional<std::string> generate_conversation_label(
    const ConversationLabelParams& params) {
    if (params.user_message.empty()) return std::nullopt;

    const int max_length = (params.max_length > 0) ? params.max_length : DEFAULT_MAX_LABEL_LENGTH;
    const int timeout_ms = (params.timeout_ms > 0) ? params.timeout_ms : TIMEOUT_MS;

    try {
        // Resolve model from config
        auto model_ref = llm::resolve_default_model_for_agent(
            params.config_json, params.agent_id);
        if (model_ref.model.empty()) return std::nullopt;

        // Build simple completion request
        llm::CompletionRequest req;
        req.model = model_ref.model;
        req.provider = model_ref.provider;
        req.max_tokens = 100;
        req.temperature = 0.3;
        req.timeout_ms = timeout_ms;
        req.messages = {{
            "user",
            params.prompt + "\n\n" + params.user_message
        }};

        // Execute simple completion (blocking, with timeout)
        auto client = llm::LLMClient::create_from_config(params.config_json);
        if (!client) return std::nullopt;

        auto result = client->complete(req);
        if (!result.ok || result.content.empty()) return std::nullopt;

        // Extract text
        std::string text = result.content;
        // Trim whitespace
        auto start = text.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return std::nullopt;
        auto end = text.find_last_not_of(" \t\n\r");
        text = text.substr(start, end - start + 1);

        if (text.empty()) return std::nullopt;

        // Truncate to max_length
        if (static_cast<int>(text.size()) > max_length) {
            text = text.substr(0, max_length);
        }
        return text;
    } catch (...) {
        return std::nullopt;
    }
}

// ============================================================================
// Async version
// ============================================================================

void generate_conversation_label_async(
    const ConversationLabelParams& params,
    ConversationLabelCallback callback) {
    // Fire-and-forget on a detached thread (like OC's Promise-based approach)
    std::thread([params, callback]() {
        auto result = generate_conversation_label(params);
        if (callback) {
            callback(result);
        }
    }).detach();
}

}  // namespace auto_reply
}  // namespace hunterclaw
