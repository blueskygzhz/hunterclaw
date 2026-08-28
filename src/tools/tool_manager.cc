#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/tools/tool_hooks.h"
#include "hunterclaw/tools/before_tool_call_policy.h"
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
    
    // ✅ 对齐 OpenClaw: 实现 JsonValue 的成员访问
    if (!params_.is_object() || !params_.has(key)) {
        if (required) {
            throw std::runtime_error("Required parameter '" + key + "' missing");
        }
        return default_val;
    }
    const auto& val = params_[key];
    if (val.is_string()) return val.as_string();
    if (val.is_number()) return std::to_string(val.as_number());
    if (val.is_bool()) return val.as_bool() ? "true" : "false";
    if (required) throw std::runtime_error("Parameter '" + key + "' is not a string");
    return default_val;}

double ToolParams::read_number(const std::string& key, bool required, double default_val) const {
    if (!params_.is_object() || !params_.has(key)) {
        if (required) throw std::runtime_error("Required parameter '" + key + "' missing");
        return default_val;
    }
    const auto& val = params_[key];
    if (val.is_number()) return val.as_number();
    if (val.is_string()) { try { return std::stod(val.as_string()); } catch (...) {} }
    if (required) throw std::runtime_error("Parameter '" + key + "' is not a number");
    return default_val;
}

int64_t ToolParams::read_int(const std::string& key, bool required, int64_t default_val) const {
    if (!params_.is_object() || !params_.has(key)) {
        if (required) throw std::runtime_error("Required parameter '" + key + "' missing");
        return default_val;
    }
    const auto& val = params_[key];
    if (val.is_number()) return static_cast<int64_t>(val.as_number());
    if (val.is_string()) { try { return std::stoll(val.as_string()); } catch (...) {} }
    if (required) throw std::runtime_error("Parameter '" + key + "' is not an integer");
    return default_val;
}

bool ToolParams::read_bool(const std::string& key, bool required, bool default_val) const {
    if (!params_.is_object() || !params_.has(key)) {
        if (required) throw std::runtime_error("Required parameter '" + key + "' missing");
        return default_val;
    }
    const auto& val = params_[key];
    if (val.is_bool()) return val.as_bool();
    if (val.is_string()) {
        const auto& s = val.as_string();
        if (s == "true" || s == "1" || s == "yes") return true;
        if (s == "false" || s == "0" || s == "no") return false;
    }
    if (val.is_number()) return val.as_number() != 0.0;
    if (required) throw std::runtime_error("Parameter '" + key + "' is not a boolean");
    return default_val;
}

std::vector<std::string> ToolParams::read_string_array(const std::string& key, bool required) const {
    if (!params_.is_object() || !params_.has(key)) {
        if (required) throw std::runtime_error("Required parameter '" + key + "' missing");
        return {};
    }
    const auto& val = params_[key];
    std::vector<std::string> result;
    if (val.is_array()) {
        for (size_t i = 0; i < val.size(); ++i) {
            const auto& item = val[i];
            if (item.is_string()) result.push_back(item.as_string());
            else if (item.is_number()) result.push_back(std::to_string(item.as_number()));
        }
    } else if (val.is_string()) {
        result.push_back(val.as_string());
    }
    return result;
}

bool ToolParams::has(const std::string& key) const {
    // ✅ 对齐 OpenClaw: 实现 JsonValue 的 has 方法
    return params_.is_object() && params_.has(key);
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
    return execute_tool(name, params, std::move(on_update), ExecuteOptions{});
}

ToolResult ToolManager::execute_tool(
    const std::string& name,
    const core::JsonValue& params,
    ToolUpdateCallback on_update,
    const ExecuteOptions& options
) {
    auto start = std::chrono::steady_clock::now();
    
    // 1. 查找工具
    auto tool = get_tool(name);
    if (!tool) {
        return tool_result::error("Tool not found: " + name);
    }
    
    // 2. 策略检查 (100% 对齐 OpenClaw)
    //
    // ★★ 审批【已移出本段】, 现为 before_tool_call 链的 Stage 1
    //   (before_tool_call_policy.cc 的 run_approval_gate)。原因有两条:
    //
    //   ① 本段被 `policy_enabled_ && policy_pipeline_` 门控, 而
    //      policy_enabled_ 默认 false —— 默认部署下审批链路根本不存在。
    //      安全门禁不能挂在一个默认关闭的开关后面。
    //   ② 审批语义(四种 ApprovalMode / 三类 denied_reason)只该实现一份。
    //
    //   本段现在只保留 DENY 与「把 REQUIRE_APPROVAL 转交策略链」两件事。
    bool policy_requires_approval = false;
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
            // ★ 只记录, 不在此处发起审批 —— 交给下面 before_tool_call 链的
            //   Stage 1 统一处理。这样审批只有【一个】执行点:
            //     · 不会与 react_engine 已跑过的链重复问用户
            //     · 不受 policy_enabled_ 门控 (工具自声明的审批照样生效)
            //     · 四种 ApprovalMode / denied_reason 语义只实现一份
            policy_requires_approval = true;
        }
    }
    
    // 3. Before Hooks (参数修改和验证) — 严格对齐 OpenClaw runBeforeToolCallHook()
    // 🎯 owner-only 门禁 (Stage 0) 需在无插件 hook 时也执行, 因此 policy 链
    // 无条件运行; hook 阶段在hook_manager 为空时内部跳过。
    //
    // ⚠⚠ options.before_tool_call_applied=true 时【整段跳过】:
    //   调用方(react_engine)已经跑过同一条链。重复跑的后果:
    //     · 审批被问两遍 —— 用户点了同意又被弹一次
    //     · before_tool_call 插件 hook 副作用执行两次
    //     · 参数被 hook 改写两轮, 第二轮基于已改写的值
    //   跳过是安全的: 调用方跑的是【同一个函数】, 门禁强度完全相同,
    //   且它已按结果决定是否继续（Veto/Failure 时根本不会走到这里）。
    core::JsonValue effective_params = params;
    if (!options.before_tool_call_applied) {
        BeforeToolCallContext policy_ctx;
        policy_ctx.tool_name = name;
        policy_ctx.agent_id = context_.agent_id;
        policy_ctx.session_key = context_.session_key;
        policy_ctx.run_id = context_.run_id;
        policy_ctx.session_id = context_.session_id;
        // 🎯 对齐 OpenClaw senderIsOwner +ownerOnly gate
        policy_ctx.sender_is_owner = context_.sender_is_owner;
        policy_ctx.tool_is_owner_only = tool->owner_only();
        // 🎯 对齐 OpenClaw needsApproval / trustedPolicyResult.requireApproval:
        //   工具自声明 与 策略判定 两个来源取"或", 由 Stage 1 统一裁决。
        // ⚠ tool_requires_approval 此前【零消费】—— 声明要审批的工具从不审批。
        policy_ctx.tool_requires_approval = tool->requires_approval();
        policy_ctx.policy_requires_approval = policy_requires_approval;
        // ★ 与 react_engine 一致: 工具可覆盖审批模式与可选决策集。
        policy_ctx.approval_mode = parse_approval_mode(tool->approval_mode());
        policy_ctx.allowed_decisions = tool->approval_allowed_decisions();
        // ★ approval_broker 留空 —— 策略链会自动从进程级
        //   ToolApprovalRuntime::instance() 取。
        //   ⚠ 不在这里按会话注入: ToolManager 是每会话创建的,
        //     按会话持有 broker 会产生 N 个待审列表(见 tool_manager.h 说明)。
        policy_ctx.hook_manager = hook_manager_ ? hook_manager_.get() : nullptr;

        auto outcome = run_before_tool_call_policy(policy_ctx, params);
        if (outcome.kind == HookOutcomeKind::Veto) {
            log()->warn("Tool '{}' vetoed by before_tool_call policy: {}",
                        name, outcome.reason.value_or(""));
            return build_blocked_tool_result(
                outcome.reason.value_or("blocked by plugin hook"),
                outcome.denied_reason.value_or(denied_reason::kPluginBeforeToolCall));
        }
        if (outcome.kind == HookOutcomeKind::Failure) {
            log()->error("Tool '{}' before_tool_call FAILURE ({}): {}",
                         name, outcome.disposition.value_or(disposition::kError),
                         outcome.reason.value_or(""));
            return tool_result::error(outcome.reason.value_or("before_tool_call failed"));
        }
        effective_params = std::move(outcome.params);

        // 对齐 OpenClaw validateToolExecutionParams(): hook 改写后二次 schema 校验
        std::string verr = validate_tool_execution_params(*tool, effective_params);
        if (!verr.empty()) {
            log()->warn("Tool '{}' params invalid after before_tool_call: {}", name, verr);
            return tool_result::error("invalid tool params: " + verr);
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
        after_event.params_json = effective_params.to_string();  // serialize to JSON string
        after_event.duration_ms = duration_ms;
        if (!error_msg.empty()) {
            after_event.error = error_msg;
        } else {
            core::JsonValue result_json = core::JsonValue::object();
            result_json["success"] = result.success;
            if (!result.error.empty()) result_json["error"] = result.error;
            after_event.result_json = result_json.to_string();  // serialize to JSON string
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
// ============================================================================
// 基础 Setter 实现(补齐头文件声明)
// ============================================================================

void ToolManager::set_policy_enabled(bool enabled) {
    policy_enabled_ = enabled;
}

void ToolManager::set_policy_pipeline(std::shared_ptr<PolicyPipeline> pipeline) {
    policy_pipeline_ = pipeline;
}

void ToolManager::set_approval_manager(std::shared_ptr<ApprovalManager> manager) {
    approval_manager_ = manager;
}

void ToolManager::set_hook_manager(std::shared_ptr<ToolHookManager> manager) {
    hook_manager_ = manager;
}

void ToolManager::set_loop_detector(std::shared_ptr<ToolLoopDetector> detector) {
    loop_detector_ = detector;
}

void ToolManager::set_result_processor(std::shared_ptr<ResultProcessingPipeline> processor) {
    result_processor_ = processor;
}

void ToolManager::set_mcp_registry(std::shared_ptr<void> registry) {
    mcp_registry_ = registry;
}

// ============================================================================
// MCP 集成实现
// ============================================================================
//
// 通过 McporterClient::create() 统一入口,返回 NativeMcpClient:
//   - 纯 C++ 实现,无外部依赖
//   - 支持 Stdio / StreamableHttp / SSE 三种传输
//
// ============================================================================

core::Result<int> ToolManager::enable_mcp(const mcp::McporterConfig& config) {
    if (!config.enabled) {
        log()->info("[ToolManager] MCP is disabled in config");
        return core::Result<int>::Ok(0);
    }

    log()->info("[ToolManager] Enabling MCP integration (NativeMcpClient)...");

    // 1. 创建 MCP 客户端 (NativeMcpClient)
    mcp_client_ = mcp_impl::McporterClient::create(config);
    if (!mcp_client_) {
        return core::Result<int>::Err("Failed to create MCP client");
    }

    // 2. 创建 McpToolRegistry
    mcp_registry_typed_ = mcp_impl::McpToolRegistry::create(mcp_client_);

    // 3. 确保 daemon/连接池已启动
    auto ensure_result = mcp_client_->ensure_daemon_started();
    if (ensure_result.is_err()) {
        log()->error("[ToolManager] Failed to ensure MCP daemon: {}", ensure_result.error);
        mcp_client_.reset();
        mcp_registry_typed_.reset();
        return core::Result<int>::Err("ensure_daemon failed: " + ensure_result.error);
    }

    // 4. 发现并注册所有 MCP 工具
    auto register_result = mcp_registry_typed_->discover_and_register(*this);
    if (register_result.is_err()) {
        log()->warn("[ToolManager] MCP tool discovery failed: {}", register_result.error);
        // 不返回错误:客户端可能可用但工具发现失败(例如 server 离线)
        // 保留 mcp_client_,允许后续 refresh
        return core::Result<int>::Ok(0);
    }

    int count = register_result.value.value_or(0);
    log()->info("[ToolManager] MCP enabled: registered {} tools", count);
    return core::Result<int>::Ok(count);
}

core::Result<int> ToolManager::refresh_mcp_tools(
    const std::vector<std::string>& server_names
) {
    if (!mcp_registry_typed_) {
        return core::Result<int>::Err("MCP not enabled (call enable_mcp first)");
    }

    log()->info("[ToolManager] Refreshing MCP tools...");

    auto result = mcp_registry_typed_->discover_and_register(*this, server_names);
    if (result.is_ok()) {
        log()->info("[ToolManager] Refreshed {} MCP tools", result.value.value_or(0));
    } else {
        log()->error("[ToolManager] Failed to refresh MCP tools: {}", result.error);
    }
    return result;
}

std::shared_ptr<mcp_impl::McporterClient> ToolManager::get_mcp_client() const {
    return mcp_client_;
}

std::shared_ptr<mcp_impl::McpToolRegistry> ToolManager::get_mcp_registry() const {
    return mcp_registry_typed_;
}

bool ToolManager::is_mcp_enabled() const {
    return mcp_client_ != nullptr;
}

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
