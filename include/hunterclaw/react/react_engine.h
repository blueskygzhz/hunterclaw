#pragma once

#include "hunterclaw/core/json.h"
#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/llm/llm_client.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>

namespace hunterclaw {
namespace react {

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
    } block_reply;
    
    // 工具执行配置
    bool parallel_tool_calls = false;  // 是否并行执行工具
    bool verbose_tool_output = true;   // 是否显示详细工具输出
};

// ReAct 引擎 - 对齐 OpenClaw
class ReactEngine {
public:
    static std::shared_ptr<ReactEngine> create(
        std::shared_ptr<tools::ToolManager> tool_manager,
        std::shared_ptr<llm::LLMClient> llm_client,
        const ReactConfig& config = ReactConfig{}
    );
    
    // 执行 ReAct 循环
    ReactResult run(
        const std::string& user_input,
        const std::vector<llm::Message>& history = {},
        const StreamCallbacks& callbacks = {}
    );
    
    // 异步执行
    std::future<ReactResult> run_async(
        const std::string& user_input,
        const std::vector<llm::Message>& history = {},
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
    
    // 内部执行循环
    ReactResult execute_loop(
        const std::string& user_input,
        const std::vector<llm::Message>& history,
        const StreamCallbacks& callbacks
    );
    
    // 处理 LLM 响应
    struct LLMResponse {
        std::string text;
        std::vector<llm::ToolCall> tool_calls;
        std::string reasoning;
        llm::Usage usage;
    };
    
    LLMResponse call_llm(
        const std::vector<llm::Message>& messages,
        const StreamCallbacks& callbacks
    );
    
    // 执行工具调用
    std::map<std::string, tools::ToolResult> execute_tools(
        const std::vector<llm::ToolCall>& tool_calls,
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
    
    std::shared_ptr<tools::ToolManager> tool_manager_;
    std::shared_ptr<llm::LLMClient> llm_client_;
    ReactConfig config_;
    
    // 运行状态
    std::atomic<bool> cancelled_{false};
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace react
} // namespace hunterclaw
