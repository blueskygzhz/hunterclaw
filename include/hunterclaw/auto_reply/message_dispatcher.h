#pragma once

#include "hunterclaw/channels/message.h"
#include "hunterclaw/auto_reply/auto_reply_config.h"
#include "hunterclaw/react/react_engine.h"
#include "hunterclaw/sessions/session_manager.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <functional>
#include <chrono>

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
namespace agents { class AgentPool; }  // ✅ 对齐 OpenClaw: ACP dispatch 需要 agent_pool
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
// ✅ 严格对齐 OpenClaw: src/auto-reply/reply/queue/types.ts
// Queue mode 类型定义
// ═══════════════════════════════════════════════════════════════

// Aligned with openclaw: QueueMode (queue/types.ts)
// "steer"        → inject into running session (queueEmbeddedPiMessage)
// "followup"     → serial: one-by-one after current run completes
// "collect"      → batch: merge all queued into one prompt
// "steer-backlog"→ steer first, fallback to followup if not streaming
// "interrupt"    → abort current run + clear lane, then run-now
// "queue"        → alias for "steer" (normalized)
enum class QueueMode {
    Steer,
    Followup,
    Collect,
    SteerBacklog,
    Interrupt,
    Queue,  // alias → Steer
};

// Aligned with openclaw: QueueDropPolicy (queue/types.ts)
enum class QueueDropPolicy {
    Old,       // drop oldest items
    New,       // drop newest (reject new enqueue)
    Summarize, // drop oldest but keep summary lines
};

// Aligned with openclaw: QueueSettings (queue/types.ts)
struct QueueSettings {
    QueueMode mode = QueueMode::Collect;  // default: collect
    int debounce_ms = 1000;               // DEFAULT_QUEUE_DEBOUNCE_MS
    int cap = 20;                         // DEFAULT_QUEUE_CAP
    QueueDropPolicy drop_policy = QueueDropPolicy::Summarize; // DEFAULT_QUEUE_DROP
};

// Aligned with openclaw: ActiveRunQueueAction (queue-policy.ts)
enum class ActiveRunQueueAction {
    RunNow,
    EnqueueFollowup,
    Drop,
};

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

    // ✅ run_reply_agent (aligned with openclaw: runReplyAgent agent-runner.ts)
    // 决策: run-now (session 空闲) 或 enqueue-followup (session 忙碌)
    // 支持 steer/interrupt/collect/followup 四种模式
    // PUBLIC: 供 chat.send 路径直接调用，与 auto-reply 路径共享同一套 active_sessions_ 状态
    struct RunReplyAgentParams {
        std::string session_key;
        std::function<DispatchResult()> run_fn;  // 实际执行函数
        std::string label;
        std::string prompt;          // for collect mode
        std::string summary_line;    // for drop policy
        QueueSettings queue_settings;
        bool is_heartbeat = false;
        bool is_streaming = false;   // aligned with openclaw: isEmbeddedPiRunStreaming
        // ✅ 对齐 OpenClaw 双层 lane 机制:
        // OpenClaw runEmbeddedPiAgent 内部自己做 session lane → main lane 嵌套
        // HunterClaw: 部分调用方 (chat.cc/agent.cc) 已在外部包了 session lane，
        //             此时 run_reply_agent 不再重复进 session lane (避免死锁)
        //             auto_reply 路径未包 session lane，run_reply_agent 需自行包装
        bool in_session_lane = false;  // true = 调用方已在 session lane 中
    };
    DispatchResult run_reply_agent(const RunReplyAgentParams& params);

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
        const channels::InboundMessage& message,
        const std::string& delivery_to = ""
    );
    // Legacy fallback when AcpSessionManager not initialized
    AcpDispatchResult try_dispatch_acp_reply_legacy(
        const channels::InboundMessage& message,
        const std::string& delivery_to = ""
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

    // ✅ Session active 状态跟踪 (aligned with openclaw: ACTIVE_EMBEDDED_RUNS Map)
    // 记录哪些 session 当前正在运行 LLM 推理，用于 run-now vs enqueue-followup 决策
    // 严格对齐 OpenClaw: abort 不从 map 中移除，cleanup 由 run 的 finally 完成
    // 防误删保护由 active_engines_ 的 handle 指针匹配实现 (≈ OpenClaw handle === 检查)
    mutable std::mutex session_state_mutex_;
    std::unordered_set<std::string> active_sessions_;
    // 记录哪些 session 当前正在 streaming（对齐 openclaw: isEmbeddedPiRunStreaming）
    std::unordered_set<std::string> streaming_sessions_;

    // ✅ FollowupQueue (aligned with openclaw: queue/state.ts FollowupQueueState)
    // 当 session 正在运行时，新请求入队等待；drain 时按 mode 消费
    // Key: session_key → queue state
    struct FollowupTask {
        std::function<DispatchResult()> run_fn;  // 实际执行函数
        std::string label;
        std::string prompt;          // for collect mode merging
        std::string summary_line;    // for drop policy summarize
        int64_t enqueued_at_ms = 0;
    };
    struct FollowupQueueState {
        std::vector<FollowupTask> items;
        bool draining = false;
        int64_t last_enqueued_at_ms = 0;
        QueueMode mode = QueueMode::Collect;
        int debounce_ms = 1000;
        int cap = 20;
        QueueDropPolicy drop_policy = QueueDropPolicy::Summarize;
        int dropped_count = 0;
        std::vector<std::string> summary_lines;
        int consecutive_failures = 0;
    };
    std::unordered_map<std::string, FollowupQueueState> followup_queues_;

    // ✅ Steer queue (aligned with openclaw: queueEmbeddedPiMessage)
    // 向运行中的 session 注入消息（steer 模式）
    // Key: session_key → pending steer messages
    // 注: 仅用于 fallback（engine 不可用时暂存）; 优先通过 active_engines_ 直接 steer
    std::unordered_map<std::string, std::vector<std::string>> steer_queues_;

    // ✅ Active ReactEngine 引用 (对齐 OpenClaw: ACTIVE_EMBEDDED_RUNS Map)
    // Key: session_key → 正在运行的 ReactEngine 实例
    // steer_embedded_session 通过此 map 直接调用 engine->steer(text)
    // 由 run_fn 中 on_engine_created 回调注册，run_fn 完成后清除
    std::unordered_map<std::string, std::shared_ptr<react::ReactEngine>> active_engines_;

    // ✅ Abort 信号集 (对齐 OpenClaw: abortEmbeddedPiRun 时序保证)
    // 当 abort_embedded_session 被调用但 active_engines_ 中没有 engine 时
    // (engine 还在 main lane queue 中等待创建)，将 session_key 记录到此 set。
    // on_engine_created 回调检查此 set，如果命中则立即调用 engine->cancel()，
    // 确保 abort 信号不会因为 engine 尚未创建而丢失。
    std::unordered_set<std::string> aborted_sessions_;

    // ✅ resolve_queue_settings (aligned with openclaw: resolveQueueSettings queue/settings.ts)
    // 从 session entry 和 config 解析 queue mode
    QueueSettings resolve_queue_settings(
        const std::optional<sessions::SessionEntry>& session_entry,
        const std::string& channel
    ) const;

    // ✅ resolve_active_run_queue_action (aligned with openclaw: resolveActiveRunQueueAction queue-policy.ts)
    // 决定 run-now / enqueue-followup / drop
    ActiveRunQueueAction resolve_active_run_queue_action(
        bool is_active,
        bool is_heartbeat,
        bool should_followup,
        QueueMode queue_mode
    ) const;

    // ✅ steer_embedded_session (aligned with openclaw: queueEmbeddedPiMessage)
    // 向运行中的 session 注入 steer 消息
    bool steer_embedded_session(const std::string& session_key, const std::string& message);

    // ✅ abort_embedded_session (aligned with openclaw: abortEmbeddedPiRun)
    // 中止运行中的 session（interrupt 模式）
    bool abort_embedded_session(const std::string& session_key);

private:
    // 按 mode 消费 followup 队列：followup=逐条串行，collect=批量合并
    void drain_followup_queue(const std::string& session_key);

    // ✅ apply_queue_drop_policy (aligned with openclaw: applyQueueDropPolicy queue-helpers.ts)
    // 当队列超过 cap 时，按 drop_policy 处理
    // Returns true if item should be enqueued, false if dropped
    bool apply_queue_drop_policy(FollowupQueueState& queue, const std::string& summary_text);

    // ✅ build_collect_prompt (aligned with openclaw: buildCollectPrompt queue-helpers.ts)
    // 将多条消息合并为一条 collect prompt
    std::string build_collect_prompt(
        const std::vector<FollowupTask>& items,
        const std::string& summary_prompt
    ) const;

    // 组件依赖
    std::shared_ptr<::hunterclaw::routing::MessageRouter> router_;
    std::shared_ptr<sessions::SessionManager> session_manager_;
    std::shared_ptr<::hunterclaw::queue::DeliveryQueue> delivery_queue_;
    std::shared_ptr<tools::ToolManager> tool_manager_;
    std::shared_ptr<llm::LLMClient> llm_client_;
    // ✅ 对齐 OpenClaw: ACP dispatch 需要 agent_pool（try_dispatch_acp_reply 路由消息到 subagent）
    std::shared_ptr<::hunterclaw::agents::AgentPool> agent_pool_;
    
    DispatchConfig config_;

public:
    // ✅ 对齐 OpenClaw: 注入 agent_pool（在 Gateway 初始化后调用）
    // 用于 try_dispatch_acp_reply 通过 agent_pool 找到 ACP session 对应的 agent
    void set_agent_pool(std::shared_ptr<::hunterclaw::agents::AgentPool> agent_pool) {
        agent_pool_ = agent_pool;
    }
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

        // Aligned with openclaw: onBlockReply callback
        // Called for each intermediate block reply during streaming.
        // Receives the block text; caller accumulates for TTS and delivers.
        std::function<void(const std::string& block_text)> on_block_reply;

        // Aligned with openclaw: onToolResult callback
        // Called after each tool execution with the tool result payload.
        std::function<void(const std::string& tool_text)> on_tool_result;

        // ✅ 对齐 OpenClaw: engine 生命周期回调
        // run_prepared_reply 创建 ReactEngine 后调用此回调，
        // 使调用方（MessageDispatcher）能获取 engine 引用用于 steer 注入。
        // 对齐 OpenClaw: setActiveEmbeddedRun(sessionId, queueHandle)
        std::function<void(std::shared_ptr<react::ReactEngine> engine)> on_engine_created;
    };
    
    // ✅ 生成结果
    struct GenerateResult {
        bool success = false;
        std::string response;
        react::ReactResult react_result;
        std::string error;

        // Aligned with openclaw: runId (generated per-run in runEmbeddedPiAgent)
        // Generated via generate_uuid() in run_prepared_reply.
        // Allows callers (e.g. channel inbound path) to correlate events.
        std::string run_id;

        // Aligned with openclaw: ctx.AcpDispatchTailAfterReset
        // Set to true when command handling prepared a trailing prompt after ACP in-place reset.
        bool acp_dispatch_tail_after_reset = false;
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
