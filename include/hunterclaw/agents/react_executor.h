#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/llm/llm_client.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <optional>
#include <variant>
#include <stdexcept>

namespace hunterclaw {
namespace agents {

// ============================================================================
// ReAct Core Types (严格对齐 OpenClaw)
// ============================================================================

/// AgentAction - 代理决定执行的动作
struct AgentAction {
    std::string tool;                  // 工具名称
    core::JsonValue tool_input;        // 工具输入 (string or dict)
    std::string log;                   // 完整的LLM输出 (包含Thought)
    std::string message_log;           // 可选的消息日志
    
    // 可选的工具调用ID (用于tool_calls模式)
    std::optional<std::string> tool_call_id;
};

/// AgentFinish - 代理的最终输出
struct AgentFinish {
    std::map<std::string, core::JsonValue> return_values;  // 返回值字典
    std::string log;                   // 完整的LLM输出
    std::string message_log;           // 可选的消息日志
};

/// AgentStep - 单个agent步骤 (Action + Observation)
struct AgentStep {
    AgentAction action;                // 执行的动作
    std::string observation;           // 观察结果
};

/// ReAct步骤的输出 (可能是Action或Finish)
using AgentOutput = std::variant<AgentAction, AgentFinish>;

// ============================================================================
// ReAct Output Parser (对齐 OpenClaw)
// ============================================================================

/// OutputParserException - 解析错误异常
struct OutputParserException : public std::runtime_error {
    std::string llm_output;            // 原始LLM输出
    std::string observation;           // 错误观察 (返回给LLM)
    bool send_to_llm;                  // 是否将错误发送给LLM重试
    
    OutputParserException(
        const std::string& msg,
        const std::string& llm_out = "",
        const std::string& obs = "",
        bool send = true
    ) : std::runtime_error(msg)
      , llm_output(llm_out)
      , observation(obs)
      , send_to_llm(send)
    {}
};

/// ReAct输出解析器 (单输入模式)
class ReActOutputParser {
public:
    ReActOutputParser();
    
    /// 解析LLM输出为AgentAction或AgentFinish
    /// @throws OutputParserException 解析失败时抛出
    AgentOutput parse(const std::string& text);
    
    /// 获取格式说明 (用于prompt)
    std::string get_format_instructions() const;

private:
    static constexpr const char* FINAL_ANSWER_ACTION = "Final Answer:";
    static constexpr const char* MISSING_ACTION_ERROR = 
        "Invalid Format: Missing 'Action:' after 'Thought:'";
    static constexpr const char* FINAL_ANSWER_AND_PARSABLE_ACTION_ERROR =
        "Parsing LLM output produced both a final answer and a parse-able action";
};

// ============================================================================
// ReAct Agent Configuration (对齐 OpenClaw)
// ============================================================================

struct ReActConfig {
    // 执行控制
    int max_iterations = 15;           // 最大迭代次数 (OpenClaw默认)
    double max_execution_time = 0.0;   // 最大执行时间(秒, 0=无限制)
    
    // 早停策略: "force" 或 "generate"
    std::string early_stopping_method = "force";
    
    // 错误处理
    bool handle_parsing_errors = true;  // 是否处理解析错误
    std::string parsing_error_message;  // 自定义解析错误消息
    
    // Prompt配置
    std::string observation_prefix = "Observation: ";
    std::string llm_prefix = "Thought: ";
    
    // Tool调用模式
    bool use_tool_calls = false;        // 使用结构化tool_calls (vs 文本解析)
    
    // 回调配置
    bool trim_intermediate_steps = false;  // 是否裁剪中间步骤
    int max_intermediate_steps = -1;       // 最大保留步骤数 (-1=无限制)
};

// ============================================================================
// ReAct Executor (对齐 OpenClaw AgentExecutor)
// ============================================================================

/// ReAct执行器回调
struct ReActCallbacks {
    using ActionCallback = std::function<void(const AgentAction&)>;
    using FinishCallback = std::function<void(const AgentFinish&)>;
    using StepCallback = std::function<void(const AgentStep&)>;
    using ErrorCallback = std::function<void(const std::string&)>;
    
    ActionCallback on_action_start;    // Action开始前
    ActionCallback on_action_end;      // Action完成后
    StepCallback on_step;              // 每个Step完成后
    FinishCallback on_finish;          // Agent完成时
    ErrorCallback on_error;            // 错误发生时
};

/// ReAct执行器 - 核心循环引擎
class ReActExecutor {
public:
    ReActExecutor(
        std::shared_ptr<llm::LLMClient> llm,
        std::shared_ptr<tools::ToolManager> tool_manager,
        const ReActConfig& config = ReActConfig()
    );
    
    ~ReActExecutor() = default;
    
    /// 执行Agent
    /// @param inputs 输入字典 (必须包含 "input" 键)
    /// @return AgentFinish结果或错误
    core::Result<AgentFinish> run(
        const std::map<std::string, std::string>& inputs
    );
    
    /// 执行单步迭代 (流式API)
    /// @param inputs 输入字典
    /// @param callback 每个步骤的回调
    /// @return 最终结果或错误
    core::Result<AgentFinish> run_iter(
        const std::map<std::string, std::string>& inputs,
        std::function<void(const AgentStep&)> callback
    );
    
    /// 设置回调
    void set_callbacks(const ReActCallbacks& callbacks);
    
    /// 获取配置
    const ReActConfig& get_config() const { return config_; }
    
    /// 设置系统Prompt
    void set_system_prompt(const std::string& prompt);
    
    /// 设置工具描述
    void set_tools_description(const std::string& desc);

private:
    // 核心组件
    std::shared_ptr<llm::LLMClient> llm_;
    std::shared_ptr<tools::ToolManager> tool_manager_;
    ReActConfig config_;
    ReActOutputParser parser_;
    ReActCallbacks callbacks_;
    
    // Prompt组件
    std::string system_prompt_;
    std::string tools_description_;
    
    // 辅助方法
    
    /// 判断是否应该继续执行
    bool should_continue(int iterations, double time_elapsed) const;
    
    /// 执行单步: Thought -> Action -> Observation
    core::Result<std::vector<AgentStep>> take_next_step(
        const std::map<std::string, std::string>& inputs,
        std::vector<AgentStep>& intermediate_steps
    );
    
    /// 执行单个agent action
    core::Result<AgentStep> perform_agent_action(
        const AgentAction& action
    );
    
    /// 构建完整输入 (包含agent_scratchpad)
    std::map<std::string, std::string> get_full_inputs(
        const std::map<std::string, std::string>& inputs,
        const std::vector<AgentStep>& intermediate_steps
    ) const;
    
    /// 格式化intermediate_steps为agent_scratchpad
    std::string format_log_to_str(
        const std::vector<AgentStep>& intermediate_steps
    ) const;
    
    /// 处理解析错误
    core::Result<AgentStep> handle_parsing_error(
        const OutputParserException& e
    );
    
    /// 生成早停响应
    AgentFinish return_stopped_response(
        const std::vector<AgentStep>& intermediate_steps,
        const std::map<std::string, std::string>& inputs
    );
    
    /// 调用LLM进行规划
    core::Result<AgentOutput> plan(
        const std::map<std::string, std::string>& inputs,
        const std::vector<AgentStep>& intermediate_steps
    );
    
    /// 准备中间步骤 (裁剪等)
    std::vector<AgentStep> prepare_intermediate_steps(
        const std::vector<AgentStep>& steps
    ) const;
};

// ============================================================================
// ReAct Prompt Templates (对齐 OpenClaw)
// ============================================================================

namespace react_prompts {

/// ReAct标准格式说明
extern const char* FORMAT_INSTRUCTIONS;

/// ReAct Prefix (工具介绍)
extern const char* PREFIX;

/// ReAct Suffix (开始执行)
extern const char* SUFFIX;

/// Wiki风格的Few-shot示例
extern const char* WIKI_EXAMPLES;

/// 构建完整ReAct Prompt
std::string build_react_prompt(
    const std::string& tools_description,
    const std::string& examples = "",
    const std::string& prefix = PREFIX,
    const std::string& suffix = SUFFIX
);

/// 渲染工具描述为文本格式
std::string render_text_description(
    const std::vector<tools::ToolManager::LLMToolDefinition>& tools
);

} // namespace react_prompts

// ============================================================================
// ReAct Graph Builder (对齐 LangGraph模式)
// ============================================================================

/// ReAct图状态
struct ReActState {
    std::vector<core::Message> messages;     // 消息历史
    std::optional<int> remaining_steps;      // 剩余步骤数
    
    // 扩展状态 (可选)
    std::map<std::string, core::JsonValue> extra;
};

/// ReAct图节点类型
enum class ReActNodeType {
    Agent,      // Agent节点 (调用LLM决策)
    Tools,      // 工具节点 (执行工具)
    PreHook,    // 前置钩子
    PostHook,   // 后置钩子
    End         // 结束节点
};

/// ReAct图配置 (LangGraph风格)
struct ReActGraphConfig {
    // 钩子
    std::function<void(ReActState&)> pre_model_hook;
    std::function<void(ReActState&)> post_model_hook;
    
    // 路由配置
    std::string entrypoint = "agent";  // 入口节点
    int recursion_limit = 25;          // 递归限制
    
    // 检查点 (持久化支持)
    bool enable_checkpointer = false;
    std::string checkpoint_dir;
};

/// ReAct图执行器 (LangGraph风格 - 更现代化)
class ReActGraph {
public:
    ReActGraph(
        std::shared_ptr<llm::LLMClient> llm,
        std::shared_ptr<tools::ToolManager> tool_manager,
        const ReActGraphConfig& config = ReActGraphConfig()
    );
    
    /// 执行图
    core::Result<ReActState> invoke(const ReActState& initial_state);
    
    /// 流式执行
    core::Result<void> stream(
        const ReActState& initial_state,
        std::function<void(const ReActState&)> callback
    );

private:
    std::shared_ptr<llm::LLMClient> llm_;
    std::shared_ptr<tools::ToolManager> tool_manager_;
    ReActGraphConfig config_;
    
    // 节点实现
    ReActState call_agent_node(const ReActState& state);
    ReActState call_tools_node(const ReActState& state);
    
    // 路由决策
    ReActNodeType should_continue(const ReActState& state);
};

} // namespace agents
} // namespace hunterclaw
