#pragma once

#include "hunterclaw/core/json.h"
#include "hunterclaw/core/thread_pool.h"
#include "hunterclaw/tools/tool.h"  // ToolManager is defined in tool.h
#include "hunterclaw/llm/llm_client.h"
#include "hunterclaw/llm/provider.h"  // For Response, ToolCall, Usage
#include "hunterclaw/memory/compaction_manager.h"  // ✅ 新增: Compaction 支持
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <future>
#include <optional>

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
    std::function<void(const std::string& reasoning)> on_reasoning_stream;
    
    // 助手输出流 (assistant delta)
    std::function<void(const std::string& text)> on_assistant_delta;
    
    // 块回复 (block reply) - 在工具执行前刷新
    std::function<void(const std::string& block)> on_block_reply;
    
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
    
    // 🎯 OpenClaw 对齐: 用于直连推送的 session_id
    std::string session_id;
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
    
    // 获取配置
    const ReactConfig& config() const { return config_; }
    ReactConfig& mutable_config() { return config_; }
    
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
    
    // 双缓冲区状态 (对齐 OpenClaw)
    struct StreamingState {
        std::string delta_buffer;           // 原始累积 (含 <thinking>)
        std::string block_buffer;           // 过滤后累积 (发送用)
        std::string last_block_reply_text;  // 上次发送的块 (去重)
        bool suppress_block_chunks = false; // 动态抑制标志
        
        // Messaging tool 冲突检测
        std::vector<std::string> messaging_tool_sent_texts;
        std::vector<std::string> messaging_tool_sent_texts_normalized;
        
        void reset() {
            delta_buffer.clear();
            block_buffer.clear();
            last_block_reply_text.clear();
            suppress_block_chunks = false;
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
    
    // ✅ 新增: Token 估算 (对齐 OpenClaw)
    int estimate_conversation_tokens(const std::vector<LLMMessage>& messages) const;
    
    // ✅ 新增: Overflow 检测 (对齐 OpenClaw)
    bool is_context_overflow_error(const std::string& error_msg) const;
    std::optional<int> extract_overflow_token_count(const std::string& error_msg) const;
    
    // ✅ 新增: Compaction 触发 (对齐 OpenClaw)
    struct CompactionResult {
        bool compacted = false;
        std::vector<LLMMessage> compressed_messages;
        std::string reason;
        int messages_before = 0;
        int messages_after = 0;
        int tokens_saved = 0;
    };
    CompactionResult trigger_compaction(std::vector<LLMMessage>& messages);
    
    std::shared_ptr<tools::ToolManager> tool_manager_;
    std::shared_ptr<llm::LLMClient> llm_client_;  // 向后兼容
    std::shared_ptr<llm::Provider> provider_;     // ✅ 直接 Provider 支持
    ReactConfig config_;
    
    // 运行状态
    std::atomic<bool> cancelled_{false};
    std::chrono::steady_clock::time_point start_time_;
    
    // ✅ 流式处理状态 (对齐 OpenClaw)
    StreamingState streaming_state_;
    
    // ✅ Reasoning Stream 累积状态 (对齐 OpenClaw)
    std::string accumulated_reasoning_raw_;        // 原始累积 (未格式化)
    std::string accumulated_reasoning_formatted_;  // 格式化累积 (用于增量计算)
    
    // ✅ 全局线程池 (用于并行工具执行)
    static std::shared_ptr<core::ThreadPool>& get_thread_pool();
    
    // ✅ 新增: CompactionManager 集成 (对齐 OpenClaw)
    std::shared_ptr<memory::CompactionManager> compaction_mgr_;
    
    // ✅ 新增: Context Window 配置 (对齐 OpenClaw)
    struct ContextWindowConfig {
        int context_window_tokens = 200000;
        int reserve_tokens_floor = 20000;
        int soft_threshold_tokens = 4000;
    };
    ContextWindowConfig context_window_config_;

public:
    // ✅ 新增: 设置 CompactionManager (从 AgentInstance 调用)
    void set_compaction_manager(std::shared_ptr<memory::CompactionManager> mgr) {
        compaction_mgr_ = mgr;
    }
    
    // ✅ 新增: 配置 Context Window (对齐 OpenClaw)
    void set_context_window_config(const ContextWindowConfig& cfg) {
        context_window_config_ = cfg;
    }
};

} // namespace react
} // namespace hunterclaw
