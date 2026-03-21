#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/tools/tool_hooks.h"
#include "hunterclaw/tools/tool_loop_detector.h"
#include "hunterclaw/tools/tool_policy.h"
#include "hunterclaw/tools/tool_result_processor.h"
#include "hunterclaw/tools/approval.h"
#include "hunterclaw/mcp/mcporter_client.h"
#include "hunterclaw/mcp/mcp_tool_adapter.h"
#include <stdexcept>
#include <sstream>
#include <chrono>
#include <jsoncpp/json/json.h>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace tools {

// Namespace alias for MCP (resolve namespace conflict with forward declaration)
namespace mcp_impl = hunterclaw::mcp;

namespace {
    auto log() {
        auto logger = spdlog::get("tools");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("tools");
        }
        return logger;
    }
}

// ToolParams实现
std::string ToolParams::read_string(const std::string& key, bool required, 
                                   const std::string& default_val) const {
    if (!params_.is_object()) {
        if (required) {
            throw std::runtime_error("Required parameter '" + key + "' missing");
        }
        return default_val;
    }
    
    // TODO: 实现JsonValue的成员访问
    // 这里简化处理,实际需要从params_中取值
    if (required) {
        throw std::runtime_error("Required parameter '" + key + "' missing");
    }
    return default_val;
}

double ToolParams::read_number(const std::string& key, bool required, double default_val) const {
    if (required) {
        throw std::runtime_error("Required parameter '" + key + "' missing");
    }
    return default_val;
}

int64_t ToolParams::read_int(const std::string& key, bool required, int64_t default_val) const {
    if (required) {
        throw std::runtime_error("Required parameter '" + key + "' missing");
    }
    return default_val;
}

bool ToolParams::read_bool(const std::string& key, bool required, bool default_val) const {
    if (required) {
        throw std::runtime_error("Required parameter '" + key + "' missing");
    }
    return default_val;
}

std::vector<std::string> ToolParams::read_string_array(const std::string& key, bool required) const {
    if (required) {
        throw std::runtime_error("Required parameter '" + key + "' missing");
    }
    return {};
}

bool ToolParams::has(const std::string& key) const {
    return false;  // TODO: 实现
}

// ToolManager实现
std::shared_ptr<ToolManager> ToolManager::create(const ToolContext& context) {
    auto manager = std::make_shared<ToolManager>();
    manager->context_ = context;
    manager->hook_manager_ = std::make_shared<ToolHookManager>();
    return manager;
}

void ToolManager::register_tool(std::shared_ptr<Tool> tool) {
    tools_[tool->name()] = tool;
}

std::shared_ptr<Tool> ToolManager::get_tool(const std::string& name) {
    auto it = tools_.find(name);
    if (it != tools_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::string> ToolManager::list_tools() const {
    std::vector<std::string> result;
    for (const auto& [name, _] : tools_) {
        result.push_back(name);
    }
    return result;
}

ToolResult ToolManager::execute_tool(const std::string& name, const core::JsonValue& params) {
    return execute_tool(name, params, nullptr);
}

ToolResult ToolManager::execute_tool(
    const std::string& name,
    const core::JsonValue& params,
    ToolUpdateCallback on_update
) {
    auto start = std::chrono::steady_clock::now();
    
    // 1. 查找工具
    auto tool = get_tool(name);
    if (!tool) {
        return tool_result::error("Tool not found: " + name);
    }
    
    // 2. 策略检查 (100% 对齐 OpenClaw)
    if (policy_enabled_ && policy_pipeline_) {
        PolicyContext policy_ctx;
        policy_ctx.tool_name = name;
        policy_ctx.params = params;
        policy_ctx.tool_context = context_;
        
        auto policy_result = policy_pipeline_->evaluate(policy_ctx);
        
        if (policy_result.decision == PolicyDecision::DENY) {
            log()->warn("Tool '{}' denied by policy: {}", name, policy_result.reason);
            return tool_result::error("Policy denied: " + policy_result.reason);
        }
        
        if (policy_result.decision == PolicyDecision::REQUIRE_APPROVAL) {
            log()->info("Tool '{}' requires approval: {}", name, policy_result.reason);
            
            // 实现用户审批流程（对齐 OpenClaw）
            if (approval_manager_) {
                // 构建审批请求
                std::string command = name;
                if (params.is_object() && params.contains("command")) {
                    command = params["command"].as_string();
                }
                
                std::vector<std::string> argv = {name};
                if (params.is_object() && params.contains("args") && params["args"].is_array()) {
                    // 简化处理：从字符串解析参数
                    if (params["args"].is_string()) {
                        std::string args_str = params["args"].as_string();
                        // 简单的空格分割
                        std::istringstream iss(args_str);
                        std::string arg;
                        while (iss >> arg) {
                            argv.push_back(arg);
                        }
                    }
                }
                
                std::string cwd = context_.workspace_dir;
                if (params.is_object() && params.contains("cwd")) {
                    cwd = params["cwd"].as_string();
                }
                
                auto agent_config = approval_manager_->get_agent_config(context_.agent_id);
                auto approval_req = build_approval_request(
                    command, argv, cwd, 
                    context_.agent_id, context_.session_key, 
                    agent_config
                );
                
                // 请求审批
                auto req_result = approval_manager_->request_approval(approval_req);
                if (req_result.is_err()) {
                    return tool_result::error("Failed to request approval: " + req_result.error);
                }
                
                log()->info("Waiting for approval: {}", approval_req.id);
                
                // 等待用户决策
                auto resp_result = approval_manager_->wait_decision(approval_req.id);
                if (resp_result.is_err()) {
                    return tool_result::error("Approval wait failed: " + resp_result.error);
                }
                
                auto& response = resp_result.get();
                
                // 处理决策
                if (response.decision == ApprovalDecision::DENY) {
                    log()->warn("Tool '{}' denied by user", name);
                    return tool_result::error("Execution denied by user: " + response.reason);
                }
                
                if (response.decision == ApprovalDecision::TIMEOUT) {
                    log()->warn("Tool '{}' approval timeout", name);
                    return tool_result::error("Approval timeout");
                }
                
                // ALLOW_ONCE 或 ALLOW_ALWAYS，继续执行
                log()->info("Tool '{}' approved by {}", name, response.resolved_by);
            } else {
                // 没有 approval_manager，直接拒绝
                return tool_result::error("Tool requires approval but approval manager not configured");
            }
        }
    }
    
    // 3. Before Hooks (参数修改和验证)
    // Aligned with OpenClaw runBeforeToolCall() in plugins/hooks.ts
    core::JsonValue effective_params = params;
    if (hook_manager_) {
        PluginHookBeforeToolCallEvent before_event;
        before_event.tool_name = name;
        before_event.params = params;

        PluginHookToolContext tool_ctx;
        tool_ctx.tool_name = name;
        tool_ctx.agent_id = context_.agent_id.empty()
            ? std::nullopt : std::make_optional(context_.agent_id);
        tool_ctx.session_key = context_.session_key.empty()
            ? std::nullopt : std::make_optional(context_.session_key);

        auto before_result = hook_manager_->run_before_tool_call(before_event, tool_ctx);
        if (before_result.has_value()) {
            if (before_result->block.value_or(false)) {
                std::string reason = before_result->block_reason.value_or("blocked by plugin hook");
                log()->warn("Tool '{}' blocked by before_tool_call hook: {}", name, reason);
                return tool_result::error(reason);
            }
            if (before_result->params.has_value()) {
                effective_params = before_result->params.value();
            }
        }
    }
    
    // 4. 执行工具
    ToolResult result;
    std::string error_msg;
    try {
        result = tool->execute(effective_params);
    } catch (const std::exception& e) {
        error_msg = std::string("Tool execution failed: ") + e.what();
        result = tool_result::error(error_msg);
    }
    
    // 5. 结果处理 (100% 对齐 OpenClaw)
    // Note: result_normalization_enabled_ and result_pipeline_ not in current class
    // if (result_normalization_enabled_ && result_pipeline_) {
    //     result = result_pipeline_->process(result);
    // }
    
    // 6. After Hooks (日志记录、审计等)
    // Aligned with OpenClaw runAfterToolCall() in plugins/hooks.ts
    auto end = std::chrono::steady_clock::now();
    double duration_ms = std::chrono::duration<double, std::milli>(end - start).count();

    if (hook_manager_) {
        PluginHookAfterToolCallEvent after_event;
        after_event.tool_name = name;
        after_event.params = effective_params;
        after_event.duration_ms = duration_ms;
        if (!error_msg.empty()) {
            after_event.error = error_msg;
        } else {
            core::JsonValue result_json = core::JsonValue::object();
            result_json["success"] = result.success;
            if (!result.error.empty()) result_json["error"] = result.error;
            after_event.result = result_json;
        }

        PluginHookToolContext tool_ctx;
        tool_ctx.tool_name = name;
        tool_ctx.agent_id = context_.agent_id.empty()
            ? std::nullopt : std::make_optional(context_.agent_id);
        tool_ctx.session_key = context_.session_key.empty()
            ? std::nullopt : std::make_optional(context_.session_key);

        hook_manager_->run_after_tool_call(after_event, tool_ctx);
    }
    
    // 7. 循环检测 (100% 对齐 OpenClaw)
    // Note: loop_detection_enabled_ and loop_detector_ not in current class
    // if (loop_detection_enabled_ && loop_detector_) {
    //     loop_detector_->record_call(name, effective_params, result.success, "");
    //     
    //     auto loop_result = loop_detector_->detect_loop();
    //     if (loop_result.is_looping) {
    //         log()->error("Tool loop detected: {}", loop_result.suggestion);
    //         // 记录到 result metadata
    //         result.details["loop_warning"] = loop_result.suggestion;
    //     }
    // }
    
    return result;
}

// std::vector<ToolManager::LLMToolDefinition> ToolManager::to_llm_format() const {
//     std::vector<LLMToolDefinition> result;
//     
//     for (const auto& [name, tool] : tools_) {
//         LLMToolDefinition def;
//         def.name = tool->name();
//         def.description = tool->description();
//         
//         // 转换Schema为JSON字符串
//         auto schema = tool->parameters();
//         Json::Value json_schema;
//         json_schema["type"] = schema.type;
//         json_schema["description"] = schema.description;
//         
//         Json::Value props(Json::objectValue);
//         for (const auto& [prop_name, prop_value] : schema.properties) {
//             // TODO: 转换JsonValue到Json::Value
//             props[prop_name] = Json::Value();
//         }
//         json_schema["properties"] = props;
//         
//         Json::Value required(Json::arrayValue);
//         for (const auto& req : schema.required) {
//             required.append(req);
//         }
//         json_schema["required"] = required;
//         
//         Json::StreamWriterBuilder writer;
//         def.parameters = Json::writeString(writer, json_schema);
//         
//         result.push_back(def);
//     }
//     
//     return result;
// }
// 
// // ========== MCP 集成 ==========
// 
// void ToolManager::enable_mcp(const mcp::McporterConfig& config) {
//     if (!config.enabled) {
//         log()->info("MCP is disabled in config");
//         return;
//     }
//     
//     // 检查 mcporter 是否已安装
//     if (!mcp_impl::McporterClient::is_mcporter_installed()) {
//         log()->error("mcporter not installed. Please install: npm install -g mcporter");
//         return;
//     }
//     
//     log()->info("Enabling MCP integration...");
//     
//     // 创建 McporterClient
//     mcp_client_ = mcp_impl::create_mcporter_client(config);
//     
//     // 创建 McpToolRegistry
//     mcp_registry_ = mcp_impl::McpToolRegistry::create(mcp_client_);
//     
//     // 确保 daemon 启动
//     auto ensure_result = mcp_client_->ensure_daemon_started();
//     if (ensure_result.is_err()) {
//         log()->error("Failed to start mcporter daemon: {}", ensure_result.error);
//         mcp_client_.reset();
//         mcp_registry_.reset();
//         return;
//     }
//     
//     log()->info("MCP integration enabled successfully");
// }
// 
// core::Result<int> ToolManager::refresh_mcp_tools() {
//     if (!mcp_registry_) {
//         return core::Result<int>::Err("MCP not enabled");
//     }
//     
//     log()->info("Refreshing MCP tools...");
//     
//     // 发现并注册所有 MCP tools
//     auto result = mcp_registry_->discover_and_register(*this);
//     
//     if (result.is_ok()) {
//         log()->info("Refreshed {} MCP tools", result.value);
//     } else {
//         log()->error("Failed to refresh MCP tools: {}", result.error);
//     }
//     
//     return result;
// }
// 
// std::shared_ptr<hunterclaw::mcp::McporterClient> ToolManager::get_mcp_client() {
//     return mcp_client_;
// }
// 
// bool ToolManager::is_mcp_enabled() const {
//     return mcp_client_ != nullptr;
// }
// 
// // ========== Hook 系统 ==========
// 
// std::shared_ptr<ToolHookManager> ToolManager::get_hook_manager() {
//     return hook_manager_;
// }
// 
// void ToolManager::enable_builtin_hooks(bool logging, bool performance) {
//     if (!hook_manager_) {
//         hook_manager_ = std::make_shared<ToolHookManager>();
//     }
//     
//     // 参数验证 Hook (总是启用)
//     hook_manager_->register_before_hook("param_validation", 
//         builtin_hooks::create_param_validation_hook());
//     
//     // 日志记录 Hook
//     if (logging) {
//         hook_manager_->register_after_hook("logging",
//             builtin_hooks::create_logging_hook());
//     }
//     
//     // 性能监控 Hook
//     if (performance) {
//         hook_manager_->register_after_hook("performance",
//             builtin_hooks::create_performance_monitoring_hook());
//         hook_manager_->register_after_hook("error_collection",
//             builtin_hooks::create_error_collection_hook());
//     }
//     
//     log()->info("Enabled builtin hooks: param_validation={}, logging={}, performance={}",
//         true, logging, performance);
// }
// 
// // ========== 循环检测 ==========
// 
// std::shared_ptr<ToolLoopDetector> ToolManager::get_loop_detector() {
//     return loop_detector_;
// }
// 
// void ToolManager::enable_loop_detection(bool enable) {
//     loop_detection_enabled_ = enable;
//     
//     if (enable && loop_detector_ && hook_manager_) {
//         // 注册循环检测 Hooks
//         hook_manager_->register_before_hook("loop_detection",
//             builtin_loop_detectors::create_loop_detection_hook(loop_detector_));
//         hook_manager_->register_after_hook("loop_recording",
//             builtin_loop_detectors::create_loop_recording_hook(loop_detector_));
//         
//         log()->info("Loop detection enabled");
//     } else if (!enable) {
//         // 移除循环检测 Hooks
//         if (hook_manager_) {
//             hook_manager_->unregister_before_hook("loop_detection");
//             hook_manager_->unregister_after_hook("loop_recording");
//         }
//         log()->info("Loop detection disabled");
//     }
// }
// 
// // ========== 策略管道 ==========
// 
// std::shared_ptr<PolicyPipeline> ToolManager::get_policy_pipeline() {
//     return policy_pipeline_;
// }
// 
// void ToolManager::enable_policy_pipeline(bool enable) {
//     policy_enabled_ = enable;
//     log()->info("Policy pipeline {}", enable ? "enabled" : "disabled");
// }
// 
// void ToolManager::apply_profile(const std::string& profile_name) {
//     if (!policy_pipeline_) {
//         policy_pipeline_ = std::make_shared<PolicyPipeline>();
//     }
//     
//     ToolProfile profile;
//     
//     if (profile_name == "restricted") {
//         profile = profiles::create_restricted_profile();
//     } else if (profile_name == "standard") {
//         profile = profiles::create_standard_profile();
//     } else if (profile_name == "developer") {
//         profile = profiles::create_developer_profile();
//     } else if (profile_name == "production") {
//         profile = profiles::create_production_profile();
//     } else {
//         log()->error("Unknown profile: {}", profile_name);
//         return;
//     }
//     
//     // 注册 Profile 的所有策略
//     for (const auto& policy : profile.policies) {
//         policy_pipeline_->register_policy(policy);
//     }
//     
//     policy_enabled_ = true;
//     
//     log()->info("Applied policy profile: {} ({})", profile.name, profile.description);
// }
// 
// // ========== 结果处理 ==========
// 
// std::shared_ptr<ResultProcessingPipeline> ToolManager::get_result_pipeline() {
//     return result_pipeline_;
// }
// 
// void ToolManager::enable_result_normalization(bool enable) {
//     result_normalization_enabled_ = enable;
//     
//     if (enable && result_pipeline_) {
//         // 添加默认处理器
//         ResultNormalizerConfig config;
//         config.sanitize_images = true;
//         config.remove_sensitive_data = true;
//         
//         result_pipeline_->add_processor("normalizer",
//             builtin_processors::create_normalizer(config));
//         
//         log()->info("Result normalization enabled");
//     } else if (!enable && result_pipeline_) {
//         result_pipeline_->remove_processor("normalizer");
//         log()->info("Result normalization disabled");
//     }
// }
// 
// // ========== 审批系统 ==========
// 
// std::shared_ptr<ApprovalManager> ToolManager::get_approval_manager() {
//     return approval_manager_;
// }
// 
// void ToolManager::set_approval_manager(std::shared_ptr<ApprovalManager> manager) {
//     approval_manager_ = manager;
//     log()->info("Approval manager set");
// }
// 
// void ToolManager::enable_approval_system(const std::string& config_path) {
//     if (!approval_manager_) {
//         approval_manager_ = ApprovalManager::create(config_path);
//     }
//     log()->info("Approval system enabled with config: {}", 
//                 config_path.empty() ? "default" : config_path);
// }
// 
// Tool结果辅助函数实现
namespace tool_result {

ToolResult text(const std::string& content) {
    ToolResult result;
    result.success = true;

    ToolResultContent text_content;
    text_content.type = ToolResultContent::Type::Text;
    text_content.text = content;
    result.content.push_back(text_content);

    return result;
}

ToolResult json(const core::JsonValue& data) {
    ToolResult result;
    result.success = true;
    result.details = data;

    ToolResultContent json_content;
    json_content.type = ToolResultContent::Type::Json;
    json_content.text = data.to_string();
    result.content.push_back(json_content);

    return result;
}

ToolResult image(const std::string& base64_data, const std::string& mime_type) {
    ToolResult result;
    result.success = true;

    ToolResultContent img_content;
    img_content.type = ToolResultContent::Type::Image;
    img_content.data = base64_data;
    img_content.mime_type = mime_type;
    result.content.push_back(img_content);

    return result;
}

ToolResult error(const std::string& message) {
    ToolResult result;
    result.success = false;
    result.error = message;

    ToolResultContent text_content;
    text_content.type = ToolResultContent::Type::Text;
    text_content.text = "Error: " + message;
    result.content.push_back(text_content);

    return result;
}

} // namespace tool_result
} // namespace tools
} // namespace hunterclaw
