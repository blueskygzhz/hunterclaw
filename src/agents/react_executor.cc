#include "hunterclaw/agents/react_executor.h"
#include <regex>
#include <sstream>
#include <chrono>
#include <iostream>

namespace hunterclaw {
namespace agents {

// ============================================================================
// ReAct Prompts (对齐 OpenClaw)
// ============================================================================

namespace react_prompts {

const char* FORMAT_INSTRUCTIONS = R"(Use the following format:

Question: the input question you must answer
Thought: you should always think about what to do
Action: the action to take, should be one of [{tool_names}]
Action Input: the input to the action
Observation: the result of the action
... (this Thought/Action/Action Input/Observation can repeat N times)
Thought: I now know the final answer
Final Answer: the final answer to the original input question)";

const char* PREFIX = "Answer the following questions as best you can. You have access to the following tools:";

const char* SUFFIX = R"(Begin!

Question: {input}
Thought:{agent_scratchpad})";

const char* WIKI_EXAMPLES = R"(
Question: What is the elevation range for the area that the eastern sector of the Colorado orogeny extends into?
Thought: I need to search Colorado orogeny, find the area that the eastern sector extends into, then find the elevation range of that area.
Action: search
Action Input: Colorado orogeny
Observation: The Colorado orogeny was an episode of mountain building (an orogeny) in Colorado and surrounding areas.
Thought: It does not mention the eastern sector. So I need to look up eastern sector.
Action: lookup
Action Input: eastern sector
Observation: (Result 1 / 1) The eastern sector extends into the High Plains and is called the Central Plains orogeny.
Thought: The eastern sector of Colorado orogeny extends into the High Plains. So I need to search High Plains and find its elevation range.
Action: search
Action Input: High Plains
Observation: High Plains refers to one of two distinct land regions
Thought: I need to instead search High Plains (United States).
Action: search
Action Input: High Plains (United States)
Observation: The High Plains are a subregion of the Great Plains. From east to west, the High Plains rise in elevation from around 1,800 to 7,000 ft (550 to 2,130 m).
Thought: High Plains rise in elevation from around 1,800 to 7,000 ft, so the answer is 1,800 to 7,000 feet.
Action: Final Answer
Action Input: 1,800 to 7,000 feet
)";

std::string build_react_prompt(
    const std::string& tools_description,
    const std::string& examples,
    const std::string& prefix,
    const std::string& suffix
) {
    std::ostringstream ss;
    
    ss << prefix << "\n\n";
    ss << tools_description << "\n\n";
    ss << FORMAT_INSTRUCTIONS << "\n";
    
    if (!examples.empty()) {
        ss << "\nExamples:\n" << examples << "\n";
    }
    
    ss << "\n" << suffix;
    
    return ss.str();
}

std::string render_text_description(
    const std::vector<tools::ToolManager::LLMToolDefinition>& tools
) {
    std::ostringstream ss;
    
    for (size_t i = 0; i < tools.size(); ++i) {
        if (i > 0) ss << "\n";
        ss << tools[i].name << ": " << tools[i].description;
    }
    
    return ss.str();
}

} // namespace react_prompts

// ============================================================================
// ReActOutputParser Implementation
// ============================================================================

ReActOutputParser::ReActOutputParser() {}

AgentOutput ReActOutputParser::parse(const std::string& text) {
    // 检查是否包含Final Answer
    bool includes_answer = text.find(FINAL_ANSWER_ACTION) != std::string::npos;
    
    // 正则表达式匹配 Action 和 Action Input
    // 格式: "Action: <tool_name>\nAction Input: <input>"
    // 注意: C++标准regex不支持dotall，使用[\s\S]*匹配任意字符(包括换行)
    std::regex action_regex(
        R"(Action\s*\d*\s*:\s*(.*?)\s*Action\s*\d*\s*Input\s*\d*\s*:\s*([\s\S]*))"
    );
    
    std::smatch match;
    bool has_action = std::regex_search(text, match, action_regex);
    
    if (has_action) {
        if (includes_answer) {
            throw OutputParserException(
                FINAL_ANSWER_AND_PARSABLE_ACTION_ERROR,
                text,
                "Parsing error: both final answer and action found",
                true
            );
        }
        
        // 提取tool和tool_input
        std::string tool = match[1].str();
        std::string tool_input_str = match[2].str();
        
        // 去除前后空白
        tool.erase(0, tool.find_first_not_of(" \t\n\r"));
        tool.erase(tool.find_last_not_of(" \t\n\r") + 1);
        tool_input_str.erase(0, tool_input_str.find_first_not_of(" \t\n\r"));
        tool_input_str.erase(tool_input_str.find_last_not_of(" \t\n\r") + 1);
        
        // 构建AgentAction
        AgentAction action;
        action.tool = tool;
        action.tool_input = core::JsonValue(tool_input_str);
        action.log = text;
        
        return action;
    }
    
    // 检查Final Answer
    if (includes_answer) {
        size_t pos = text.find(FINAL_ANSWER_ACTION);
        std::string answer = text.substr(pos + std::strlen(FINAL_ANSWER_ACTION));
        
        // 去除前后空白
        answer.erase(0, answer.find_first_not_of(" \t\n\r"));
        answer.erase(answer.find_last_not_of(" \t\n\r") + 1);
        
        AgentFinish finish;
        finish.return_values["output"] = core::JsonValue(answer);
        finish.log = text;
        
        return finish;
    }
    
    // 解析失败 - 抛出异常让Agent重试
    throw OutputParserException(
        "Could not parse LLM output: " + text,
        text,
        MISSING_ACTION_ERROR,
        true
    );
}

std::string ReActOutputParser::get_format_instructions() const {
    return react_prompts::FORMAT_INSTRUCTIONS;
}

// ============================================================================
// ReActExecutor Implementation
// ============================================================================

ReActExecutor::ReActExecutor(
    std::shared_ptr<llm::LLMClient> llm,
    std::shared_ptr<tools::ToolManager> tool_manager,
    const ReActConfig& config
)
    : llm_(llm)
    , tool_manager_(tool_manager)
    , config_(config)
{
    // 初始化默认prompt
    auto tool_defs = tool_manager_->to_llm_format();
    tools_description_ = react_prompts::render_text_description(tool_defs);
    
    // 提取工具名称列表
    std::ostringstream tool_names_ss;
    for (size_t i = 0; i < tool_defs.size(); ++i) {
        if (i > 0) tool_names_ss << ", ";
        tool_names_ss << tool_defs[i].name;
    }
    
    // 构建系统prompt
    system_prompt_ = react_prompts::build_react_prompt(
        tools_description_,
        "",  // 可选: 添加few-shot examples
        react_prompts::PREFIX,
        react_prompts::SUFFIX
    );
    
    // 替换工具名称占位符
    size_t pos = system_prompt_.find("{tool_names}");
    if (pos != std::string::npos) {
        system_prompt_.replace(pos, 12, tool_names_ss.str());
    }
}

void ReActExecutor::set_callbacks(const ReActCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void ReActExecutor::set_system_prompt(const std::string& prompt) {
    system_prompt_ = prompt;
}

void ReActExecutor::set_tools_description(const std::string& desc) {
    tools_description_ = desc;
}

bool ReActExecutor::should_continue(int iterations, double time_elapsed) const {
    // 检查最大迭代次数
    if (config_.max_iterations > 0 && iterations >= config_.max_iterations) {
        return false;
    }
    
    // 检查最大执行时间
    if (config_.max_execution_time > 0.0 && time_elapsed >= config_.max_execution_time) {
        return false;
    }
    
    return true;
}

std::string ReActExecutor::format_log_to_str(
    const std::vector<AgentStep>& intermediate_steps
) const {
    std::ostringstream ss;
    
    for (const auto& step : intermediate_steps) {
        ss << step.action.log;
        ss << "\n" << config_.observation_prefix << step.observation;
        ss << "\n" << config_.llm_prefix;
    }
    
    return ss.str();
}

std::vector<AgentStep> ReActExecutor::prepare_intermediate_steps(
    const std::vector<AgentStep>& steps
) const {
    if (!config_.trim_intermediate_steps) {
        return steps;
    }
    
    if (config_.max_intermediate_steps <= 0) {
        return steps;
    }
    
    // 只保留最后N个步骤
    if (static_cast<int>(steps.size()) <= config_.max_intermediate_steps) {
        return steps;
    }
    
    return std::vector<AgentStep>(
        steps.end() - config_.max_intermediate_steps,
        steps.end()
    );
}

std::map<std::string, std::string> ReActExecutor::get_full_inputs(
    const std::map<std::string, std::string>& inputs,
    const std::vector<AgentStep>& intermediate_steps
) const {
    auto full_inputs = inputs;
    
    // 添加agent_scratchpad
    auto prepared_steps = prepare_intermediate_steps(intermediate_steps);
    full_inputs["agent_scratchpad"] = format_log_to_str(prepared_steps);
    
    return full_inputs;
}

core::Result<AgentOutput> ReActExecutor::plan(
    const std::map<std::string, std::string>& inputs,
    const std::vector<AgentStep>& intermediate_steps
) {
    // 获取完整输入
    auto full_inputs = get_full_inputs(inputs, intermediate_steps);
    
    // 构建prompt
    std::string prompt = system_prompt_;
    
    // 替换占位符
    for (const auto& [key, value] : full_inputs) {
        std::string placeholder = "{" + key + "}";
        size_t pos = 0;
        while ((pos = prompt.find(placeholder, pos)) != std::string::npos) {
            prompt.replace(pos, placeholder.length(), value);
            pos += value.length();
        }
    }
    
    // 调用LLM
    auto llm_result = llm_->generate(prompt);
    if (llm_result.is_err()) {
        return core::Result<AgentOutput>::Err(
            "LLM call failed: " + llm_result.error
        );
    }
    
    // 解析输出
    try {
        auto output = parser_.parse(llm_result.value);
        return core::Result<AgentOutput>::Ok(output);
    } catch (const OutputParserException& e) {
        // 返回错误，让调用者处理
        return core::Result<AgentOutput>::Err(e.what());
    }
}

core::Result<AgentStep> ReActExecutor::handle_parsing_error(
    const OutputParserException& e
) {
    if (!config_.handle_parsing_errors) {
        return core::Result<AgentStep>::Err(
            "Parsing error: " + std::string(e.what())
        );
    }
    
    // 构建错误observation
    std::string observation;
    if (!config_.parsing_error_message.empty()) {
        observation = config_.parsing_error_message;
    } else if (!e.observation.empty()) {
        observation = e.observation;
    } else {
        observation = "Invalid or incomplete response. Please try again.";
    }
    
    // 创建异常Action
    AgentAction error_action;
    error_action.tool = "_Exception";
    error_action.tool_input = core::JsonValue(observation);
    error_action.log = e.llm_output.empty() ? "Parsing error occurred" : e.llm_output;
    
    // 返回错误步骤
    AgentStep error_step;
    error_step.action = error_action;
    error_step.observation = observation;
    
    return core::Result<AgentStep>::Ok(error_step);
}

core::Result<AgentStep> ReActExecutor::perform_agent_action(
    const AgentAction& action
) {
    // 触发回调
    if (callbacks_.on_action_start) {
        callbacks_.on_action_start(action);
    }
    
    // 检查工具是否存在
    auto tool = tool_manager_->get_tool(action.tool);
    if (!tool) {
        // 工具不存在 - 返回错误observation
        std::ostringstream error_msg;
        error_msg << "Error: " << action.tool << " is not a valid tool, try one of [";
        
        auto available_tools = tool_manager_->list_tools();
        for (size_t i = 0; i < available_tools.size(); ++i) {
            if (i > 0) error_msg << ", ";
            error_msg << available_tools[i];
        }
        error_msg << "].";
        
        AgentStep step;
        step.action = action;
        step.observation = error_msg.str();
        
        return core::Result<AgentStep>::Ok(step);
    }
    
    // 执行工具
    auto result = tool_manager_->execute_tool(action.tool, action.tool_input);
    
    // 构建observation
    std::ostringstream obs_ss;
    if (result.success) {
        // 成功 - 提取文本内容
        for (const auto& content : result.content) {
            if (content.type == tools::ToolResultContent::Type::Text) {
                obs_ss << content.text;
            } else if (content.type == tools::ToolResultContent::Type::Json) {
                obs_ss << content.text;  // JSON也作为文本返回
            }
        }
    } else {
        // 失败 - 返回错误信息
        obs_ss << "Error: " << result.error;
    }
    
    AgentStep step;
    step.action = action;
    step.observation = obs_ss.str();
    
    // 触发回调
    if (callbacks_.on_action_end) {
        callbacks_.on_action_end(action);
    }
    
    if (callbacks_.on_step) {
        callbacks_.on_step(step);
    }
    
    return core::Result<AgentStep>::Ok(step);
}

core::Result<std::vector<AgentStep>> ReActExecutor::take_next_step(
    const std::map<std::string, std::string>& inputs,
    std::vector<AgentStep>& intermediate_steps
) {
    std::vector<AgentStep> new_steps;
    
    // 1. 调用LLM进行规划
    auto plan_result = plan(inputs, intermediate_steps);
    
    if (plan_result.is_err()) {
        // 检查是否是解析错误
        try {
            throw;  // 尝试重新抛出当前异常
        } catch (const OutputParserException& e) {
            // 处理解析错误
            auto error_step_result = handle_parsing_error(e);
            if (error_step_result.is_err()) {
                return core::Result<std::vector<AgentStep>>::Err(
                    error_step_result.error
                );
            }
            new_steps.push_back(error_step_result.value);
            return core::Result<std::vector<AgentStep>>::Ok(new_steps);
        } catch (...) {
            // 其他错误
            return core::Result<std::vector<AgentStep>>::Err(
                plan_result.error
            );
        }
    }
    
    auto& output = plan_result.value;
    
    // 2. 检查是否完成
    if (std::holds_alternative<AgentFinish>(output)) {
        // 已完成 - 返回空steps (调用者会检测到AgentFinish)
        return core::Result<std::vector<AgentStep>>::Ok(new_steps);
    }
    
    // 3. 执行Action
    auto& action = std::get<AgentAction>(output);
    auto step_result = perform_agent_action(action);
    
    if (step_result.is_err()) {
        return core::Result<std::vector<AgentStep>>::Err(step_result.error);
    }
    
    new_steps.push_back(step_result.value);
    
    return core::Result<std::vector<AgentStep>>::Ok(new_steps);
}

AgentFinish ReActExecutor::return_stopped_response(
    const std::vector<AgentStep>& intermediate_steps,
    const std::map<std::string, std::string>& inputs
) {
    AgentFinish finish;
    
    if (config_.early_stopping_method == "force") {
        // 强制返回固定消息
        finish.return_values["output"] = core::JsonValue(
            "Agent stopped due to iteration limit or time limit."
        );
        finish.log = "Stopped early";
    } else if (config_.early_stopping_method == "generate") {
        // 让LLM生成最终答案
        auto full_inputs = get_full_inputs(inputs, intermediate_steps);
        
        std::string final_prompt = system_prompt_;
        for (const auto& [key, value] : full_inputs) {
            std::string placeholder = "{" + key + "}";
            size_t pos = 0;
            while ((pos = final_prompt.find(placeholder, pos)) != std::string::npos) {
                final_prompt.replace(pos, placeholder.length(), value);
                pos += value.length();
            }
        }
        
        final_prompt += "\n\nI now need to return a final answer based on the previous steps:";
        
        auto llm_result = llm_->generate(final_prompt);
        if (llm_result.is_ok()) {
            finish.return_values["output"] = core::JsonValue(llm_result.value);
            finish.log = llm_result.value;
        } else {
            finish.return_values["output"] = core::JsonValue(
                "Agent stopped but failed to generate final answer."
            );
            finish.log = "Early stop with LLM error";
        }
    }
    
    return finish;
}

core::Result<AgentFinish> ReActExecutor::run(
    const std::map<std::string, std::string>& inputs
) {
    // 检查必须的输入
    if (inputs.find("input") == inputs.end()) {
        return core::Result<AgentFinish>::Err(
            "Missing required input key: 'input'"
        );
    }
    
    std::vector<AgentStep> intermediate_steps;
    int iterations = 0;
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 核心循环: Thought -> Action -> Observation
    while (true) {
        // 计算已用时间
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - start_time).count();
        
        // 检查是否应该继续
        if (!should_continue(iterations, elapsed)) {
            auto finish = return_stopped_response(intermediate_steps, inputs);
            if (callbacks_.on_finish) {
                callbacks_.on_finish(finish);
            }
            return core::Result<AgentFinish>::Ok(finish);
        }
        
        // 执行下一步
        auto next_step_result = take_next_step(inputs, intermediate_steps);
        
        if (next_step_result.is_err()) {
            if (callbacks_.on_error) {
                callbacks_.on_error(next_step_result.error);
            }
            return core::Result<AgentFinish>::Err(next_step_result.error);
        }
        
        auto& new_steps = next_step_result.value;
        
        // 检查是否完成 (new_steps为空表示收到AgentFinish)
        if (new_steps.empty()) {
            // 从最后的plan结果中提取AgentFinish
            auto plan_result = plan(inputs, intermediate_steps);
            if (plan_result.is_ok() && 
                std::holds_alternative<AgentFinish>(plan_result.value)) {
                auto finish = std::get<AgentFinish>(plan_result.value);
                if (callbacks_.on_finish) {
                    callbacks_.on_finish(finish);
                }
                return core::Result<AgentFinish>::Ok(finish);
            }
        }
        
        // 添加新步骤
        intermediate_steps.insert(
            intermediate_steps.end(),
            new_steps.begin(),
            new_steps.end()
        );
        
        iterations++;
    }
}

core::Result<AgentFinish> ReActExecutor::run_iter(
    const std::map<std::string, std::string>& inputs,
    std::function<void(const AgentStep&)> callback
) {
    // 临时设置step回调
    auto old_callback = callbacks_.on_step;
    callbacks_.on_step = callback;
    
    auto result = run(inputs);
    
    // 恢复原回调
    callbacks_.on_step = old_callback;
    
    return result;
}

// ============================================================================
// ReActGraph Implementation (简化版 - LangGraph风格)
// ============================================================================

ReActGraph::ReActGraph(
    std::shared_ptr<llm::LLMClient> llm,
    std::shared_ptr<tools::ToolManager> tool_manager,
    const ReActGraphConfig& config
)
    : llm_(llm)
    , tool_manager_(tool_manager)
    , config_(config)
{}

ReActNodeType ReActGraph::should_continue(const ReActState& state) {
    if (state.messages.empty()) {
        return ReActNodeType::End;
    }
    
    // 获取最后一条消息
    const auto& last_message = state.messages.back();
    
    // 检查是否有tool_calls (简化实现 - 检查metadata)
    bool has_tool_calls = false;
    if (last_message.metadata.count("tool_calls")) {
        has_tool_calls = !last_message.metadata.at("tool_calls").is_null();
    }
    
    // 如果没有tool_calls，结束
    if (!has_tool_calls) {
        if (config_.post_model_hook) {
            return ReActNodeType::PostHook;
        }
        return ReActNodeType::End;
    }
    
    // 检查remaining_steps
    if (state.remaining_steps.has_value()) {
        if (state.remaining_steps.value() < 2) {
            // 步骤不足，结束
            return ReActNodeType::End;
        }
    }
    
    return ReActNodeType::Tools;
}

ReActState ReActGraph::call_agent_node(const ReActState& state) {
    // 简化实现: 调用LLM
    // TODO: 实现完整的agent节点逻辑
    ReActState new_state = state;
    return new_state;
}

ReActState ReActGraph::call_tools_node(const ReActState& state) {
    // 简化实现: 执行工具
    // TODO: 实现完整的工具节点逻辑
    ReActState new_state = state;
    return new_state;
}

core::Result<ReActState> ReActGraph::invoke(const ReActState& initial_state) {
    ReActState current_state = initial_state;
    int steps = 0;
    
    // 主循环
    while (steps < config_.recursion_limit) {
        // 前置钩子
        if (config_.pre_model_hook && steps == 0) {
            config_.pre_model_hook(current_state);
        }
        
        // Agent节点
        current_state = call_agent_node(current_state);
        
        // 决策下一步
        auto next_node = should_continue(current_state);
        
        if (next_node == ReActNodeType::End) {
            break;
        } else if (next_node == ReActNodeType::PostHook) {
            if (config_.post_model_hook) {
                config_.post_model_hook(current_state);
            }
            break;
        } else if (next_node == ReActNodeType::Tools) {
            current_state = call_tools_node(current_state);
        }
        
        // 更新remaining_steps
        if (current_state.remaining_steps.has_value()) {
            current_state.remaining_steps = current_state.remaining_steps.value() - 1;
        }
        
        steps++;
    }
    
    return core::Result<ReActState>::Ok(current_state);
}

core::Result<void> ReActGraph::stream(
    const ReActState& initial_state,
    std::function<void(const ReActState&)> callback
) {
    // 简化实现: 同步执行 + 回调
    auto result = invoke(initial_state);
    if (result.is_ok()) {
        callback(result.value);
    }
    return result.is_ok() 
        ? core::Result<void>::Ok() 
        : core::Result<void>::Err("Graph execution failed");
}

} // namespace agents
} // namespace hunterclaw
