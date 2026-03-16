#pragma once

#include "hunterclaw/memory/session_memory_provider.h"
#include "hunterclaw/core/result.h"
#include "hunterclaw/core/logging.h"
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace auto_reply {

using json = nlohmann::json;

// ========================================
// Message Structure
// 用于 delta 触发的消息结构
// ========================================

struct Message {
    std::string role;           // "user" | "assistant" | "system"
    json content;               // string | array of content blocks
    std::optional<std::string> agent_id;
};

// ========================================
// DeltaTrigger
// 严格对齐 OpenClaw 的 delta 触发机制
// 参考: src/memory/manager-sync-ops.ts
// ========================================

class DeltaTrigger {
public:
    explicit DeltaTrigger(
        std::shared_ptr<memory::SessionMemoryProvider> session_provider
    );
    
    // 批量触发 delta 更新
    // 对齐: processSessionDeltaBatch + updateSessionDelta
    core::Result<void> trigger_on_message_batch(
        const std::string& session_key,
        const std::vector<Message>& messages
    );
    
    // 单条消息触发
    core::Result<void> trigger_on_single_message(
        const std::string& session_key,
        const std::string& role,
        const json& content,
        const std::optional<std::string>& agent_id = std::nullopt
    );

private:
    std::shared_ptr<memory::SessionMemoryProvider> session_provider_;
    std::shared_ptr<spdlog::logger> logger_;
};

// ========================================
// Integration Hook
// 在 auto-reply 流程中集成 delta 触发
// ========================================

// 在回复完成后触发 delta 更新
// 对齐 OpenClaw: 在 auto-reply/reply/index.ts 中集成
core::Result<void> trigger_delta_after_reply(
    const std::string& session_key,
    const std::string& user_message,
    const std::string& assistant_message,
    std::shared_ptr<memory::SessionMemoryProvider> session_provider
);

} // namespace auto_reply
} // namespace hunterclaw
