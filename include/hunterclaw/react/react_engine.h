#pragma once

#include "hunterclaw/core/json.h"
#include "hunterclaw/core/thread_pool.h"
#include "hunterclaw/tools/tool_manager.h"  // Full definition of ToolManager
#include "hunterclaw/llm/llm_client.h"
#include "hunterclaw/llm/provider.h"  // For Response, ToolCall, Usage
#include "hunterclaw/memory/compaction_manager.h"  // ✅ Compaction 支持
#include "hunterclaw/context/context_engine.h"    // ✅ Context Engine 支持
#include "hunterclaw/context/context_pruning.h"   // ✅ 对齐 OC: pi-hooks/context-pruning
#include "hunterclaw/react/loop_detection.h"       // ✅ 对齐 OpenClaw: tool loop detection
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <future>
#include <optional>
#include <mutex>
#include <queue>

namespace hunterclaw {
namespace react {

// 类型别名,方便使用
using LLMMessage = hunterclaw::llm::LLMMessage;
using LLMResponse = hunterclaw::llm::GenerateResponse;
using LLMToolCall = hunterclaw::llm::ToolCall;

// 对齐 OpenClaw 的 ReAct 模式实现
// 参考: openclaw-analysis/src/agents/pi-embedded-subscribe.ts

// ReAct 步骤类型
enum class StepType {
    Observe,     // 观察 (接收用户输入/工具结果)
    Reason,      // 推理 (LLM 思考)
    Act,         // 行动 (执行工具)
    Respond      // 响应 (回复用户)
};

// ReAct 步骤结果
struct ReactStep {
    StepType type = StepType::Observe;
    std::string content;                      // 步骤内容
    std::vector<std::string> tool_calls;      // 工具调用列表
    std::map<std::string, core::JsonValue> tool_results;  // 工具执行结果
    bool is_final = false;                    // 是否最终步骤
    int64_t timestamp_ms = 0;                 // 时间戳
    
    // 推理内容 (thinking tags)
    std::string reasoning;
    bool has_reasoning = false;
};

// ReAct 执行结果
struct ReactResult {
    bool success = false;
    std::string final_response;               // 最终响应
    std::vector<ReactStep> steps;             // 执行步骤
    std::string error;                        // 错误信息

    // 🎯 对齐 OpenClaw: assistantTexts — all text chunks emitted during the run
    // Mirrors subscribeEmbeddedPiSession().assistantTexts
    std::vector<std::string> assistant_texts;

    // ✅ 对齐 OC attempt.ts:2610-2660: messages_snapshot for context engine afterTurn
    std::vector<core::JsonValue> messages_snapshot;

    // 统计信息
    int total_steps = 0;
    int tool_calls = 0;
    int64_t duration_ms = 0;

    // Token 使用统计
    struct Usage {
        int64_t input_tokens = 0;
        int64_t output_tokens = 0;
        int64_t total_tokens = 0;
        int64_t cache_read_tokens = 0;
        int64_t cache_write_tokens = 0;
    } usage;
};

// 流式回调
struct StreamCallbacks {
    // 推理流 (reasoning stream)
    // 🎯 对齐 OpenClaw: onReasoningStream — called with { text, delta }
    std::function<void(const std::string& reasoning)> on_reasoning_stream;

    // 推理结束
    // 🎯 对齐 OpenClaw: onReasoningEnd
    std::function<void()> on_reasoning_end;

    // 助手输出流 (assistant delta)
    // 🎯 对齐 OpenClaw: onPartialReply / onAgentEvent stream="assistant"
    std::function<void(const std::string& text)> on_assistant_delta;

    // 助手消息开始
    // 🎯 对齐 OpenClaw: onAssistantMessageStart — earliest "writing" signal
    std::function<void()> on_assistant_message_start;

    // 块回复 (block reply)
    // 🎯 对齐 OpenClaw: onBlockReply — called at text_end or message_end
    std::function<void(const std::string& block)> on_block_reply;

    // 块回复 flush
    // 🎯 对齐 OpenClaw: onBlockReplyFlush — called at agent_end and before compaction wait
    // Ensures buffered block replies reach the channel before compaction blocks the run.
    std::function<void()> on_block_reply_flush;

    // 工具结果
    // 🎯 对齐 OpenClaw: onToolResult — verbose tool output
    std::function<void(const std::string& text)> on_tool_result;

    // 工具调用开始
    std::function<void(const std::string& tool_name, const core::JsonValue& args)> on_tool_start;

    // 工具调用完成
    std::function<void(const std::string& tool_name, const tools::ToolResult& result)> on_tool_end;

    // 步骤完成
    std::function<void(const ReactStep& step)> on_step;

    // 生命周期事件
    std::function<void()> on_start;
    std::function<void()> on_end;
    std::function<void(const std::string& error)> on_error;

    // 🎯 对齐 OpenClaw: onAgentEvent — structured stream events for WebSocket broadcast
    // stream: "tool" | "assistant" | "lifecycle" | "thinking" | "compaction"
    // data: JSON payload describing the event phase and details
    struct AgentEvent {
        std::string stream;    // "tool", "assistant", "lifecycle", "thinking", "compaction"
        core::JsonValue data;  // { phase: "start"|"update"|"result", name, toolCallId, ... }
    };
    std::function<void(const AgentEvent& event)> on_agent_event;

    // 🎯 OpenClaw 对齐: 用于直连推送的 session_id
    std::string session_id;

    // 🎯 OpenClaw 对齐: run_id (aligned with openclaw: runId in runEmbeddedPiAgent)
    // Generated per-run via generate_uuid(); used to correlate events across channels.
    // In HTTP API path: generated in chat.cc and passed via SSE events.
    // In Discord/channel path: generated in run_prepared_reply and stored in GenerateResult.
    std::string run_id;

    // 🎯 对齐 OpenClaw: hook context 字段 (attempt.ts:2685-2693)
    std::string agent_id;       // e.g. "default" or custom agent id
    std::string session_key;    // stable session key (survives /reset)
    std::string workspace_dir;  // agent workspace directory
    std::string trigger;        // "user", "heartbeat", "cron", "memory"
    std::string channel_id;     // "telegram", "discord", "whatsapp", etc.

    // 🎯 OpenClaw 对齐: transcript 路径 (用于 tool_result_persist hook)
    // 如果非空，工具结果将通过 append_tool_result_to_transcript 写入
    std::string transcript_path;

    // 🎯 OpenClaw 对齐: abortSignal (chat.send 传入的 per-run abort 信号)
    // OpenClaw: dispatchInboundMessage({ replyOptions: { abortSignal: abortController.signal } })
    // 当 chat.abort 被调用时，abort_signal->load() 变为 true。
    // execute_loop 在每步循环开始时检查此信号（除了 ReactEngine::cancelled_ 以外），
    // 使得 per-run-id 的精确 abort 能直接中断该 run 的执行，
    // 而不需要通过 session-level 的 abort_embedded_session。
    std::shared_ptr<std::atomic<bool>> abort_signal;

    // ✅ 对齐 OC agent-runner-memory.ts: memory flush 触发回调
    // engine run 结束后检查 memory_flush_gate, 如果超过阈值则通知上层执行 flush
    // 上层 (dispatcher/agent-runner) 负责调用 run_memory_flush_if_needed
    std::function<void(int total_tokens)> on_memory_flush_needed;

    // ════════════════════════════════════════════════════════════════════
    // 🎯 严格对齐 OpenClaw agent-runner-memory.ts 三大入口：
    //   runPreflightCompactionIfNeeded → runMemoryFlushIfNeeded
    // 由 engine 内部在 run() 的 pre/post 阶段调用。上层（dispatcher/agent_instance/
    // chat handler）通过下面的 function 字段注入实际执行逻辑。
    //
    // 可选字段 —— 不注入则跳过对应阶段（向后兼容）。
    // ════════════════════════════════════════════════════════════════════

    // 对齐 OC agent-runner-memory.ts L396-449: session 当前 totalTokens 的"新鲜值"
    // （从 session store 读取），engine 用它判断是否需要 preflight compaction
    std::optional<int> persisted_total_tokens;
    bool total_tokens_fresh = false;
    std::optional<int> compaction_count;
    std::optional<int> memory_flush_compaction_count;

    // 🎯 对齐 OC agent-runner-memory.ts：preflight compaction 执行路径
    //
    // OC 里 compactEmbeddedPiSession 是 preflight / /compact 命令 / token-overflow
    // 三条入口共用的唯一 compact 实现（通过 trigger 字段区分）。HC 对齐做法：
    // ReactEngine::run() 在 preflight 门控触发时直接调 ReactEngine::trigger_compaction
    // （其内部调 CompactionManager::compact_session），因此**不需要任何 compact
    // 回调注入**。CompactionManager 由 set_compaction_manager() 注入 engine 一次即可。
    //
    // （历史上曾有 compact_session_fn / on_compaction_complete_fn 两个空转回调——
    //  已移除，因为 OC 本身就没有这层 indirection。）

    // 对齐 OC agent-runner-memory.ts L693-848 memory flush callbacks:
    //   1) run_flush_agent_fn: 起 flush agent 子进程写 MEMORY.md
    //   2) persist_session_tokens_fn: 回写 transcript-derived token 到 session store
    //   3) on_flush_complete_fn: flush 完成后 session rotation (新 session_id)
    std::function<void(const std::string& prompt, const std::string& system_prompt)>
        run_flush_agent_fn;
    std::function<void(int derived_total_tokens)>
        persist_session_tokens_fn;
    std::function<void(int new_compaction_count,
                       const std::string& new_session_id)>
        on_flush_complete_fn;

    // Memory flush 执行路径额外需要的配置（对齐 OC agent-runner-memory.ts L564-620）：
    //   - flush_write_path: MEMORY.md 目标路径
    //   - flush_system_prompt / flush_prompt: flush agent 的 system/user prompt 模板
    //   - force_flush_transcript_bytes: transcript 超过多少字节强制 flush
    std::string flush_write_path;
    std::string flush_system_prompt;
    std::string flush_prompt;
    int64_t force_flush_transcript_bytes = 2 * 1024 * 1024;  // 默认 2MB
    bool memory_flush_writable = true;
    bool is_heartbeat = false;
    bool is_cli = false;
    std::string provider;  // 对齐 OC: 传给 flush agent 作为 context
    std::string model;     // 对齐 OC: 当前模型名
    std::string default_model;  // 对齐 OC: fallback 模型名
    std::optional<int> agent_cfg_context_tokens;
};

// ReAct 配置
struct ReactConfig {
    // 最大步骤数 (防止无限循环)
    int max_steps = 20;
    
    // 超时设置
    std::chrono::seconds step_timeout{60};
    std::chrono::seconds total_timeout{300};
    
    // 推理模式
    enum class ReasoningMode {
        Off,      // 不显示推理
        On,       // 显示推理
        Stream    // 流式推理
    } reasoning_mode = ReasoningMode::Off;
    
    // 块回复配置
    struct BlockReply {
        bool enabled = true;
        enum class BreakMode {
            TextEnd,     // 文本结束时
            MessageEnd   // 消息结束时
        } break_mode = BreakMode::TextEnd;
        
        // 分块设置
        int min_chars = 100;
        int max_chars = 1000;
        enum class BreakPreference {
            Paragraph,
            Newline,
            Sentence
        } break_preference = BreakPreference::Paragraph;
        
        // ✅ OpenClaw 对齐: Block streaming 控制
        // 参考: openclaw-analysis/src/web/auto-reply/monitor/process-message.ts:455
        // 
        // 用途: 控制是否发送 block reply (中间响应)
        // - false: 发送所有 block replies (Telegram, Slack, Web UI)
        // - true: 只发送 final reply (WhatsApp, Signal, iMessage)
        //
        // 设计理由:
        // - WhatsApp 等渠道不支持消息更新，发送 block 会造成消息洪水
        // - 隐私保护: 避免在外部渠道泄露推理过程 (thinking)
        //
        // 使用示例:
        // if (message.channel == Channel::WhatsApp) {
        //     config.block_reply.disable_streaming = true;
        // }
        bool disable_streaming = false;
    } block_reply;
    
    // 工具执行配置
    bool parallel_tool_calls = false;  // 是否并行执行工具
    bool verbose_tool_output = true;   // 是否显示详细工具输出
    int thread_pool_size = 4;          // 全局线程池大小 (0=auto)

    // 🎯 对齐 OpenClaw: limitHistoryTurns — 限制 DM/channel 会话的历史轮数
    // 0 = 不限制 (default); >0 = 只保留最近 N 轮 user 消息及其关联的 assistant 响应
    int max_history_turns = 0;

    // 🎯 对齐 OpenClaw thinking.ts: dropThinkingBlocks
    // 当模型切换到不支持推理的模型时，自动剥离 <think> 块以避免 provider 拒绝
    // 对齐 OpenClaw transcript-policy.ts: shouldDropThinkingBlocksForModel
    bool drop_thinking_blocks = false;

    // ✅ 对齐 OpenClaw: tool loop detection config (types.tools.ts ToolLoopDetectionConfig)
    // 默认 disabled=false 以对齐 OpenClaw 的 DEFAULT_LOOP_DETECTION_CONFIG.enabled=false
    // 需要在 agent 配置中显式开启: loop_detection.enabled = true
    LoopDetectionConfig loop_detection;

    // ============================================================================
    // 🎯 对齐 OpenClaw run.ts:412-660: Auth profile rotation + model failover
    // ============================================================================

    // Auth profile — represents a single LLM provider credential
    struct AuthProfile {
        std::string id;           // e.g. "openai-main", "anthropic-fallback"
        std::string provider;     // e.g. "openai", "anthropic"
        std::string api_key;
        std::string base_url;
        std::string model;        // model override for this profile
        int64_t cooldown_until_ms = 0;  // timestamp when cooldown expires (0 = not cooling)
    };

    // Failover configuration
    struct FailoverConfig {
        bool enabled = false;
        // Auth profiles in priority order (first = primary, rest = fallbacks)
        std::vector<AuthProfile> profiles;
        // Cooldown duration in ms after a profile fails (default 60s)
        int64_t cooldown_ms = 60000;
        // Maximum retry attempts across all profiles before giving up
        int max_retries = 3;
        // Fallback model to use if primary model fails (thinking level degradation)
        std::string fallback_model;
    } failover;

    // ============================================================================
    // 🎯 对齐 OpenClaw: pi-hooks/context-pruning/ (extension.ts + runtime.ts)
    // Opt-in microcompact: 在每次调用 LLM 前对 in-memory messages 做剪枝
    // （仅修改本次请求的 payload，不改写 session transcript）
    // ============================================================================
    context::ContextPruningConfig context_pruning;
};

// ReAct 引擎 - 对齐 OpenClaw
class ReactEngine {
public:
    // ✅ 使用 LLMClient (向后兼容)
    static std::shared_ptr<ReactEngine> create(
        std::shared_ptr<tools::ToolManager> tool_manager,
        std::shared_ptr<llm::LLMClient> llm_client,
        const ReactConfig& config = ReactConfig{}
    );
    
    // ✅ 直接使用 Provider (推荐)
    static std::shared_ptr<ReactEngine> create_with_provider(
        std::shared_ptr<tools::ToolManager> tool_manager,
        std::shared_ptr<llm::Provider> provider,
        const ReactConfig& config = ReactConfig{}
    );
    
    // 执行 ReAct 循环
    ReactResult run(
        const std::string& user_input,
        const std::vector<LLMMessage>& history = {},
        const StreamCallbacks& callbacks = {}
    );
    
    // 异步执行
    std::future<ReactResult> run_async(
        const std::string& user_input,
        const std::vector<LLMMessage>& history = {},
        const StreamCallbacks& callbacks = {}
    );
    
    // 取消执行
    void cancel();
    
    // ✅ 新增: Steer — 向正在运行的 execute_loop 实时注入 user message
    // 🎯 对齐 OpenClaw: activeSession.steer(text)  (attempt.ts:2231)
    //   → session.agent.steer(AgentMessage{role:"user", content:text})
    //   语义: 向正在 streaming 的 LLM session 注入一条新 user turn
    //         注入的消息在下一轮 ReAct 循环开始时被消费，追加到 messages 列表
    //
    // 线程安全: 可从任意线程调用（通过 mutex 保护 steer 队列）
    // 返回 true 表示消息已入队（engine 正在运行）
    bool steer(const std::string& text);
    
    // 获取配置
    const ReactConfig& config() const { return config_; }
    ReactConfig& mutable_config() { return config_; }
    
    // ✅ 新增: Token 估算 (对齐 OpenClaw) - 改为 public
    int estimate_conversation_tokens(const std::vector<LLMMessage>& messages) const;
    
    // ✅ 新增: Overflow 检测 (对齐 OpenClaw) - 改为 public
    bool is_context_overflow_error(const std::string& error_msg) const;
    std::optional<int> extract_overflow_token_count(const std::string& error_msg) const;
    
    // ✅ 新增: Compaction 触发 (对齐 OpenClaw) - 改为 public
    struct CompactionResult {
        bool compacted = false;
        std::vector<LLMMessage> compressed_messages;
        std::string reason;
        int messages_before = 0;
        int messages_after = 0;
        int tokens_saved = 0;
    };
    CompactionResult trigger_compaction(
        std::vector<LLMMessage>& messages,
        const std::string& transcript_path = ""  // 对齐 OC: compaction 后持久化到 transcript
    );
    
private:
    ReactEngine(
        std::shared_ptr<tools::ToolManager> tool_manager,
        std::shared_ptr<llm::LLMClient> llm_client,
        const ReactConfig& config
    );
    
    // ✅ Provider 构造函数
    ReactEngine(
        std::shared_ptr<tools::ToolManager> tool_manager,
        std::shared_ptr<llm::Provider> provider,
        const ReactConfig& config
    );
    
    // 内部执行循环
    ReactResult execute_loop(
        const std::string& user_input,
        const std::vector<LLMMessage>& history,
        const StreamCallbacks& callbacks
    );
    
    // 处理 LLM 响应 (内部使用的简化结构)
    struct LLMResponseData {
        std::string text;
        std::vector<LLMToolCall> tool_calls;
        std::string reasoning;
        
        // Token usage (对应 GenerateResponse 的字段)
        int input_tokens = 0;
        int output_tokens = 0;
        int cache_read_tokens = 0;
        int cache_write_tokens = 0;
        // 🎯 对齐 OpenClaw: isLikelyContextOverflowError → triggers auto-compaction
        bool context_overflow = false;
    };
    
    // ✅ call_llm 支持 tools 参数 (对齐 OpenClaw)
    LLMResponseData call_llm(
        const std::vector<LLMMessage>& messages,
        const StreamCallbacks& callbacks,
        const std::vector<llm::ToolDefinition>& tools = {}
    );
    
    // 执行工具调用
    std::map<std::string, tools::ToolResult> execute_tools(
        const std::vector<LLMToolCall>& tool_calls,
        const StreamCallbacks& callbacks
    );
    
    // 提取推理内容 (thinking tags)
    struct ReasoningExtraction {
        std::string reasoning;
        std::string remaining_text;
        bool has_reasoning = false;
    };
    
    ReasoningExtraction extract_reasoning(const std::string& text);
    
    // 分块处理
    std::vector<std::string> chunk_text(const std::string& text);
    
    // ✅ 严格对齐 OpenClaw 的流式处理机制
    // 参考: openclaw-analysis/src/agents/pi-embedded-subscribe.ts
    
    // 双缓冲区状态 (对齐 OpenClaw pi-embedded-subscribe.ts)
    struct StreamingState {
        // 🎯 对齐 OpenClaw: deltaBuffer — raw accumulated text (including <think> tags)
        std::string delta_buffer;
        // 🎯 对齐 OpenClaw: blockBuffer — filtered text for block reply emission
        std::string block_buffer;
        // 🎯 对齐 OpenClaw: lastBlockReplyText — dedup guard
        std::string last_block_reply_text;
        // 🎯 对齐 OpenClaw: suppressBlockChunks — set by finalizeAssistantTexts
        bool suppress_block_chunks = false;
        // 🎯 对齐 OpenClaw EmbeddedBlockChunker: code-fence aware chunking
        // Tracks whether we're inside a fenced code block (``` or ~~~)
        bool in_code_fence = false;
        std::string code_fence_marker;  // stores the opening fence marker for matching close
        // 🎯 对齐 OpenClaw: reasoningStreamOpen
        bool reasoning_stream_open = false;
        // 🎯 对齐 OpenClaw: emittedAssistantUpdate
        bool emitted_assistant_update = false;
        // 🎯 对齐 OpenClaw: lastStreamedAssistant / lastStreamedAssistantCleaned
        std::string last_streamed_assistant;
        std::string last_streamed_assistant_cleaned;
        // 🎯 对齐 OpenClaw: lastStreamedReasoning / lastReasoningSent
        std::string last_streamed_reasoning;
        std::string last_reasoning_sent;
        // 🎯 对齐 OpenClaw: assistantMessageIndex — incremented at message_start
        int assistant_message_index = 0;
        // 🎯 对齐 OpenClaw: lastAssistantTextMessageIndex / lastAssistantTextNormalized
        int last_assistant_text_message_index = -1;
        std::string last_assistant_text_normalized;
        std::string last_assistant_text_trimmed;
        // 🎯 对齐 OpenClaw: assistantTextBaseline
        int assistant_text_baseline = 0;
        // 🎯 对齐 OpenClaw: noteLastAssistant — tracks last assistant message for agent_end error detection
        std::string last_assistant_full_text;
        // 🎯 对齐 OpenClaw: deterministicApprovalPromptSent
        // Set when an exec approval prompt is deterministically delivered.
        // When true, assistant text output is suppressed (handlers.messages.ts:88,267)
        bool deterministic_approval_prompt_sent = false;
        // 🎯 对齐 OpenClaw: partialBlockState (thinking/final stateful tracking)
        struct BlockState {
            bool thinking = false;
            bool final_tag = false;
        } block_state, partial_block_state;

        // Messaging tool 冲突检测
        // 🎯 对齐 OpenClaw: messagingToolSentTexts / messagingToolSentTextsNormalized
        std::vector<std::string> messaging_tool_sent_texts;
        std::vector<std::string> messaging_tool_sent_texts_normalized;

        // 🎯 对齐 OpenClaw: resetAssistantMessageState(nextAssistantTextBaseline)
        void reset_assistant_message_state(int next_baseline) {
            delta_buffer.clear();
            block_buffer.clear();
            last_block_reply_text.clear();
            suppress_block_chunks = false;
            reasoning_stream_open = false;
            emitted_assistant_update = false;
            last_streamed_assistant.clear();
            last_streamed_assistant_cleaned.clear();
            last_streamed_reasoning.clear();
            last_reasoning_sent.clear();
            block_state = {};
            partial_block_state = {};
            assistant_message_index += 1;
            last_assistant_text_message_index = -1;
            last_assistant_text_normalized.clear();
            last_assistant_text_trimmed.clear();
            assistant_text_baseline = next_baseline;
        }

        void reset() {
            delta_buffer.clear();
            block_buffer.clear();
            last_block_reply_text.clear();
            suppress_block_chunks = false;
            reasoning_stream_open = false;
            emitted_assistant_update = false;
            last_streamed_assistant.clear();
            last_streamed_assistant_cleaned.clear();
            last_streamed_reasoning.clear();
            last_reasoning_sent.clear();
            assistant_message_index = 0;
            last_assistant_text_message_index = -1;
            last_assistant_text_normalized.clear();
            last_assistant_text_trimmed.clear();
            assistant_text_baseline = 0;
            block_state = {};
            partial_block_state = {};
            in_code_fence = false;
            code_fence_marker.clear();
            last_assistant_full_text.clear();
            deterministic_approval_prompt_sent = false;
            messaging_tool_sent_texts.clear();
            messaging_tool_sent_texts_normalized.clear();
        }
    };
    
    // 发送块回复 (带去重和冲突检测)
    void emit_block_chunk(const std::string& text, const StreamCallbacks& callbacks);
    
    // 刷新块回复缓冲区
    void flush_block_reply_buffer(const StreamCallbacks& callbacks);
    
    // 辅助函数
    std::string normalize_text_for_comparison(const std::string& text);
    bool is_messaging_tool_duplicate(const std::string& normalized_text);
    std::string strip_thinking_tags(const std::string& text);
    std::string trim_end(const std::string& text);
    
    std::shared_ptr<tools::ToolManager> tool_manager_;
    std::shared_ptr<llm::LLMClient> llm_client_;  // 向后兼容
    std::shared_ptr<llm::Provider> provider_;     // ✅ 直接 Provider 支持
    ReactConfig config_;

    // ✅ 对齐 OpenClaw: tool loop detection state (pi-tools.before-tool-call.ts)
    // 每次 run() 调用时重置，跨工具调用之间保持状态
    std::shared_ptr<LoopDetectionState> loop_detection_state_;
    
    // 运行状态
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> running_{false};  // ✅ 新增: 标记 execute_loop 是否正在运行
    std::chrono::steady_clock::time_point start_time_;
    
    // ✅ 新增: Steer 队列 (对齐 OpenClaw: activeSession.steer → agent.steer)
    // 线程安全: 由 steer_mutex_ 保护，steer() 写入，execute_loop 读取
    mutable std::mutex steer_mutex_;
    std::queue<std::string> steer_queue_;
    
    // ✅ 流式处理状态 (对齐 OpenClaw)
    StreamingState streaming_state_;
    
    // ✅ Reasoning Stream 累积状态 (对齐 OpenClaw)
    std::string accumulated_reasoning_raw_;        // 原始累积 (未格式化)
    std::string accumulated_reasoning_formatted_;  // 格式化累积 (用于增量计算)
    
    // ✅ 全局线程池 (用于并行工具执行)
    static std::shared_ptr<core::ThreadPool>& get_thread_pool();
    
    // ✅ 新增: CompactionManager 集成 (对齐 OpenClaw)
    std::shared_ptr<memory::CompactionManager> compaction_mgr_;
    
    // ✅ 新增: ContextEngine 集成 (对齐 OpenClaw run.ts:contextEngine)
    std::shared_ptr<context::ContextEngine> context_engine_;
    
    // ✅ 新增: Context Window 配置 (对齐 OpenClaw)
    struct ContextWindowConfig {
        int context_window_tokens = 200000;
        int reserve_tokens_floor = 20000;
        int soft_threshold_tokens = 4000;
    };
    ContextWindowConfig context_window_config_;

    // ✅ 对齐 OpenClaw pi-hooks/context-pruning/runtime.ts:
    //   cache-ttl 模式下记录上一次 prompt cache 写入时间戳
    // - 单位: 毫秒 since epoch; 0 = 尚未 touched
    // - 每次剪枝后更新 (对齐 OC extension.ts: runtime.lastCacheTouchAt = Date.now())
    int64_t last_cache_touch_at_ms_ = 0;

    // ✅ 对齐 OC: 归一化的剪枝设置 (懒初始化，避免每次 run 重算)
    mutable std::optional<context::EffectiveContextPruningSettings> effective_pruning_settings_;

    // ✅ 对齐 OC: pruner.ts pruneContextMessages
    //   在每次 call_llm 之前调用，返回值表示是否有修改 (used for cache-touch update)
    bool maybe_prune_context(std::vector<LLMMessage>& messages);

    // 🎯 严格对齐 OpenClaw pi-embedded-runner/tools-payload.ts:
    //   每次调用 LLM 前，从 ToolManager 收集全部工具 (内置 + MCP)，
    //   构造 LLM 原生 tools 参数 (API `tools: [...]` 字段)。
    //   - 与 system_prompt 无关 (OC 的工具声明严格走 API 参数通道)
    //   - 与 skills 不冲突 (skills 走 system_prompt 的 <available_skills> 块)
    std::vector<llm::ToolDefinition> build_tool_definitions() const;

public:
    // ✅ 新增: 设置 CompactionManager (从 AgentInstance 调用)
    void set_compaction_manager(std::shared_ptr<memory::CompactionManager> mgr) {
        compaction_mgr_ = mgr;
    }
    
    // ✅ 新增: 设置 ContextEngine (对齐 OpenClaw run.ts:contextEngine)
    // 在 run 开始前设置，由 message_dispatcher 或 agent instance 注入
    void set_context_engine(std::shared_ptr<context::ContextEngine> engine) {
        context_engine_ = std::move(engine);
    }
    
    // 获取 ContextEngine (用于外部访问)
    std::shared_ptr<context::ContextEngine> get_context_engine() const {
        return context_engine_;
    }
    
    // ✅ 新增: 配置 Context Window (对齐 OpenClaw)
    void set_context_window_config(const ContextWindowConfig& cfg) {
        context_window_config_ = cfg;
    }
};

} // namespace react
} // namespace hunterclaw
