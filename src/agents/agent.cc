#include "hunterclaw/agents/agent.h"
#include "hunterclaw/agents/react_executor.h"
#include "hunterclaw/llm/llm_client.h"
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/utils.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include <iostream>

namespace hunterclaw {
namespace agents {

// ============================================================================
// Helper Functions
// ============================================================================

static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    
    const char* hex = "0123456789abcdef";
    std::string uuid = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    
    for (auto& c : uuid) {
        if (c == 'x') {
            c = hex[dis(gen)];
        } else if (c == 'y') {
            c = hex[(dis(gen) & 0x3) | 0x8];
        }
    }
    
    return uuid;
}

static uint64_t current_timestamp_ms() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// ============================================================================
// DefaultAgent Implementation
// ============================================================================

Agent::Ptr DefaultAgent::create(
    const AgentConfig& config,
    std::shared_ptr<skills::SkillManager> skill_mgr,
    std::shared_ptr<channels::ChannelManager> channel_mgr
) {
    return std::make_shared<DefaultAgent>(config, skill_mgr, channel_mgr);
}

DefaultAgent::DefaultAgent(
    const AgentConfig& config,
    std::shared_ptr<skills::SkillManager> skill_mgr,
    std::shared_ptr<channels::ChannelManager> channel_mgr
)
    : config_(config)
    , skill_manager_(skill_mgr)
    , channel_manager_(channel_mgr)
{
}

std::string DefaultAgent::get_id() const {
    return config_.id;
}

const AgentConfig& DefaultAgent::get_config() const {
    return config_;
}

std::string DefaultAgent::generate_run_id() const {
    return "run_" + generate_uuid();
}

void DefaultAgent::emit_event(const AgentEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& callback : event_callbacks_) {
        try {
            callback(event);
        } catch (const std::exception& e) {
            // Log error but continue
            std::cerr << "[Agent] Event callback error: " << e.what() << std::endl;
        }
    }
}

void DefaultAgent::emit_lifecycle_event(
    const std::string& run_id,
    AgentLifecyclePhase phase,
    const core::JsonValue& data
) {
    auto ctx = get_run_context(run_id);
    if (!ctx) {
        return;
    }
    
    AgentEvent event;
    event.run_id = run_id;
    event.seq = ctx->next_seq++;
    event.stream = AgentEventStream::Lifecycle;
    event.timestamp_ms = current_timestamp_ms();
    event.session_key = ctx->session_key;
    
    // Build event data
    core::JsonValue event_data = core::JsonValue::object();
    event_data["phase"] = core::JsonValue(static_cast<int64_t>(phase));
    
    if (!data.is_null()) {
        event_data["details"] = data;
    }
    
    event.data = event_data;
    
    // Update context
    ctx->updated_at = event.timestamp_ms;
    
    switch (phase) {
        case AgentLifecyclePhase::Start:
            ctx->status = "running";
            break;
        case AgentLifecyclePhase::Running:
            ctx->status = "running";
            break;
        case AgentLifecyclePhase::Paused:
            ctx->status = "paused";
            break;
        case AgentLifecyclePhase::Resumed:
            ctx->status = "running";
            break;
        case AgentLifecyclePhase::End:
            ctx->status = "completed";
            break;
    }
    
    ctx->events.push_back(event);
    emit_event(event);
}

core::Result<std::string> DefaultAgent::build_system_prompt() const {
    std::ostringstream ss;
    
    // Add identity
    if (!config_.identity.name.empty()) {
        ss << "You are " << config_.identity.name << ".\n";
    }
    
    if (!config_.identity.role.empty()) {
        ss << "Role: " << config_.identity.role << "\n";
    }
    
    if (!config_.identity.persona.empty()) {
        ss << "Persona: " << config_.identity.persona << "\n";
    }
    
    if (!config_.identity.rules.empty()) {
        ss << "\nRules:\n";
        for (const auto& rule : config_.identity.rules) {
            ss << "- " << rule << "\n";
        }
//     }
//     
//     // Add skills section
//     if (skill_manager_) {
//         auto skills_list = skill_manager_->list_skills();
//         
//         // Apply skill filter
//         std::vector<std::string> filtered_skills;
//         if (config_.skills.empty()) {
//             // No filter, use all
//             filtered_skills = skills_list;
//         } else {
//             // Apply filter
//             for (const auto& skill_id : skills_list) {
//                 bool allowed = false;
//                 for (const auto& pattern : config_.skills) {
//                     if (pattern == "*" || pattern == skill_id) {
//                         allowed = true;
//                         break;
//                     }
//                     // Simple wildcard matching
//                     if (pattern.back() == '*') {
//                         std::string prefix = pattern.substr(0, pattern.size() - 1);
//                         if (skill_id.substr(0, prefix.size()) == prefix) {
//                             allowed = true;
//                             break;
//                         }
//                     }
//                 }
//                 if (allowed) {
//                     filtered_skills.push_back(skill_id);
//                 }
//             }
//         }
//         
//         if (!filtered_skills.empty()) {
//             ss << "\n<skills>\n";
//             ss << "You have access to the following skills:\n\n";
//             
//             for (const auto& skill_id : filtered_skills) {
//                 auto skill_result = skill_manager_->get_skill(skill_id);
//                 if (skill_result.is_ok()) {
//                     auto& skill = skill_result.value;
//                     ss << "## " << skill->meta.name << "\n";
//                     ss << skill->meta.description << "\n";
//                     
//                     if (!skill->meta.usage.empty()) {
//                         ss << "\nUsage:\n```\n" << skill->meta.usage << "\n```\n";
//                     }
//                     
//                     ss << "\n";
//                 }
//             }
//             
//             ss << "</skills>\n";
        }
    }
    
    // Add thinking level instruction
    if (config_.thinking_level != "off") {
        ss << "\nThinking: Your thinking level is set to '" 
           << config_.thinking_level << "'.\n";
        
        if (config_.thinking_level == "high") {
            ss << "Please think deeply and show your reasoning process.\n";
        } else if (config_.thinking_level == "low") {
            ss << "Please be concise and avoid lengthy explanations.\n";
        }
    }
    
    return core::Result<std::string>::Ok(ss.str());
}

core::Result<std::string> DefaultAgent::call_llm(
//     const std::string& system_prompt,
//     const std::string& user_message,
//     const std::string& run_id
// ) {
//     // 创建LLM配置
//     llm::LLMConfig llm_config;
//     llm_config.provider = config_.model.provider;
//     llm_config.model = config_.model.model;
//     llm_config.api_key = config_.model.api_key;
//     llm_config.endpoint = config_.model.endpoint;
//     llm_config.max_tokens = config_.model.max_tokens;
//     llm_config.temperature = config_.model.temperature;
//     llm_config.context_window = config_.model.context_tokens;
//     
//     // 创建LLM客户端
//     auto llm_client = llm::LLMClientFactory::create(llm_config);
//     
//     // 创建工具管理器
//     tools::ToolContext tool_context;
//     tool_context.workspace_dir = config_.workspace;
//     tool_context.agent_id = config_.id;
//     tool_context.session_key = "";  // 从run_context获取
//     
//     auto tool_manager = tools::ToolManager::create(tool_context);
//     
//     // 注册内置工具
//     tools::BuiltinTools::register_all(*tool_manager, tool_context);
//     
//     // 创建ReAct配置
//     ReActConfig react_config;
//     react_config.max_iterations = 15;
//     react_config.early_stopping_method = "force";
//     react_config.handle_parsing_errors = true;
//     
//     // 创建ReAct执行器
//     ReActExecutor executor(llm_client, tool_manager, react_config);
//     
//     // 设置系统prompt
//     executor.set_system_prompt(system_prompt);
//     
//     // 设置回调 - 发送事件
//     ReActCallbacks callbacks;
//     callbacks.on_action_start = [this, run_id](const AgentAction& action) {
//         // 发送工具执行开始事件
//         core::JsonValue event_data = core::JsonValue::object();
//         event_data["tool"] = action.tool;
//         event_data["input"] = action.tool_input.as_string();
//         
//         AgentEvent event;
//         auto ctx = get_run_context(run_id);
//         if (ctx) {
//             event.run_id = run_id;
//             event.seq = ctx->next_seq++;
//             event.stream = AgentEventStream::Tool;
//             event.timestamp_ms = current_timestamp_ms();
//             event.session_key = ctx->session_key;
//             event.data = event_data;
//             
//             emit_event(event);
//         }
//     };
//     
//     callbacks.on_step = [this, run_id](const AgentStep& step) {
//         // 发送步骤完成事件
//         core::JsonValue event_data = core::JsonValue::object();
//         event_data["action"] = step.action.tool;
//         event_data["observation"] = step.observation;
//         
//         AgentEvent event;
//         auto ctx = get_run_context(run_id);
//         if (ctx) {
//             event.run_id = run_id;
//             event.seq = ctx->next_seq++;
//             event.stream = AgentEventStream::Assistant;
//             event.timestamp_ms = current_timestamp_ms();
//             event.session_key = ctx->session_key;
//             event.data = event_data;
//             
//             emit_event(event);
//         }
//     };
//     
//     executor.set_callbacks(callbacks);
//     
//     // 执行ReAct循环
//     std::map<std::string, std::string> inputs;
//     inputs["input"] = user_message;
//     
//     auto result = executor.run(inputs);
//     
//     if (result.is_err()) {
//         return core::Result<std::string>::Err(result.error);
//     }
//     
//     // 提取最终答案
//     auto& finish = result.value;
//     if (finish.return_values.count("output")) {
//         return core::Result<std::string>::Ok(
//             finish.return_values.at("output").as_string()
//         );
//     }
    
    return core::Result<std::string>::Ok(finish.log);
}

core::Result<std::string> DefaultAgent::run(
    const std::string& message,
    const std::map<std::string, std::string>& options
) {
    // Generate run ID
    std::string run_id = generate_run_id();
    
    // Create run context
    auto context = std::make_shared<AgentRunContext>();
    context->run_id = run_id;
    context->session_key = options.count("session_key") 
        ? options.at("session_key") : "default";
    context->agent_id = config_.id;
    context->started_at = current_timestamp_ms();
    context->updated_at = context->started_at;
    context->status = "running";
    context->skill_manager = skill_manager_;
    context->channel_manager = channel_manager_;
    
    // Store context
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_runs_[run_id] = context;
    }
    
    // Emit start event
    emit_lifecycle_event(run_id, AgentLifecyclePhase::Start, core::JsonValue::object());
    
    try {
        // Build system prompt
        auto prompt_result = build_system_prompt();
        if (prompt_result.is_err()) {
            emit_lifecycle_event(
                run_id, 
                AgentLifecyclePhase::End,
                core::JsonValue::object({
                    {"error", prompt_result.error}
                })
            );
            return core::Result<std::string>::Err(
                "Failed to build system prompt: " + prompt_result.error
            );
        }
        
        // Call LLM
        auto llm_result = call_llm(prompt_result.value, message, run_id);
        if (llm_result.is_err()) {
            emit_lifecycle_event(
                run_id,
                AgentLifecyclePhase::End,
                core::JsonValue::object({
                    {"error", llm_result.error}
                })
            );
            return core::Result<std::string>::Err(
                "LLM call failed: " + llm_result.error
            );
        }
        
        // Emit end event
        emit_lifecycle_event(
            run_id,
            AgentLifecyclePhase::End,
            core::JsonValue::object({
                {"stop_reason", "complete"},
                {"response", llm_result.value}
            })
        );
        
        return core::Result<std::string>::Ok(run_id);
        
    } catch (const std::exception& e) {
        emit_lifecycle_event(
            run_id,
            AgentLifecyclePhase::End,
            core::JsonValue::object({
                {"error", e.what()}
            })
        );
        
        return core::Result<std::string>::Err(
            std::string("Agent run failed: ") + e.what()
        );
    }
}

core::Result<void> DefaultAgent::stop(
    const std::string& run_id,
    AgentStopReason reason
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = active_runs_.find(run_id);
    if (it == active_runs_.end()) {
        return core::Result<void>::Err("Run not found: " + run_id);
    }
    
    // Mark as stopped
    it->second->status = "stopped";
    
    // Emit end event
    std::string reason_str;
    switch (reason) {
        case AgentStopReason::Complete:
            reason_str = "complete";
            break;
        case AgentStopReason::Error:
            reason_str = "error";
            break;
        case AgentStopReason::Timeout:
            reason_str = "timeout";
            break;
        case AgentStopReason::UserStop:
            reason_str = "user_stop";
            break;
        case AgentStopReason::SessionReset:
            reason_str = "session_reset";
            break;
        case AgentStopReason::SessionDelete:
            reason_str = "session_delete";
            break;
    }
    
    emit_lifecycle_event(
        run_id,
        AgentLifecyclePhase::End,
        core::JsonValue::object({
            {"stop_reason", reason_str}
        })
    );
    
    // Remove from active runs
    active_runs_.erase(it);
    
    return core::Result<void>::Ok();
}

std::shared_ptr<AgentRunContext> DefaultAgent::get_run_context(
    const std::string& run_id
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_runs_.find(run_id);
    return it != active_runs_.end() ? it->second : nullptr;
}

std::vector<std::string> DefaultAgent::list_active_runs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> runs;
    for (const auto& pair : active_runs_) {
        runs.push_back(pair.first);
    }
    return runs;
}

void DefaultAgent::on_event(AgentEventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callbacks_.push_back(callback);
}

core::Result<AgentDeliveryPlan> DefaultAgent::build_delivery_plan(
    const std::string& channel_hint,
    const std::string& to_hint
) const {
    if (!channel_manager_) {
        return core::Result<AgentDeliveryPlan>::Err("Channel manager not available");
    }
    
    AgentDeliveryPlan plan;
    
    // Resolve channel
    if (!channel_hint.empty()) {
        plan.resolved_channel = channel_hint;
        plan.delivery_target_mode = "explicit";
    } else {
        // Try to infer from context
        auto channels = channel_manager_->list_channels();
        if (!channels.empty()) {
            plan.resolved_channel = channels[0];
            plan.delivery_target_mode = "implicit";
        } else {
            return core::Result<AgentDeliveryPlan>::Err("No channels available");
        }
    }
    
    // Resolve target
    if (!to_hint.empty()) {
        plan.resolved_to = to_hint;
    }
    
    return core::Result<AgentDeliveryPlan>::Ok(plan);
}

// ============================================================================
// AgentManager Implementation
// ============================================================================

AgentManager::Ptr AgentManager::create(
    const core::Config& config,
    std::shared_ptr<skills::SkillManager> skill_mgr,
    std::shared_ptr<channels::ChannelManager> channel_mgr
) {
    return std::make_shared<AgentManager>(config, skill_mgr, channel_mgr);
}

AgentManager::AgentManager(
    const core::Config& config,
    std::shared_ptr<skills::SkillManager> skill_mgr,
    std::shared_ptr<channels::ChannelManager> channel_mgr
)
    : base_config_(config)
    , skill_manager_(skill_mgr)
    , channel_manager_(channel_mgr)
{
    // Create default agent if not exists
    if (config.agents.empty()) {
        AgentConfig default_config;
        default_config.id = "default";
        default_config.is_default = true;
        default_config.name = "Default Agent";
        default_config.workspace = core::utils::expand_path("~/hunterclaw-workspace");
        
        // Set default model
        default_config.model.provider = "anthropic";
        default_config.model.model = "claude-3-5-sonnet-20241022";
        default_config.model.api_key = std::getenv("ANTHROPIC_API_KEY") 
            ? std::getenv("ANTHROPIC_API_KEY") : "";
        
        register_agent(default_config);
    } else {
        // Load agents from config
        for (const auto& agent_cfg : config.agents) {
            register_agent(agent_cfg);
        }
    }
}

AgentConfig AgentManager::merge_with_defaults(const AgentConfig& config) const {
    AgentConfig merged = config;
    
    // Apply defaults if not set
    if (merged.workspace.empty()) {
        merged.workspace = core::utils::expand_path("~/hunterclaw-workspace");
    }
    
    if (merged.timeout_seconds == 0) {
        merged.timeout_seconds = 600;
    }
    
    if (merged.model.model.empty()) {
        merged.model.model = "claude-3-5-sonnet-20241022";
    }
    
    return merged;
}

core::Result<void> AgentManager::register_agent(const AgentConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (config.id.empty()) {
        return core::Result<void>::Err("Agent ID cannot be empty");
    }
    
    if (agents_.count(config.id)) {
        return core::Result<void>::Err("Agent already exists: " + config.id);
    }
    
    auto merged_config = merge_with_defaults(config);
    auto agent = DefaultAgent::create(merged_config, skill_manager_, channel_manager_);
    
    agents_[config.id] = agent;
    
    if (merged_config.is_default) {
        default_agent_id_ = config.id;
    }
    
    return core::Result<void>::Ok();
}

core::Result<void> AgentManager::unregister_agent(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = agents_.find(agent_id);
    if (it == agents_.end()) {
        return core::Result<void>::Err("Agent not found: " + agent_id);
    }
    
    agents_.erase(it);
    
    if (default_agent_id_ == agent_id) {
        default_agent_id_.clear();
        // Set first agent as default
        if (!agents_.empty()) {
            default_agent_id_ = agents_.begin()->first;
        }
    }
    
    return core::Result<void>::Ok();
}

Agent::Ptr AgentManager::get_agent(const std::string& agent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(agent_id);
    return it != agents_.end() ? it->second : nullptr;
}

Agent::Ptr AgentManager::get_default_agent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!default_agent_id_.empty()) {
        auto it = agents_.find(default_agent_id_);
        if (it != agents_.end()) {
            return it->second;
        }
    }
    // Return first agent if no default
    return !agents_.empty() ? agents_.begin()->second : nullptr;
}

std::string AgentManager::resolve_agent_id(const std::string& session_key) const {
    // Simple resolution: check if session_key contains agent ID hint
    // Format: "agent_id:session_name" or just use default
    
    auto colon_pos = session_key.find(':');
    if (colon_pos != std::string::npos) {
        std::string potential_agent_id = session_key.substr(0, colon_pos);
        
        std::lock_guard<std::mutex> lock(mutex_);
        if (agents_.count(potential_agent_id)) {
            return potential_agent_id;
        }
    }
    
    return default_agent_id_;
}

std::vector<std::string> AgentManager::list_agents() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> agent_ids;
    for (const auto& pair : agents_) {
        agent_ids.push_back(pair.first);
    }
    return agent_ids;
}

core::Result<AgentConfig> AgentManager::get_agent_config(
    const std::string& agent_id
) const {
    auto agent = get_agent(agent_id);
    if (!agent) {
        return core::Result<AgentConfig>::Err("Agent not found: " + agent_id);
    }
    return core::Result<AgentConfig>::Ok(agent->get_config());
}

core::Result<void> AgentManager::update_agent_config(
    const std::string& agent_id,
    const AgentConfig& config
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = agents_.find(agent_id);
    if (it == agents_.end()) {
        return core::Result<void>::Err("Agent not found: " + agent_id);
    }
    
    // Recreate agent with new config
    auto merged_config = merge_with_defaults(config);
    auto new_agent = DefaultAgent::create(merged_config, skill_manager_, channel_manager_);
    
    agents_[agent_id] = new_agent;
    
    return core::Result<void>::Ok();
}

} // namespace agents
} // namespace hunterclaw
