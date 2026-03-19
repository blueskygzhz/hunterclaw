#pragma once

#include "hunterclaw/channels/message.h"
#include "hunterclaw/auto_reply/auto_reply_config.h"
#include "hunterclaw/react/react_engine.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <map>

namespace hunterclaw {
namespace auto_reply {

// Forward declarations
} // namespace auto_reply
} // namespace hunterclaw

// Forward declarations in their actual namespaces
namespace hunterclaw {
namespace llm { class LLMClient; }
namespace tools { class ToolManager; }
namespace sessions { class SessionManager; }
} // namespace hunterclaw

// 前置声明全局 queue 命名空间
namespace hunterclaw {
namespace queue { 
    class DeliveryQueue;
    struct MessagePayload;
    struct DeliveryTarget;
}
namespace routing { class MessageRouter; }
}

namespace hunterclaw {
namespace auto_reply {

// Bring real types into scope
namespace llm = ::hunterclaw::llm;
namespace tools = ::hunterclaw::tools;
namespace sessions = ::hunterclaw::sessions;

// ═══════════════════════════════════════════════════════════════
// ✅ 严格对齐 OpenClaw: src/auto-reply/dispatch.ts
// ✅ 以及 src/auto-reply/reply/dispatch-from-config.ts
// 消息分发器 (Message Dispatcher)
// ═══════════════════════════════════════════════════════════════

class MessageDispatcher {
public:
    // ✅ 分发结果
    struct DispatchResult {
        bool success = false;
        bool skipped = false;          // 是否跳过 (去重/Echo)
        bool denied = false;           // 是否拒绝 (权限)
        bool fast_aborted = false;     // 是否快速中止
        std::string skip_reason;       // 跳过原因
        std::string error;             // 错误信息
        
        // ReAct 执行结果
        react::ReactResult react_result;
        
        // 投递结果
        std::vector<std::string> delivered_message_ids;
        
        // 统计
        int64_t processing_time_ms = 0;
        int total_tokens = 0;
    };
    
    // ✅ 构造函数
    static std::shared_ptr<MessageDispatcher> create(
        std::shared_ptr<::hunterclaw::routing::MessageRouter> router,
        std::shared_ptr<sessions::SessionManager> session_manager,
        std::shared_ptr<::hunterclaw::queue::DeliveryQueue> delivery_queue,
        std::shared_ptr<tools::ToolManager> tool_manager,
        std::shared_ptr<llm::LLMClient> llm_client,
        const DispatchConfig& config
    );
    
    // ✅ 对齐 OpenClaw: dispatchInboundMessage (line 21-33)
    // 分发入站消息 (核心入口)
    DispatchResult dispatch_inbound_message(
        const channels::InboundMessage& message
    );
    
    // ✅ 对齐 OpenClaw: dispatchReplyFromConfig (line 50-610)
    // 从配置分发回复 (完整流程)
    DispatchResult dispatch_reply_from_config(
        const channels::InboundMessage& message,
        const AutoReplyConfig& config
    );
    
private:
    MessageDispatcher(
        std::shared_ptr<::hunterclaw::routing::MessageRouter> router,
        std::shared_ptr<sessions::SessionManager> session_manager,
        std::shared_ptr<::hunterclaw::queue::DeliveryQueue> delivery_queue,
        std::shared_ptr<tools::ToolManager> tool_manager,
        std::shared_ptr<llm::LLMClient> llm_client,
        const DispatchConfig& config
    );
    
    // ✅ 对齐 OpenClaw: shouldSkipDuplicateInbound (line 80-100)
    // 消息去重检测
    bool should_skip_duplicate_inbound(
        const channels::InboundMessage& message
    );
    
    // ✅ 对齐 OpenClaw: tryFastAbortFromMessage (line 120-150)
    // 快速中止检查
    struct FastAbortResult {
        bool should_abort = false;
        std::string reason;
    };
    FastAbortResult try_fast_abort_from_message(
        const channels::InboundMessage& message,
        const AutoReplyConfig& config
    );
    
    // ✅ 对齐 OpenClaw: tryDispatchAcpReply (line 180-220)
    // ACP (Agent Context Protocol) 调度
    struct AcpDispatchResult {
        bool handled = false;
        std::string response;
    };
    AcpDispatchResult try_dispatch_acp_reply(
        const channels::InboundMessage& message
    );
    
    // ✅ 权限检查
    struct PermissionCheckResult {
        bool allowed = false;
        std::string denial_reason;
    };
    PermissionCheckResult check_permissions(
        const channels::InboundMessage& message,
        const AutoReplyConfig& config
    );
    
    // ✅ 对齐 OpenClaw: maybeApplyTtsToPayload (line 550-580)
    // TTS (文本转语音) 处理
    void maybe_apply_tts_to_payload(
        std::vector<::hunterclaw::queue::MessagePayload>& payloads,
        const AutoReplyConfig& config
    );
    
    // ✅ 对齐 OpenClaw: shouldRouteToOriginating (line 320-340)
    // 判断是否路由到原始渠道
    bool should_route_to_originating(
        const channels::InboundMessage& message
    );
    
    // ✅ 去重缓存 (aligned with openclaw: inbound-dedupe.ts)
    // Key: provider|accountId|sessionScope|peerId|threadId|messageId
    struct DedupRecord {
        std::string dedupe_key;   // composite key (aligned with openclaw: buildInboundDedupeKey)
        int64_t timestamp_ms;
    };
    std::vector<DedupRecord> dedup_cache_;
    
    // 组件依赖
    std::shared_ptr<::hunterclaw::routing::MessageRouter> router_;
    std::shared_ptr<sessions::SessionManager> session_manager_;
    std::shared_ptr<::hunterclaw::queue::DeliveryQueue> delivery_queue_;
    std::shared_ptr<tools::ToolManager> tool_manager_;
    std::shared_ptr<llm::LLMClient> llm_client_;
    
    DispatchConfig config_;
};

// ═══════════════════════════════════════════════════════════════
// ✅ 严格对齐 OpenClaw: src/auto-reply/reply/get-reply.ts
// 回复生成器 (Reply Generator)
// ═══════════════════════════════════════════════════════════════

class ReplyGenerator {
public:
    // ✅ 生成配置
    struct GenerateConfig {
        std::string agent_id;
        std::string workspace_path;
        std::string session_key;
        std::string user_input;
        
        // 上下文配置
        bool enable_media_understanding = true;
        bool enable_link_understanding = true;
        bool enable_command_auth = true;
        
        // 模型覆盖
        std::optional<std::string> model_override;
        
        // 是否为心跳
        bool is_heartbeat = false;
    };
    
    // ✅ 生成结果
    struct GenerateResult {
        bool success = false;
        std::string response;
        react::ReactResult react_result;
        std::string error;
    };
    
    // ✅ 对齐 OpenClaw: getReplyFromConfig (line 50-403)
    // 从配置生成回复 (完整流程)
    static GenerateResult get_reply_from_config(
        const GenerateConfig& config,
        std::shared_ptr<llm::LLMClient> llm_client,
        std::shared_ptr<tools::ToolManager> tool_manager,
        std::shared_ptr<sessions::SessionManager> session_manager
    );
    
private:
    // ✅ 对齐 OpenClaw: applyMediaUnderstanding (line 150-180)
    // 应用媒体理解
    static void apply_media_understanding(
        std::string& prompt,
        const channels::InboundMessage& message
    );
    
    // ✅ 对齐 OpenClaw: applyLinkUnderstanding (line 190-220)
    // 应用链接理解
    static void apply_link_understanding(
        std::string& prompt,
        const channels::InboundMessage& message
    );
    
    // ✅ 对齐 OpenClaw: runPreparedReply (line 300-380)
    // 执行准备好的回复
    static GenerateResult run_prepared_reply(
        const GenerateConfig& config,
        const std::string& prepared_prompt,
        std::shared_ptr<llm::LLMClient> llm_client,
        std::shared_ptr<tools::ToolManager> tool_manager,
        std::shared_ptr<sessions::SessionManager> session_manager
    );
};

} // namespace auto_reply
} // namespace hunterclaw
