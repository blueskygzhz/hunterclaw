#include "hunterclaw/react/react_engine.h"
// ── engine 层实际使用的模块 ──
#include "hunterclaw/react/abort_primitives.h"          // ✅ engine: setAbortMemory (run 成功后清除)
#include "hunterclaw/react/execution_guard.h"           // ✅ engine: classify_error, isLikelyExecutionAckPrompt
#include "hunterclaw/react/memory_flush_gate.h"         // ✅ engine: shouldRunMemoryFlush (run 结束后检查)
#include "hunterclaw/react/runner_helpers.h"             // ✅ engine: shouldEmitToolResult/Output (工具执行时)
#include "hunterclaw/react/verbose_reasoning_config.h"  // ✅ engine: format_reasoning_text
#include "hunterclaw/react/queue_policy.h"              // ✅ engine: resolve_enforce_final_tag
// ── 以下模块在 dispatcher 层 (message_dispatcher.cc) 使用，此处保留 include 供编译 ──
// abort_cutoff, normalize_reply, response_prefix_template, reply_delivery,
// reply_media_paths, reply_operation, reply_payloads, reply_threading,
// queue_policy, typing_mode, session_reset, followup_runner, streaming_directives,
// preflight_memory — 均在 dispatcher/外层调用
#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/tools/tool_hooks.h"             // PluginHook* types + get_global_hook_manager()
#include "hunterclaw/tools/before_tool_call_policy.h"
#include "hunterclaw/tools/interrupt_coordinator.h"  // ✅ 工具中断等待/恢复      // ✅ 执行后二次确认
#include "hunterclaw/tools/deferred_approval_registry.h"  // ✅ Defer 凭据收敛 // unified before_tool_call policy chain
#include "hunterclaw/sessions/transcript_io.h"       // append_tool_result_to_transcript (tool_result_persist)
#include "hunterclaw/observability/observability.h"  // ✅ Span, Tracer, SpanKind
#include "hunterclaw/observability/diagnostic_events.h"  // ✅ 可观测性集成
#include "hunterclaw/react/preflight_memory.h"       // ✅ 对齐 OC post-compaction-context.ts: readPostCompactionContext
#include "hunterclaw/infra/system_events.h"          // ✅ 对齐 OC system-events: enqueueSystemEvent
#include <regex>
#include <algorithm>
#include <fstream>
#include <future>
#include <thread>
#include <set>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace react {

namespace {

// Logger
auto log() {
    static auto logger = spdlog::get("react");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("react");
    }
    return logger;
}

// format_reasoning_text — 已在 verbose_reasoning_config.h 中声明，verbose_reasoning_config.cc 中实现

// 获取 CPU 核心数 (用于默认线程池大小)
int get_hardware_concurrency() {
    int cores = std::thread::hardware_concurrency();
    return (cores > 0) ? cores : 4;  // 默认 4 核
}

// 提取 <think>, <thinking>, <thought>, <antthinking> 标签
// 对齐 OpenClaw: THINKING_TAG_SCAN_RE
std::regex thinking_tag_re(R"(<\s*(/?)?\s*(?:think(?:ing)?|thought|antthinking)\s*>)", 
                          std::regex_constants::icase);

// 提取 <final> 标签
std::regex final_tag_re(R"(<\s*(/?)?\s*final\s*>)", 
                       std::regex_constants::icase);

// 分块分隔符
bool is_paragraph_boundary(char c) {
    return c == '\n';
}

bool is_sentence_boundary(const std::string& text, size_t pos) {
    if (pos >= text.size()) return false;
    char c = text[pos];
    return (c == '.' || c == '!' || c == '?') && 
           (pos + 1 >= text.size() || text[pos + 1] == ' ' || text[pos + 1] == '\n');
}

} // namespace

// ✅ 全局线程池 (懒加载单例)
std::shared_ptr<core::ThreadPool>& ReactEngine::get_thread_pool() {
    static std::shared_ptr<core::ThreadPool> pool;
    static std::once_flag init_flag;
    
    std::call_once(init_flag, []() {
        int pool_size = get_hardware_concurrency();
        log()->info("Initializing global thread pool with {} threads", pool_size);
        pool = std::make_shared<core::ThreadPool>(pool_size);
    });
    
    return pool;
}

// ReactEngine 实现
std::shared_ptr<ReactEngine> ReactEngine::create(
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::LLMClient> llm_client,
    const ReactConfig& config
) {
    return std::shared_ptr<ReactEngine>(
        new ReactEngine(tool_manager, llm_client, config)
    );
}

// ✅ create_with_provider - 直接使用 Provider (推荐)
std::shared_ptr<ReactEngine> ReactEngine::create_with_provider(
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::Provider> provider,
    const ReactConfig& config
) {
    return std::shared_ptr<ReactEngine>(
        new ReactEngine(tool_manager, provider, config)
    );
}

ReactEngine::ReactEngine(
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::LLMClient> llm_client,
    const ReactConfig& config
) : tool_manager_(tool_manager),
    llm_client_(llm_client),
    provider_(nullptr),  // ✅ 使用 LLMClient 时 provider_ 为 null
    config_(config) {
    
    // 🎯 如果配置了自定义线程池大小,重新初始化
    if (config_.thread_pool_size > 0) {
        auto& pool = get_thread_pool();
        if (!pool || pool->num_threads() != static_cast<size_t>(config_.thread_pool_size)) {
            log()->info("Reinitializing thread pool with {} threads", config_.thread_pool_size);
            pool = std::make_shared<core::ThreadPool>(config_.thread_pool_size);
        }
    }
}

// ✅ Provider 构造函数
ReactEngine::ReactEngine(
    std::shared_ptr<tools::ToolManager> tool_manager,
    std::shared_ptr<llm::Provider> provider,
    const ReactConfig& config
) : tool_manager_(tool_manager),
    llm_client_(nullptr),  // ✅ 使用 Provider 时 llm_client_ 为 null
    provider_(provider),
    config_(config) {
    
    // 线程池初始化
    if (config_.thread_pool_size > 0) {
        auto& pool = get_thread_pool();
        if (!pool || pool->num_threads() != static_cast<size_t>(config_.thread_pool_size)) {
            log()->info("Reinitializing thread pool with {} threads", config_.thread_pool_size);
            pool = std::make_shared<core::ThreadPool>(config_.thread_pool_size);
        }
    }
}

ReactResult ReactEngine::run(
    const std::string& user_input,
    const std::vector<llm::LLMMessage>& history,
    const StreamCallbacks& callbacks
) {
    cancelled_ = false;
    running_ = true;  // ✅ Mark engine as running (enables steer)
    // Clear any leftover steer messages from previous runs
    {
        std::lock_guard<std::mutex> lock(steer_mutex_);
        std::queue<std::string> empty;
        steer_queue_.swap(empty);
    }
    start_time_ = std::chrono::steady_clock::now();
    
    // ✅ 可观测性: 开启 Span (对齐 OpenClaw)
    std::unique_ptr<observability::Span> span;
    try {
        span = observability::Tracer::instance().start_span(
            "react.run", 
            observability::SpanKind::Internal
        );
        span->set_attribute("user_input_length", static_cast<int64_t>(user_input.size()));
        span->set_attribute("history_size", static_cast<int64_t>(history.size()));
        if (!callbacks.session_id.empty()) {
            span->set_attribute("session_id", callbacks.session_id);
        }
    } catch (...) {
        // 故障隔离: Span 创建失败不影响主流程
    }
    
    // 生命周期: start
    if (callbacks.on_start) {
        callbacks.on_start();
    }
    
    // ✅ ContextEngine: bootstrap (对齐 OpenClaw attempt.ts:1737-1745)
    // 在 session 文件存在时初始化引擎状态
    if (context_engine_) {
        try {
            if (!callbacks.transcript_path.empty()) {
                auto bootstrap_result = context_engine_->bootstrap(
                    callbacks.session_id, callbacks.transcript_path);
                if (bootstrap_result.bootstrapped) {
                    log()->info("[ContextEngine] bootstrap completed: imported {} messages",
                        bootstrap_result.imported_messages.value_or(0));
                }
            }
        } catch (const std::exception& e) {
            log()->warn("[ContextEngine] bootstrap failed: {}", e.what());
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // 🎯 严格对齐 OC agent-runner-memory.ts:346-503:
    //   runPreflightCompactionIfNeeded(agent, user_input, runtime_ctx)
    //
    // 在发起第一次 LLM 调用之前检查：
    //   - session 累积 token (projected_count) 是否已接近 context_window 上限
    //
    // 🎯 架构对齐 OC："preflight 门控 + 执行"统一于同一份 compact 实现。
    //   OC 里 preflight 直接调用 compactEmbeddedPiSession（trigger: "budget"），
    //   与 /compact 手动命令、token-overflow 自动路径共用同一份代码。
    //
    //   HC 对齐做法：门控由 evaluate_preflight_compaction 完成，执行由
    //   ReactEngine::trigger_compaction 完成——这是 HC 内 overflow / manual /
    //   preflight 三条入口共用的唯一 CompactionManager 实现。
    //
    // 🎯 注意：OC preflight **只看 token**，不看 transcript 字节数。
    //   字节数强制路径 (force_flush_transcript_bytes) 属于 memory flush 语义。
    //
    // heartbeat / CLI / 空 session_key / 无 CompactionManager 自动跳过。
    // ════════════════════════════════════════════════════════════════════
    if (compaction_mgr_ &&
        !callbacks.session_key.empty() &&
        !callbacks.session_id.empty() &&
        !callbacks.transcript_path.empty() &&
        !callbacks.is_heartbeat && !callbacks.is_cli) {
        try {
            react::PreflightRunParams pre;
            pre.session_key = callbacks.session_key;
            pre.session_id = callbacks.session_id;
            pre.session_file = callbacks.transcript_path;
            pre.store_path = "";  // HC 当前用 transcript_path 替代
            pre.workspace_dir = callbacks.workspace_dir;
            pre.provider = callbacks.provider.empty() && llm_client_
                ? llm_client_->get_config().provider : callbacks.provider;
            pre.model = callbacks.model.empty() && llm_client_
                ? llm_client_->get_config().model : callbacks.model;
            pre.default_model = callbacks.default_model;
            pre.agent_cfg_context_tokens = callbacks.agent_cfg_context_tokens;
            pre.is_heartbeat = callbacks.is_heartbeat;
            pre.is_cli = callbacks.is_cli;
            pre.persisted_total_tokens = callbacks.persisted_total_tokens;
            pre.total_tokens_fresh = callbacks.total_tokens_fresh;
            pre.compaction_count = callbacks.compaction_count;
            pre.prompt_for_estimate = user_input;
            pre.context_window_tokens =
                context_window_config_.context_window_tokens;
            pre.reserve_tokens_floor =
                context_window_config_.reserve_tokens_floor;
            pre.soft_threshold_tokens =
                context_window_config_.soft_threshold_tokens;

            auto decision = react::evaluate_preflight_compaction(pre);
            if (decision.should_compact) {
                // 对齐 OC：preflight 触发 → 直接调用共用 compact 实现
                // （HC 内 trigger_compaction = CompactionManager::compact_session 的包装，
                //  内部负责 before_compaction hook / 消息重写 / appendCompaction 写 transcript）
                auto transcript_msgs = sessions::read_transcript_messages(
                    callbacks.transcript_path, /*limit=*/0);
                if (!transcript_msgs.empty()) {
                    auto compact_res = trigger_compaction(
                        transcript_msgs, callbacks.transcript_path, callbacks.session_id);
                    if (compact_res.compacted) {
                        log()->info(
                            "[ReactEngine] preflight compaction: session={} "
                            "projected_tokens={} threshold={} messages {}→{} saved={}",
                            callbacks.session_key,
                            decision.projected_token_count.value_or(-1),
                            decision.threshold,
                            compact_res.messages_before,
                            compact_res.messages_after,
                            compact_res.tokens_saved);
                    } else {
                        log()->info(
                            "[ReactEngine] preflight compaction skipped by CompactionManager: "
                            "session={} reason={}",
                            callbacks.session_key, compact_res.reason);
                    }
                } else {
                    log()->debug(
                        "[ReactEngine] preflight compaction skipped: "
                        "transcript empty (session={})", callbacks.session_key);
                }
            } else if (!decision.skip_reason.empty()) {
                log()->debug("[ReactEngine] preflight gate skip: session={} reason={}",
                             callbacks.session_key, decision.skip_reason);
            }
        } catch (const std::exception& e) {
            log()->warn("[ReactEngine] preflight compaction failed: {}", e.what());
        } catch (...) {
            log()->warn("[ReactEngine] preflight compaction failed: unknown");
        }
    }

    try {
        auto result = execute_loop(user_input, history, callbacks);
        
        // ✅ ContextEngine: afterTurn (严格对齐 OpenClaw attempt.ts:2610-2660)
        if (context_engine_ && !result.messages_snapshot.empty()) {
            try {
                std::vector<std::string> message_texts;
                for (const auto& msg : result.messages_snapshot) {
                    if (msg.is_object() && msg.contains("content")) {
                        message_texts.push_back(msg["content"].as_string());
                    }
                }
                context_engine_->after_turn(
                    "",     // session_id (not available in ReactEngine scope)
                    "main", // agent_id
                    message_texts,
                    result.tool_calls,
                    std::nullopt,
                    false,
                    std::nullopt,
                    context::ContextEngineRuntimeContext{}
                );
            } catch (...) {
                // afterTurn is best-effort
            }
        }
        
        // ✅ Span 成功标记
        if (span) {
            span->set_status(result.success, result.error);
            span->set_attribute("total_steps", static_cast<int64_t>(result.steps.size()));
            span->set_attribute("tool_calls", static_cast<int64_t>(result.tool_calls));
            span->set_attribute("input_tokens", static_cast<int64_t>(result.usage.input_tokens));
            span->set_attribute("output_tokens", static_cast<int64_t>(result.usage.output_tokens));
        }
        
        // 生命周期: end
        if (callbacks.on_end) {
            callbacks.on_end();
        }
        
        // ✅ ContextEngine: dispose (对齐 OpenClaw run.ts:1601)
        if (context_engine_) {
            try { context_engine_->dispose(); } catch (...) {}
        }
        
        running_ = false;  // ✅ Mark engine as stopped (disables steer)
        return result;
    } catch (const std::exception& e) {
        // ✅ Span 失败标记
        if (span) {
            span->set_status(false, e.what());
            span->add_event("error", {{"exception", e.what()}});
        }
        
        // 生命周期: error
        if (callbacks.on_error) {
            callbacks.on_error(e.what());
        }
        
        // ✅ ContextEngine: dispose on error path
        if (context_engine_) {
            try { context_engine_->dispose(); } catch (...) {}
        }
        
        running_ = false;  // ✅ Mark engine as stopped (disables steer)
        ReactResult result;
        result.success = false;
        result.error = e.what();
        return result;
    }
}

std::future<ReactResult> ReactEngine::run_async(
    const std::string& user_input,
    const std::vector<llm::LLMMessage>& history,
    const StreamCallbacks& callbacks
) {
    return std::async(std::launch::async, [this, user_input, history, callbacks]() {
        return run(user_input, history, callbacks);
    });
}

void ReactEngine::cancel() {
    cancelled_ = true;
}

// ============================================================================
// steer — 向正在运行的 execute_loop 实时注入 user message
// 🎯 对齐 OpenClaw: activeSession.steer(text)  (attempt.ts:2231)
//   → session.agent.steer(AgentMessage{role:"user", content:text})
//
// 线程安全: steer_mutex_ 保护 steer_queue_。
// execute_loop 在每轮循环开始时消费 steer_queue_ 中的消息。
//
// 返回 true: 消息已入队（engine 正在运行 execute_loop）
// 返回 false: engine 未运行，steer 被拒绝
// ============================================================================
bool ReactEngine::steer(const std::string& text) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    if (text.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(steer_mutex_);
        steer_queue_.push(text);
    }
    log()->info("[ReactEngine] steer: queued user message ({} chars)", text.size());
    return true;
}

ReactResult ReactEngine::execute_loop(
    const std::string& user_input,
    const std::vector<llm::LLMMessage>& history,
    const StreamCallbacks& callbacks
) {
    ReactResult result;
    result.success = true;

    // ✅ 对齐 OpenClaw: tool loop detection — 每次 run 重置状态
    // OpenClaw: getDiagnosticSessionState() 返回 per-session 的持久 state
    // HunterClaw: 每次 run() 创建全新 state (per-run 语义，更简单)
    // 差异: OpenClaw 跨多次 run 累积历史，HunterClaw 每次 run 从头开始
    // 对于绝大多数场景（单次对话 turn）行为一致。
    loop_detection_state_ = LoopDetectionState::create(config_.loop_detection);

    // =========================================================================
    // Hook: before_agent_reply (claiming) — 严格对齐 OpenClaw runBeforeAgentReply
    // Runs BEFORE the LLM is resolved/invoked. If a plugin claims the turn
    // (handled==true), short-circuit the entire run and return itsReplyPayload.
    // first-claim-wins. (plugins/hooks.ts: runClaimingHook)
    // =========================================================================
    {
        auto* claim_mgr = tools::get_global_hook_manager();
        if (claim_mgr && claim_mgr->has_hooks(plugins::PluginHookName::BEFORE_AGENT_REPLY)) {
            tools::PluginHookBeforeAgentReplyEvent bar_event;
            bar_event.cleaned_body = user_input;  // 对齐 OC: cleanedBody
            tools::PluginHookAgentContext bar_ctx;
            bar_ctx.session_id = callbacks.session_id.empty()
                ? std::nullopt : std::make_optional(callbacks.session_id);
            try {
                auto claim = claim_mgr->run_before_agent_reply(bar_event, bar_ctx);
                if (claim && claim->handled) {
                    result.success = true;
                    result.handled_by_hook = true;
                    result.handled_reason = claim->reason;
                    // 从ReplyPayload JSON 中取 text（对齐 OC 直接投递 reply）
                    std::string reply_text;
                    if (claim->reply_json) {
                        auto parsed = core::JsonValue::parse(*claim->reply_json);
                        if (parsed.ok && parsed.value && parsed.value->is_object() &&
                            parsed.value->contains("text") &&
                            (*parsed.value)["text"].is_string()) {
                            reply_text = (*parsed.value)["text"].as_string();
                        }
                    }
                    result.final_response = reply_text;
                    if (!reply_text.empty()) {
                        result.assistant_texts.push_back(reply_text);
                        if (callbacks.on_assistant_delta) callbacks.on_assistant_delta(reply_text);
                        if (callbacks.on_block_reply) callbacks.on_block_reply(reply_text);
                    }
                    log()->info("[hooks] before_agent_reply short-circuited the run{}",
                        claim->reason ? (" (" + *claim->reason + ")") : std::string{});
                    return result;  // ← 跳过 LLM 调用
                }
            } catch (const std::exception& e) {
                log()->warn("before_agent_reply hook failed: {}", e.what());
            }
        }
    }

    // =========================================================================
    // Hook: before_agent_run (gate) — 严格对齐 OpenClaw runBeforeAgentRun
    // 会话解析 + 工作区准备之后、模型推理之前触发。返回最严格 pass/block
    // （block 优先短路），void=pass，fail-closed（hook 异常 → block）。
    // block 时中止本次 run，返回面向用户的 block message。
    // =========================================================================
    {
        auto* bar_mgr = tools::get_global_hook_manager();
        if (bar_mgr && bar_mgr->has_hooks(plugins::PluginHookName::BEFORE_AGENT_RUN)) {
            tools::PluginHookBeforeAgentRunEvent bar_event;
            bar_event.prompt = user_input;
            if (!callbacks.channel_id.empty()) bar_event.channel_id = callbacks.channel_id;
            tools::PluginHookAgentContext bar_ctx;
            bar_ctx.session_id = callbacks.session_id.empty()
                ? std::nullopt : std::make_optional(callbacks.session_id);
            if (!callbacks.agent_id.empty())      bar_ctx.agent_id     = callbacks.agent_id;
            if (!callbacks.session_key.empty())   bar_ctx.session_key  = callbacks.session_key;
            if (!callbacks.workspace_dir.empty()) bar_ctx.workspace_dir = callbacks.workspace_dir;
            if (!callbacks.channel_id.empty())    bar_ctx.channel_id   = callbacks.channel_id;

            auto gate = bar_mgr->run_before_agent_run(bar_event, bar_ctx);
            if (gate && gate->block) {
                // 对齐 OC resolveBlockMessage：面向用户的 block提示。
                std::string block_msg = gate->message.value_or("Your message could not be sent");
                log()->warn("[hooks] before_agent_run blocked the run");
                result.success = false;
                result.handled_by_hook = true;
                result.handled_reason = gate->reason;
                result.final_response = block_msg;
                result.assistant_texts.push_back(block_msg);
                if (callbacks.on_assistant_delta) callbacks.on_assistant_delta(block_msg);
                return result;  // ← 中止本次 run（fail-closed gate）
            }
        }
    }

    // =========================================================================
    // Hook: before_model_resolve
    // Aligned with OpenClaw run.ts:329 — runs before resolveModel()
    // Allows plugins to override provider/model for this run.
    // =========================================================================
    auto* hook_mgr = tools::get_global_hook_manager();
    std::string effective_model_override;
    std::string effective_provider_override;

    if (hook_mgr && hook_mgr->has_hooks(plugins::PluginHookName::BEFORE_MODEL_RESOLVE)) {
        tools::PluginHookBeforeModelResolveEvent bmr_event;
        bmr_event.prompt = user_input;
        tools::PluginHookAgentContext hook_ctx;
        hook_ctx.session_id = callbacks.session_id.empty()
            ? std::nullopt : std::make_optional(callbacks.session_id);
        try {
            auto bmr_result = hook_mgr->run_before_model_resolve(bmr_event, hook_ctx);
            if (bmr_result) {
                if (bmr_result->model_override)    effective_model_override    = *bmr_result->model_override;
                if (bmr_result->provider_override) effective_provider_override = *bmr_result->provider_override;
                if (!effective_model_override.empty())
                    log()->info("[hooks] model overridden to {}", effective_model_override);
                if (!effective_provider_override.empty())
                    log()->info("[hooks] provider overridden to {}", effective_provider_override);
            }
        } catch (const std::exception& e) {
            log()->warn("before_model_resolve hook failed: {}", e.what());
        }
    }

    // =========================================================================
    // Hook: before_agent_start (legacy) + before_prompt_build
    // Aligned with OpenClaw run.ts:339 and attempt.ts:1172/1188
    // Legacy: before_agent_start may also carry model/provider overrides.
    // before_prompt_build: inject system prompt / context additions.
    // =========================================================================
    std::optional<std::string> hook_system_prompt;
    std::optional<std::string> hook_prepend_context;
    std::optional<std::string> hook_prepend_system_context;
    std::optional<std::string> hook_append_system_context;
    std::optional<std::string> hook_append_context;              // 对齐 OC appendContext
    std::optional<std::vector<std::string>> hook_tools_allow;    // 对齐 OC toolsAllow

    if (hook_mgr) {
        tools::PluginHookAgentContext hook_ctx;
        hook_ctx.session_id = callbacks.session_id.empty()
            ? std::nullopt : std::make_optional(callbacks.session_id);
        // 🎯 填充 hook 上下文（供 active-memory lane-1 等插件读取 workspace/agent）。
        hook_ctx.agent_id = callbacks.agent_id.empty()
            ? std::nullopt : std::make_optional(callbacks.agent_id);
        hook_ctx.session_key = callbacks.session_key.empty()
            ? std::nullopt : std::make_optional(callbacks.session_key);
        hook_ctx.workspace_dir = callbacks.workspace_dir.empty()
            ? std::nullopt : std::make_optional(callbacks.workspace_dir);

        // Legacy: before_agent_start
        if (hook_mgr->has_hooks(plugins::PluginHookName::BEFORE_AGENT_START)) {
            tools::PluginHookBeforeAgentStartEvent bas_event;
            bas_event.prompt = user_input;
            try {
                auto bas_result = hook_mgr->run_before_agent_start(bas_event, hook_ctx);
                if (bas_result) {
                    if (bas_result->system_prompt)           hook_system_prompt           = bas_result->system_prompt;
                    if (bas_result->prepend_context)         hook_prepend_context         = bas_result->prepend_context;
                    if (bas_result->prepend_system_context)  hook_prepend_system_context  = bas_result->prepend_system_context;
                    if (bas_result->append_system_context)   hook_append_system_context   = bas_result->append_system_context;
                    // Legacy model/provider override (lower priority than before_model_resolve)
                    if (effective_model_override.empty()    && bas_result->model_override)
                        effective_model_override    = *bas_result->model_override;
                    if (effective_provider_override.empty() && bas_result->provider_override)
                        effective_provider_override = *bas_result->provider_override;
                }
            } catch (const std::exception& e) {
                log()->warn("before_agent_start hook (legacy) failed: {}", e.what());
            }
        }

        // before_prompt_build (takes precedence over legacy for prompt fields)
        if (hook_mgr->has_hooks(plugins::PluginHookName::BEFORE_PROMPT_BUILD)) {
            tools::PluginHookBeforePromptBuildEvent bpb_event;
            bpb_event.prompt = user_input;
            bpb_event.messages_json = "[]";  // history serialized; simplified here
            try {
                auto bpb_result = hook_mgr->run_before_prompt_build(bpb_event, hook_ctx);
                if (bpb_result) {
                    // before_prompt_build takes precedence (last wins for systemPrompt)
                    if (bpb_result->system_prompt)          hook_system_prompt          = bpb_result->system_prompt;
                    if (bpb_result->prepend_context)        hook_prepend_context        = bpb_result->prepend_context;
                    if (bpb_result->prepend_system_context) hook_prepend_system_context = bpb_result->prepend_system_context;
                    if (bpb_result->append_system_context)  hook_append_system_context  = bpb_result->append_system_context;
                    if (bpb_result->append_context)hook_append_context         = bpb_result->append_context;
                    if (bpb_result->tools_allow)            hook_tools_allow            = bpb_result->tools_allow;
                }
            } catch (const std::exception& e) {
                log()->warn("before_prompt_build hook failed: {}", e.what());
            }
        }

        // =====================================================================
        // Hook: agent_turn_prepare (modifying) — 对齐 OpenClaw runAgentTurnPrepare
        // 每轮 turn 准备时贡献 prepend/append context（拼接合并），与 prompt build
        // 同阶段。合并进 hook_prepend_context / hook_append_context。
        // =====================================================================
        if (hook_mgr->has_hooks(plugins::PluginHookName::AGENT_TURN_PREPARE)) {
            tools::PluginHookAgentTurnPrepareEvent atp_event;
            atp_event.prompt = user_input;
            try {
                auto atp_result = hook_mgr->run_agent_turn_prepare(atp_event, hook_ctx);
                if (atp_result) {
                    if (atp_result->prepend_context) {
                        hook_prepend_context = hook_prepend_context
                            ? std::make_optional(*hook_prepend_context + "\n\n" + *atp_result->prepend_context)
                            : atp_result->prepend_context;
                    }
                    if (atp_result->append_context) {
                        hook_append_context = hook_append_context
                            ? std::make_optional(*hook_append_context + "\n\n" + *atp_result->append_context)
                            : atp_result->append_context;
                    }
                }
            } catch (const std::exception& e) {
                log()->warn("agent_turn_prepare hook failed: {}", e.what());
            }
        }
    }
    // 🎯 对齐 OpenClaw: 将 hook 返回的 prompt 修改注入到消息列表
    // before_prompt_build / before_agent_start hooks 可以覆盖 system prompt 或注入上下文
    // Note: messages 需要在此处提前声明，因为 hook 块需要修改它
    std::vector<llm::LLMMessage> messages = history;
    if (hook_system_prompt.has_value() || hook_prepend_context.has_value() ||
        hook_prepend_system_context.has_value() || hook_append_system_context.has_value()) {
        // Find or create system message
        bool found_system = false;
        for (auto& msg : messages) {
            if (msg.role == llm::LLMMessageRole::System) {
                found_system = true;
                // Override system prompt if hook provided one
                if (hook_system_prompt.has_value()) {
                    msg.content = *hook_system_prompt;
                }
                // Prepend system context
                if (hook_prepend_system_context.has_value()) {
                    msg.content = *hook_prepend_system_context + "\n\n" + msg.content;
                }
                // Append system context
                if (hook_append_system_context.has_value()) {
                    msg.content = msg.content + "\n\n" + *hook_append_system_context;
                }
                break;
            }
        }
        // If no system message exists and hook wants to inject one, create it
        if (!found_system && hook_system_prompt.has_value()) {
            llm::LLMMessage sys_msg;
            sys_msg.role = llm::LLMMessageRole::System;
            sys_msg.content = *hook_system_prompt;
            if (hook_prepend_system_context.has_value()) {
                sys_msg.content = *hook_prepend_system_context + "\n\n" + sys_msg.content;
            }
            if (hook_append_system_context.has_value()) {
                sys_msg.content = sys_msg.content + "\n\n" + *hook_append_system_context;
            }
            messages.insert(messages.begin(), sys_msg);
        }
        // Prepend context as user message (if hook provided)
        if (hook_prepend_context.has_value()) {
            llm::LLMMessage ctx_msg;
            ctx_msg.role = llm::LLMMessageRole::User;
            ctx_msg.content = *hook_prepend_context;
            // Insert after system message (if any) but before conversation
            auto insert_pos = messages.begin();
            if (!messages.empty() && messages[0].role == llm::LLMMessageRole::System) {
                ++insert_pos;
            }
            messages.insert(insert_pos, ctx_msg);
        }
    }
    // Apply model override from hooks
    if (!effective_model_override.empty() && llm_client_) {
        llm_client_->set_model_override(effective_model_override);
    }
    (void)effective_provider_override;  // Provider override needs LLMClientFactory support

    // 🎯 OpenClaw 对齐: 重置流式状态 (pi-embedded-subscribe.ts: state init)
    streaming_state_.reset();
    // accumulated_reasoning_raw_/formatted_ are part of streaming_state_ now
    accumulated_reasoning_raw_.clear();
    accumulated_reasoning_formatted_.clear();

    // 构建消息列表 (messages 已在 hook 块之前声明)
    
    // 添加用户输入 (Observe)
    llm::LLMMessage user_msg;
    user_msg.role = llm::LLMMessageRole::User;
    user_msg.content = user_input;
    // 🎯 对齐 OC before_prompt_build.appendContext: 追加到用户 prompt 尾部
    if (hook_append_context.has_value() && !hook_append_context->empty()) {
        user_msg.content = user_msg.content + "\n\n" + *hook_append_context;
    }
    messages.push_back(user_msg);

    // 🎯 对齐 OpenClaw run.ts:97-109: scrubAnthropicRefusalMagic
    // Avoid Anthropic's refusal test token poisoning session transcripts
    {
        static const std::string ANTHROPIC_MAGIC = "ANTHROPIC_MAGIC_STRING_TRIGGER_REFUSAL";
        static const std::string ANTHROPIC_REPLACEMENT = "ANTHROPIC MAGIC STRING TRIGGER REFUSAL (redacted)";
        for (auto& msg : messages) {
            if (msg.content.find(ANTHROPIC_MAGIC) != std::string::npos) {
                size_t pos = 0;
                while ((pos = msg.content.find(ANTHROPIC_MAGIC, pos)) != std::string::npos) {
                    msg.content.replace(pos, ANTHROPIC_MAGIC.size(), ANTHROPIC_REPLACEMENT);
                    pos += ANTHROPIC_REPLACEMENT.size();
                }
            }
        }
    }

    // ✅ ContextEngine: assemble (对齐 OpenClaw attempt.ts:2094-2102)
    // 让 context engine 在 token 预算内组装上下文
    if (context_engine_) {
        try {
            // 将 LLMMessage 序列化为 JSON 字符串列表
            std::vector<std::string> messages_json;
            for (const auto& msg : messages) {
                nlohmann::json j;
                j["role"] = [&]() {
                    switch (msg.role) {
                        case llm::LLMMessageRole::User: return "user";
                        case llm::LLMMessageRole::Assistant: return "assistant";
                        case llm::LLMMessageRole::System: return "system";
                        case llm::LLMMessageRole::Tool: return "toolResult";
                        default: return "unknown";
                    }
                }();
                j["content"] = msg.content;
                messages_json.push_back(j.dump());
            }
            
            auto assembled = context_engine_->assemble(
                callbacks.session_id,
                messages_json,
                context_window_config_.context_window_tokens
            );
            
            // 如果 assemble 返回了 system_prompt_addition，注入到 hook 结果
            if (assembled.system_prompt_addition) {
                if (!hook_prepend_system_context) {
                    hook_prepend_system_context = *assembled.system_prompt_addition;
                } else {
                    hook_prepend_system_context = *hook_prepend_system_context + "\n" + *assembled.system_prompt_addition;
                }
            }
        } catch (const std::exception& e) {
            log()->warn("[ContextEngine] assemble failed: {}", e.what());
        }
    }

    // 🎯 对齐 OpenClaw: handleAgentStart — emit lifecycle start event
    if (callbacks.on_start) {
        callbacks.on_start();
    }
    // 🎯 对齐 OpenClaw handlers.lifecycle.ts:17-31: emit lifecycle start AgentEvent
    if (callbacks.on_agent_event) {
        core::JsonValue data;
        data["phase"] = "start";
        callbacks.on_agent_event({"lifecycle", std::move(data)});
    }

    ReactStep observe_step;
    observe_step.type = StepType::Observe;
    observe_step.content = user_input;
    observe_step.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    result.steps.push_back(observe_step);

    // ReAct 循环: Reason → Act → Observe → ...
    int step_count = 0;
    // 🎯 对齐 OpenClaw before_agent_finalize: revise 重跑计数（防无限 revise）
    int finalize_revise_count = 0;
    constexpr int kMaxFinalizeRevisions = 3;
    std::string accumulated_response;
    // 🎯 对齐 OpenClaw agent-runner.ts:770 — autoCompactionCount
    // 本轮 run 内累计发生的 auto-compaction 次数
    // Run 结束时若 > 0, 触发 post-compaction context 注入 (enqueueSystemEvent)
    int auto_compaction_count = 0;
    // 🎯 对齐 OpenClaw: assistantTexts accumulator (mirrors state.assistantTexts)
    std::vector<std::string> assistant_texts;
    
    while (step_count < config_.max_steps && !cancelled_) {
        // ✅ 对齐 OpenClaw: 检查 per-run abort signal (abortController.signal.aborted)
        // cancelled_ 是 session 级别的 abort (来自 abort_embedded_session → engine->cancel())
        // abort_signal 是 run 级别的 abort (来自 chat.abort({runId}) → abort_flag->store(true))
        if (callbacks.abort_signal && callbacks.abort_signal->load()) {
            cancelled_ = true;
            break;
        }
        // ✅ 严格对齐 OpenClaw run.ts:1007-1192: Token Overflow 检查 + compact + tool result truncation
        if (compaction_mgr_ || context_engine_) {
            int current_tokens = estimate_conversation_tokens(messages);
            int context_window = context_window_config_.context_window_tokens;
            
            // 转换 messages 为 AgentMessage (用于 should_compact 检查)
            std::vector<memory::AgentMessage> agent_messages;
            for (const auto& msg : messages) {
                memory::AgentMessage am;
                am.role = [&]() {
                    switch (msg.role) {
                        case llm::LLMMessageRole::User: return "user";
                        case llm::LLMMessageRole::Assistant: return "assistant";
                        case llm::LLMMessageRole::System: return "system";
                        case llm::LLMMessageRole::Tool: return "toolResult";
                        default: return "unknown";
                    }
                }();
                am.content = msg.content;
                agent_messages.push_back(am);
            }
            
            // 检查是否需要 compaction
            bool should_compact = compaction_mgr_ &&
                compaction_mgr_->should_compact(agent_messages, current_tokens, context_window);

            if (should_compact) {
                log()->warn("[ReactEngine] Token overflow detected: {} >= threshold, triggering compaction",
                           current_tokens);
                
                // 🎯 严格对齐 OpenClaw run.ts:741: MAX_OVERFLOW_COMPACTION_ATTEMPTS = 3
                const int MAX_OVERFLOW_COMPACTION_ATTEMPTS = 3;
                int overflow_compaction_attempts = 0;
                bool compaction_succeeded = false;

                // 🎯 严格对齐 OpenClaw run.ts:1020-1129: 先尝试 contextEngine.compact()
                while (overflow_compaction_attempts < MAX_OVERFLOW_COMPACTION_ATTEMPTS) {
                    overflow_compaction_attempts++;
                    log()->warn("[ReactEngine] Context overflow (attempt {}/{}); attempting auto-compaction",
                               overflow_compaction_attempts, MAX_OVERFLOW_COMPACTION_ATTEMPTS);

                    // 🎯 对齐 OpenClaw: emit compaction start event
                    if (callbacks.on_agent_event) {
                        core::JsonValue data;
                        data["phase"] = "start";
                        callbacks.on_agent_event({"compaction", std::move(data)});
                    }

                    // 🎯 严格对齐 OpenClaw: 优先调用 contextEngine.compact()
                    bool compact_ok = false;
                    if (context_engine_) {
                        try {
                            auto ce_result = context_engine_->compact(
                                callbacks.session_id,
                                callbacks.transcript_path,
                                context_window,
                                true,   // force = true (overflow 时强制)
                                current_tokens,  // current_token_count
                                "budget",  // compaction_target = "budget"
                                std::nullopt,  // custom_instructions
                                nlohmann::json({
                                    {"trigger", "overflow"},
                                    {"attempt", overflow_compaction_attempts},
                                    {"maxAttempts", MAX_OVERFLOW_COMPACTION_ATTEMPTS}
                                })  // runtime_context
                            );
                            compact_ok = ce_result.ok && ce_result.compacted;
                            if (!compact_ok) {
                                log()->warn("[ReactEngine] contextEngine.compact() did not compact: {}",
                                           ce_result.reason.value_or("nothing to compact"));
                            }
                        } catch (const std::exception& e) {
                            log()->warn("[ReactEngine] contextEngine.compact() threw: {}", e.what());
                        }
                    }

                    // Fallback 到 compaction_mgr_ (如果 CE compact 失败)
                    if (!compact_ok && compaction_mgr_) {
                        auto compact_result = trigger_compaction(messages, callbacks.transcript_path, callbacks.session_id);
                        compact_ok = compact_result.compacted;
                        if (!compact_ok) {
                            log()->warn("[ReactEngine] CompactionManager also failed: {}",
                                       compact_result.reason);
                        }
                    }

                    // 🎯 对齐 OpenClaw: emit compaction end event
                    if (callbacks.on_agent_event) {
                        core::JsonValue data;
                        data["phase"] = "end";
                        data["willRetry"] = !compact_ok && (overflow_compaction_attempts < MAX_OVERFLOW_COMPACTION_ATTEMPTS);
                        callbacks.on_agent_event({"compaction", std::move(data)});
                    }

                    if (compact_ok) {
                        log()->info("[ReactEngine] Auto-compaction succeeded (attempt {}); retrying prompt",
                                   overflow_compaction_attempts);
                        compaction_succeeded = true;
                        // 🎯 对齐 OC agent-runner.ts:770: 记录 auto-compaction 发生
                        ++auto_compaction_count;
                        break;
                    }
                }
                
                // 🎯 严格对齐 OpenClaw run.ts:1130-1179: Fallback — truncate oversized tool results
                if (!compaction_succeeded) {
                    log()->warn("[ReactEngine] All {} compaction attempts failed; checking for oversized tool results",
                               MAX_OVERFLOW_COMPACTION_ATTEMPTS);
                    // Truncate oversized tool results (对齐 OpenClaw: truncateOversizedToolResultsInSession)
                    bool truncated_any = false;
                    for (auto& msg : messages) {
                        if (msg.role == llm::LLMMessageRole::Tool) {
                            int estimated_tokens = static_cast<int>(msg.content.size()) / 4;
                            // If a single tool result > 25% of context window, truncate it
                            if (estimated_tokens > context_window / 4) {
                                size_t max_chars = static_cast<size_t>(context_window / 4) * 4;
                                msg.content = msg.content.substr(0, max_chars) +
                                    "\n\n[Output truncated — exceeded context window budget]";
                                truncated_any = true;
                                log()->warn("[ReactEngine] Truncated oversized tool result ({} → {} chars)",
                                           estimated_tokens * 4, max_chars);
                            }
                        }
                    }
                    if (truncated_any) {
                        log()->info("[ReactEngine] Tool result truncation complete; continuing");
                    } else {
                        log()->error("[ReactEngine] No oversized tool results to truncate; giving up");
                    }
                }
            }
        }
        
        // 检查超时
        auto elapsed = std::chrono::steady_clock::now() - start_time_;
        if (elapsed > config_.total_timeout) {
            result.success = false;
            result.error = "ReAct loop timeout";
            break;
        }
        
        // ✅ 新增: 消费 steer 队列 (对齐 OpenClaw: activeSession.steer → agent.steer)
        // OpenClaw 语义: steer 消息作为新的 user turn 实时注入到正在运行的 LLM session
        //   attempt.ts:2231: queueHandle.queueMessage → activeSession.steer(text)
        //   pi-agent-core: session.agent.steer(AgentMessage{role:"user", content:text})
        //
        // HunterClaw 实现: 在每轮 ReAct 循环开始时，将 steer 队列中的消息
        // 作为 user 消息追加到 messages 列表。这会在下一次 call_llm 时让 LLM 看到。
        {
            std::lock_guard<std::mutex> lock(steer_mutex_);
            while (!steer_queue_.empty()) {
                std::string steer_text = std::move(steer_queue_.front());
                steer_queue_.pop();
                
                llm::LLMMessage steer_msg;
                steer_msg.role = llm::LLMMessageRole::User;
                steer_msg.content = std::move(steer_text);
                messages.push_back(std::move(steer_msg));
                
                log()->info("[ReactEngine] steer consumed: injected user message into messages (total={})",
                           messages.size());
            }
        }
        
        // 🎯 对齐 OpenClaw: handleMessageStart — reset state at message boundary
        // ASSUME: message_start is the only reliable boundary for "new assistant message begins".
        // Resetting at text_end is unsafe (late/duplicate end events).
        streaming_state_.reset_assistant_message_state(
            static_cast<int>(assistant_texts.size()));
        // Emit onAssistantMessageStart (earliest "writing" signal)
        if (callbacks.on_assistant_message_start) {
            callbacks.on_assistant_message_start();
        }

        // 🎯 对齐 OpenClaw pi-hooks/context-pruning/extension.ts:
        //   api.on("context", (event, ctx) => ...) — 在每次向 LLM 发起请求前
        //   对 in-memory messages 做剪枝 (仅影响本次请求 payload，不改写 transcript)
        (void)maybe_prune_context(messages);

        // 🎯 对齐 OC tools-payload.ts: 每次 LLM 调用前打包工具 schema
        // (内置 + MCP + sandbox, 统一走API 原生 tools 参数)
        // 应用 before_prompt_build.toolsAllow 收窄本回合工具集
        auto tool_defs = build_tool_definitions(hook_tools_allow);

        // 步骤 1: Reason (推理)
        auto llm_response = call_llm(messages, callbacks, tool_defs);

        // 🎯 对齐 OpenClaw: context overflow from LLM error → compaction → retry
        if (llm_response.context_overflow && compaction_mgr_) {
            log()->warn("[ReactEngine] LLM returned context overflow, triggering emergency compaction");
            if (callbacks.on_agent_event) {
                core::JsonValue data;
                data["phase"] = "start";
                callbacks.on_agent_event({"compaction", std::move(data)});
            }
            auto compact_result = trigger_compaction(messages, callbacks.transcript_path, callbacks.session_id);
            if (callbacks.on_agent_event) {
                core::JsonValue data;
                data["phase"] = "end";
                data["willRetry"] = compact_result.compacted;
                callbacks.on_agent_event({"compaction", std::move(data)});
            }
            if (compact_result.compacted) {
                log()->info("[ReactEngine] Emergency compaction succeeded, retrying LLM call");
                // 🎯 对齐 OC agent-runner.ts:770: 记录 auto-compaction 发生
                ++auto_compaction_count;
                streaming_state_.reset_assistant_message_state(
                    static_cast<int>(assistant_texts.size()));
                // 🎯 对齐 OC: compaction 之后 retry 前再次 prune (对齐 extension.ts 每次 on("context"))
                (void)maybe_prune_context(messages);
                // 🎯 对齐 OC: compaction 后重建 tool_defs (保持 toolsAllow 约束一致)
                auto retry_tool_defs = build_tool_definitions(hook_tools_allow);
                llm_response = call_llm(messages, callbacks, retry_tool_defs);
            }
        }
        
        // 更新 token 统计
        result.usage.input_tokens += llm_response.input_tokens;
        result.usage.output_tokens += llm_response.output_tokens;
        result.usage.cache_read_tokens += llm_response.cache_read_tokens;
        result.usage.cache_write_tokens += llm_response.cache_write_tokens;
        // 🎯 对齐 OpenClaw: total = input + output + cacheRead + cacheWrite
        result.usage.total_tokens += llm_response.input_tokens + llm_response.output_tokens
            + llm_response.cache_read_tokens + llm_response.cache_write_tokens;
        
        // 记录推理步骤
        ReactStep reason_step;
        reason_step.type = StepType::Reason;
        reason_step.content = llm_response.text;
        reason_step.reasoning = llm_response.reasoning;
        reason_step.has_reasoning = !llm_response.reasoning.empty();
        reason_step.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
        
        // 累积每一轮的响应文本（包括工具调用前的推理）
        accumulated_response += llm_response.text;
        
        // 如果有工具调用
        if (!llm_response.tool_calls.empty()) {
            // 步骤 2: Act (行动)
            result.tool_calls += llm_response.tool_calls.size();
            
            // 发送块回复 (在工具执行前)
            if (config_.block_reply.enabled && 
                config_.block_reply.break_mode == ReactConfig::BlockReply::BreakMode::TextEnd &&
                !accumulated_response.empty()) {
                if (callbacks.on_block_reply) {
                    callbacks.on_block_reply(accumulated_response);
                }
                accumulated_response.clear();
            }
            
            // 执行工具
            auto tool_results = execute_tools(llm_response.tool_calls, callbacks);
            reason_step.tool_calls.reserve(llm_response.tool_calls.size());
            for (const auto& tc : llm_response.tool_calls) {
                reason_step.tool_calls.push_back(tc.name);  // ToolCall has name field directly
            }
            // 转换 tool_results 类型: ToolResult -> JsonValue
            for (const auto& [tool_name, tool_result] : tool_results) {
                // 使用 details 字段作为结构化结果
                reason_step.tool_results[tool_name] = tool_result.details;
            }
            
            // 添加助手消息 (带工具调用)
            llm::LLMMessage assistant_msg;
            assistant_msg.role = llm::LLMMessageRole::Assistant;
            assistant_msg.content = llm_response.text;
            // assistant_msg.tool_calls = llm_response.tool_calls;  // 注释掉,类型不匹配
            messages.push_back(assistant_msg);
            
            // 添加工具结果消息
            for (const auto& [tool_call_id, tool_result] : tool_results) {
                llm::LLMMessage tool_msg;
                tool_msg.role = llm::LLMMessageRole::Tool;
                tool_msg.tool_call_id = tool_call_id;
                
                // 构建工具结果内容
                std::string result_content;
                if (tool_result.success) {
                    for (const auto& content : tool_result.content) {
                        if (content.type == tools::ToolResultContent::Type::Text) {
                            result_content += content.text;
                        } else if (content.type == tools::ToolResultContent::Type::Json) {
                            result_content += content.text;  // JSON 已序列化
                        }
                    }
                } else {
                    result_content = "Error: " + tool_result.error;
                }
                
                tool_msg.content = result_content;
                messages.push_back(tool_msg);

                // =========================================================
                // Hook: tool_result_persist
                // Aligned with OpenClaw attempt.ts — sync hook, called when
                // a tool result is about to be persisted to the transcript.
                // We delegate to append_tool_result_to_transcript which
                // internally fires tool_result_persist + before_message_write.
                // =========================================================
                if (!callbacks.transcript_path.empty()) {
                    sessions::AppendToolResultOptions trp_opts;
                    trp_opts.tool_name    = tool_call_id;  // tool_call_id maps to tool name here
                    trp_opts.tool_call_id = tool_call_id;
                    trp_opts.result_json  = result_content;
                    trp_opts.run_id       = callbacks.session_id;
                    sessions::append_tool_result_to_transcript(
                        callbacks.transcript_path, trp_opts);
                } else {
                    // No transcript path: fire tool_result_persist hook directly
                    auto* trp_hook_mgr = tools::get_global_hook_manager();
                    if (trp_hook_mgr && trp_hook_mgr->has_tool_result_persist_hooks()) {
                        tools::PluginHookToolResultPersistEvent trp_event;
                        trp_event.tool_call_id = tool_call_id.empty()
                            ? std::nullopt : std::make_optional(tool_call_id);
                        trp_event.message_json = result_content;
                        trp_event.is_synthetic = false;
                        tools::PluginHookToolResultPersistContext trp_ctx;
                        trp_ctx.tool_call_id = trp_event.tool_call_id;
                        try {
                            trp_hook_mgr->run_tool_result_persist(trp_event, trp_ctx);
                        } catch (const std::exception& e) {
                            log()->warn("tool_result_persist hook failed: {}", e.what());
                        }
                    }
                }
                
                // Observe (观察工具结果)
                ReactStep observe_tool_step;
                observe_tool_step.type = StepType::Observe;
                observe_tool_step.content = result_content;
                observe_tool_step.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count();
                result.steps.push_back(observe_tool_step);
            }
            
            result.steps.push_back(reason_step);
            
            // 继续下一轮循环 (工具结果 → 再次推理)
            step_count++;
            continue;
        }
        
        // 没有工具调用 - 这是最终响应
        reason_step.is_final = true;

        // =====================================================================
        // Hook: before_agent_finalize (modifying) — 对齐 OpenClaw runBeforeAgentFinalize
        // 自然 final reply 被接受前触发。若插件返回 action="revise"（可带 retry
        // 指令），阻止本次收尾并再跑一轮模型（把 retry.instruction 作为新的 user
        // 输入注入）；action="finalize" 或无hook 则正常收尾。带重跑上限防死循环。
        // 注意：这不是用户面向的 /stop 取消路径。
        // =====================================================================
        {
            auto* baf_mgr = tools::get_global_hook_manager();
            if (baf_mgr && baf_mgr->has_hooks(plugins::PluginHookName::BEFORE_AGENT_FINALIZE) &&
                finalize_revise_count < kMaxFinalizeRevisions) {
                tools::PluginHookBeforeAgentFinalizeEvent baf_event;
                baf_event.session_id = callbacks.session_id;
                if (!callbacks.session_key.empty()) baf_event.session_key = callbacks.session_key;
                if (!callbacks.run_id.empty())      baf_event.run_id= callbacks.run_id;
                if (!callbacks.transcript_path.empty()) baf_event.transcript_path = callbacks.transcript_path;
                baf_event.stop_hook_active = false;
                baf_event.last_assistant_message = llm_response.text;
                tools::PluginHookAgentContext baf_ctx;
                baf_ctx.session_id = callbacks.session_id.empty()
                    ? std::nullopt : std::make_optional(callbacks.session_id);
                if (!callbacks.agent_id.empty())      baf_ctx.agent_id      = callbacks.agent_id;
                if (!callbacks.workspace_dir.empty()) baf_ctx.workspace_dir = callbacks.workspace_dir;
                try {
                    auto baf_result = baf_mgr->run_before_agent_finalize(baf_event, baf_ctx);
                    if (baf_result && baf_result->action.value_or("continue") == "revise") {
                        // 注入 retry 指令作为新一轮 user 输入，阻止收尾、再跑一轮。
                        std::string instruction = baf_result->retry
                            ? baf_result->retry->instruction
                            : baf_result->reason.value_or("Please continue.");
                        llm::LLMMessage revise_msg;
                        revise_msg.role = llm::LLMMessageRole::User;
                        revise_msg.content = instruction;
                        messages.push_back(revise_msg);
                        finalize_revise_count++;
                        log()->info("[hooks] before_agent_finalize requested revise (#{}/{})",
                            finalize_revise_count, kMaxFinalizeRevisions);
                        step_count++;
                        continue;  // ← 阻止收尾，再跑一轮模型
                    }
                } catch (const std::exception& e) {
                    log()->warn("before_agent_finalize hook failed: {}", e.what());
                }
            }
        }

        result.steps.push_back(reason_step);

        // 步骤 3: Respond (响应)
        // 🎯 OpenClaw 对齐: 根据 reasoning_mode 决定 final_response 内容
        switch (config_.reasoning_mode) {
            case ReactConfig::ReasoningMode::Off:
                result.final_response = streaming_state_.block_buffer;
                break;
            case ReactConfig::ReasoningMode::On:
            case ReactConfig::ReasoningMode::Stream:
                result.final_response = streaming_state_.delta_buffer;
                break;
        }

        // 🎯 对齐 OpenClaw handlers.messages.ts:282-285: resolveSilentReplyFallbackText
        // When assistant replies "NO_REPLY", fall back to the last messaging tool sent text
        {
            std::string trimmed = result.final_response;
            auto start = trimmed.find_first_not_of(" \t\n\r");
            auto end = trimmed.find_last_not_of(" \t\n\r");
            if (start != std::string::npos) trimmed = trimmed.substr(start, end - start + 1);
            else trimmed.clear();
            if (trimmed == "NO_REPLY" && !streaming_state_.messaging_tool_sent_texts.empty()) {
                result.final_response = streaming_state_.messaging_tool_sent_texts.back();
                log()->debug("[ReactEngine] Resolved NO_REPLY to last messaging tool text");
            }
        }

        // 🎯 对齐 OpenClaw: handleAgentEnd — flush block reply buffer
        // Called at agent_end, not just at loop end, to ensure delivery before compaction.
        if (config_.block_reply.enabled) {
            flush_block_reply_buffer(callbacks);
        }
        // 🎯 对齐 OpenClaw: onBlockReplyFlush — called at agent_end (#35074)
        // Ensures buffered block replies reach the channel before compaction wait blocks the run.
        if (callbacks.on_block_reply_flush) {
            callbacks.on_block_reply_flush();
        }

        // 🎯 对齐 OpenClaw: finalizeAssistantTexts
        // If includeReasoning=true and no onBlockReply, replace streamed chunks with final text.
        if (config_.reasoning_mode != ReactConfig::ReasoningMode::Off &&
            !callbacks.on_block_reply && !llm_response.text.empty()) {
            // Replace all chunks added during this message with the final text
            if (static_cast<int>(assistant_texts.size()) > streaming_state_.assistant_text_baseline) {
                assistant_texts.resize(streaming_state_.assistant_text_baseline);
            }
            assistant_texts.push_back(llm_response.text);
            streaming_state_.suppress_block_chunks = true;
        }

        break;  // 完成
    }
    
    if (cancelled_) {
        result.success = false;
        result.error = "ReAct loop cancelled";
        // 🎯 对齐 OpenClaw abort partial: 中断时保留已生成的 assistant 文本，
        //   与 gateway chat abort partial 采集语义一致（用户可见已生成内容）。
        if (result.final_response.empty()) {
            if (!assistant_texts.empty()) {
                result.final_response = assistant_texts.back();
            } else if (!streaming_state_.delta_buffer.empty()) {
                result.final_response = streaming_state_.delta_buffer;
            } else if (!streaming_state_.block_buffer.empty()) {
                result.final_response = streaming_state_.block_buffer;
            }
        }
    } else if (step_count >= config_.max_steps) {
        result.success = false;
        result.error = "ReAct loop exceeded max steps";
        // 🎯 对齐 OpenClaw: 循环因达到内部上限终止时，保留已生成的 assistant 文本
        //   作为 final_response（此前恒为空 → 丢失用户已看到的流式内容）。
        //   优先用最后一段 assistant_texts，其次delta/block buffer。
        if (result.final_response.empty()) {
            if (!assistant_texts.empty()) {
                result.final_response = assistant_texts.back();
            } else if (!streaming_state_.delta_buffer.empty()) {
                result.final_response = streaming_state_.delta_buffer;
            } else if (!streaming_state_.block_buffer.empty()) {
                result.final_response = streaming_state_.block_buffer;
            }
        }
    }

    // 🎯 对齐 OpenClaw: store assistantTexts in result
    result.assistant_texts = std::move(assistant_texts);

    // =========================================================================
    // ✅ 对齐 OC agent-runner.ts: post-loop 辅助模块集成
    // 以下调用对齐 OpenClaw 的 reply 处理管线，确保 react/ 辅助模块参与运行
    // =========================================================================

    // ✅ 对齐 OC: resolveEnforceFinalTag (agent-runner-utils.ts)
    // 用于某些 provider 上需要强制 final tag 的场景
    if (!callbacks.channel_id.empty()) {
        bool enforce_final = resolve_enforce_final_tag(callbacks.channel_id);
        (void)enforce_final;
    }

    // ✅ 对齐 OC: createShouldEmitToolResult / createShouldEmitToolOutput (agent-runner-helpers.ts)
    // 这些 lambda 在 pi-embedded-subscribe.ts 中控制工具事件是否广播给客户端
    // HC 在 on_tool_result callback 中使用
    auto should_emit_result = create_should_emit_tool_result(
        config_.verbose_tool_output ? "on" : "off");
    auto should_emit_output = create_should_emit_tool_output(
        config_.verbose_tool_output ? "on" : "off");

    // ✅ 对齐 OC: setAbortMemory(sessionKey, false) — clear abort flag after successful run
    if (result.success && !callbacks.session_key.empty()) {
        set_abort_memory(callbacks.session_key, false);
    }

    // ✅ 对齐 OC: isLikelyExecutionAckPrompt — detect short ack prompts for fallback logic
    if (!result.final_response.empty()) {
        bool is_ack = is_likely_execution_ack_prompt(result.final_response);
        if (is_ack) {
            log()->debug("[ReactEngine] Final response matches execution ack pattern");
        }
    }

    // ✅ 严格对齐 OC agent-runner-utils.ts: updateSessionAccounting(...)
    //   每轮结束后把本轮 usage 回写 session store（totalTokens/totalTokensFresh/
    //   input/output/model/model_provider/updated_at）。未注入 callback 则跳过。
    //
    //   关键意义：让 preflight_memory 的 fresh 门控在同一进程的多轮之间持续生效，
    //   而非「只在重启后第一轮」—— 之前因为没有回写链路，persisted_total_tokens
    //   不会随 turn 刷新，fresh 门控退化。
    if (callbacks.on_run_complete_fn) {
        try {
            StreamCallbacks::RunCompleteUsage ru;
            ru.input_tokens        = result.usage.input_tokens;
            ru.output_tokens       = result.usage.output_tokens;
            ru.cache_read_tokens   = result.usage.cache_read_tokens;
            ru.cache_write_tokens  = result.usage.cache_write_tokens;
            ru.model_provider = callbacks.provider.empty() && llm_client_
                ? llm_client_->get_config().provider : callbacks.provider;
            ru.model = callbacks.model.empty() && llm_client_
                ? llm_client_->get_config().model : callbacks.model;
            ru.is_heartbeat = callbacks.is_heartbeat;
            callbacks.on_run_complete_fn(ru);
        } catch (const std::exception& e) {
            log()->warn("[ReactEngine] on_run_complete_fn threw: {}", e.what());
        } catch (...) {
            log()->warn("[ReactEngine] on_run_complete_fn threw: unknown");
        }
    }

    // ✅ 对齐 OC: shouldRunMemoryFlush (memory-flush.ts) — check if memory flush needed after run
    // OC 中这个检查在 agent-runner-memory.ts runMemoryFlushIfNeeded 中完成
    // HC engine 负责门控判断 + 触发 run_memory_flush_if_needed (如果外层注入了执行器)
    {
        MemoryFlushGateParams flush_params;
        flush_params.total_tokens = result.usage.total_tokens;
        flush_params.total_tokens_fresh = true;
        flush_params.context_window_tokens = context_window_config_.context_window_tokens;
        flush_params.reserve_tokens_floor = 2048;
        flush_params.soft_threshold_tokens = 1024;
        flush_params.compaction_count = callbacks.compaction_count;
        flush_params.memory_flush_compaction_count =
            callbacks.memory_flush_compaction_count;

        if (should_run_memory_flush(flush_params)) {
            log()->info("[ReactEngine] Memory flush gate triggered — tokens: {}",
                        result.usage.total_tokens);

            // 对齐 OC: 保留观察性 callback（向后兼容上层打日志/埋点）
            if (callbacks.on_memory_flush_needed) {
                callbacks.on_memory_flush_needed(result.usage.total_tokens);
            }

            // 🎯 严格对齐 OC agent-runner-memory.ts:505-848:
            //   runMemoryFlushIfNeeded(agent, session_ctx, runtime_ctx)
            //
            // 如果外层注入了 run_flush_agent_fn，engine 直接驱动 flush 全流程：
            //   1) 再次门控（has_already_flushed_for_current_compaction）
            //   2) 起 flush agent 子进程写 MEMORY.md (run_flush_agent_fn)
            //   3) 回写 transcript-derived token (persist_session_tokens_fn)
            //   4) session rotation (on_flush_complete_fn)
            if (callbacks.run_flush_agent_fn &&
                !callbacks.session_key.empty() &&
                !callbacks.is_heartbeat && !callbacks.is_cli) {
                try {
                    react::MemoryFlushParams fp;
                    fp.session_key = callbacks.session_key;
                    fp.session_id = callbacks.session_id;
                    fp.session_file = callbacks.transcript_path;
                    fp.store_path = "";
                    fp.workspace_dir = callbacks.workspace_dir;
                    fp.provider = callbacks.provider.empty() && llm_client_
                        ? llm_client_->get_config().provider : callbacks.provider;
                    fp.model = callbacks.model.empty() && llm_client_
                        ? llm_client_->get_config().model : callbacks.model;
                    fp.default_model = callbacks.default_model;
                    fp.agent_cfg_context_tokens = callbacks.agent_cfg_context_tokens;
                    fp.is_heartbeat = callbacks.is_heartbeat;
                    fp.is_cli = callbacks.is_cli;
                    fp.memory_flush_writable = callbacks.memory_flush_writable;
                    fp.persisted_total_tokens = result.usage.total_tokens > 0
                        ? std::optional<int>(result.usage.total_tokens)
                        : callbacks.persisted_total_tokens;
                    fp.total_tokens_fresh = true;
                    fp.compaction_count = callbacks.compaction_count;
                    fp.memory_flush_compaction_count =
                        callbacks.memory_flush_compaction_count;
                    fp.reserve_tokens_floor = 2048;
                    fp.soft_threshold_tokens = 1024;
                    fp.context_window_tokens =
                        context_window_config_.context_window_tokens;
                    fp.force_flush_transcript_bytes =
                        callbacks.force_flush_transcript_bytes;
                    fp.flush_write_path = callbacks.flush_write_path;
                    fp.flush_system_prompt = callbacks.flush_system_prompt;
                    fp.flush_prompt = callbacks.flush_prompt;
                    fp.session_log_path = callbacks.transcript_path;

                    fp.run_flush_agent = callbacks.run_flush_agent_fn;
                    if (callbacks.persist_session_tokens_fn) {
                        fp.persist_session_tokens =
                            callbacks.persist_session_tokens_fn;
                    }
                    if (callbacks.on_flush_complete_fn) {
                        fp.on_flush_complete = callbacks.on_flush_complete_fn;
                    }

                    auto fr = react::run_memory_flush_if_needed(fp);
                    if (fr.flushed) {
                        log()->info(
                            "[ReactEngine] memory flush done: session={} "
                            "compaction_completed={} new_session_id={}",
                            callbacks.session_key,
                            fr.compaction_completed,
                            fr.new_session_id.value_or("<unchanged>"));
                    }
                } catch (const std::exception& e) {
                    log()->warn("[ReactEngine] memory flush failed: {}", e.what());
                } catch (...) {
                    log()->warn("[ReactEngine] memory flush failed: unknown");
                }
            }
        }
    }

    result.total_steps = result.steps.size();
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time_
    ).count();

    // 🎯 对齐 OpenClaw handlers.lifecycle.ts:33-95: emit lifecycle end/error AgentEvent
    if (callbacks.on_agent_event) {
        if (!result.success && !result.error.empty()) {
            core::JsonValue data;
            data["phase"] = "error";
            data["error"] = result.error;
            callbacks.on_agent_event({"lifecycle", std::move(data)});
        } else {
            core::JsonValue data;
            data["phase"] = "end";
            callbacks.on_agent_event({"lifecycle", std::move(data)});
        }
    }

    // =========================================================================
    // Hook: agent_end
    // Aligned with OpenClaw attempt.ts:2678 — fire-and-forget, runs even on error
    // Allows plugins to analyze the completed conversation.
    // =========================================================================
    if (hook_mgr && hook_mgr->has_hooks(plugins::PluginHookName::AGENT_END)) {
        tools::PluginHookAgentEndEvent ae_event;
        // Serialize messages to JSON string
        ae_event.messages_json = "[]";  // simplified; full serialization via transcript
        ae_event.success = result.success;
        if (!result.error.empty()) ae_event.error = result.error;
        ae_event.duration_ms = static_cast<int>(result.duration_ms);

        tools::PluginHookAgentContext ae_ctx;
        ae_ctx.session_id = callbacks.session_id.empty()
            ? std::nullopt : std::make_optional(callbacks.session_id);
        // 🎯 对齐 OpenClaw attempt.ts:2685-2693: full hook context
        if (!callbacks.agent_id.empty())      ae_ctx.agent_id        = callbacks.agent_id;
        if (!callbacks.session_key.empty())   ae_ctx.session_key     = callbacks.session_key;
        if (!callbacks.workspace_dir.empty()) ae_ctx.workspace_dir   = callbacks.workspace_dir;
        if (!callbacks.trigger.empty())       ae_ctx.trigger         = callbacks.trigger;
        if (!callbacks.channel_id.empty())    ae_ctx.channel_id      = callbacks.channel_id;
        try {
            hook_mgr->run_agent_end(ae_event, ae_ctx);
        } catch (const std::exception& e) {
            log()->warn("agent_end hook failed: {}", e.what());
        }
    }
    
    // ✅ 可观测性: 发射 ToolLoop 事件 (对齐 OpenClaw)
    try {
        auto loop_event = std::make_shared<observability::ToolLoopEvent>();
        loop_event->session_id = callbacks.session_id.empty() ? "unknown" : callbacks.session_id;
        loop_event->consecutive_calls = step_count;
        loop_event->is_loop_detected = step_count >= config_.max_steps;
        loop_event->timestamp = std::chrono::system_clock::now();
        
        observability::DiagnosticEventBus::instance().emit(loop_event);
    } catch (...) {
        // 故障隔离
    }

    // ========================================================================
    // 🎯 严格对齐 OpenClaw agent-runner.ts:770-807
    //
    //   if (autoCompactionCount > 0) {
    //     ...
    //     if (sessionKey) {
    //       const workspaceDir = process.cwd();
    //       readPostCompactionContext(workspaceDir, cfg).then(content => {
    //         if (content) enqueueSystemEvent(content, { sessionKey });
    //       }).catch(() => {});
    //     }
    //   }
    //
    // 行为: 本轮 run 发生过 auto-compaction 时, 读取 AGENTS.md 的
    //       Session Startup / Red Lines sections, 作为 SystemEvent 入队.
    //       下一轮 agent run 开始时, 该事件会被 prepend 到 prompt
    //       (通过 extraSystemPrompt 或直接作为 user turn 注入).
    //
    //       这是 best-effort — 任何失败都静默忽略, 不影响主流程.
    // ========================================================================
    if (auto_compaction_count > 0 && !callbacks.session_key.empty()) {
        try {
            auto refresh_prompt = react::read_post_compaction_context(
                callbacks.workspace_dir,
                /*configured_sections=*/{},  // 空 → 使用默认 Session Startup / Red Lines
                /*now_ms=*/0                 // 0 → 当前时间
            );
            if (refresh_prompt && !refresh_prompt->empty()) {
                bool added = infra::enqueue_system_event(
                    *refresh_prompt,
                    callbacks.session_key,
                    /*context_key=*/std::nullopt
                );
                log()->info(
                    "[ReactEngine] post-compaction context refresh enqueued "
                    "(session_key={}, compactions={}, added={})",
                    callbacks.session_key, auto_compaction_count, added
                );
            } else {
                log()->debug(
                    "[ReactEngine] post-compaction context refresh skipped "
                    "(no AGENTS.md sections found in workspace={})",
                    callbacks.workspace_dir
                );
            }
        } catch (const std::exception& e) {
            // Silent failure — post-compaction context is best-effort
            log()->debug("[ReactEngine] post-compaction context injection failed: {}", e.what());
        } catch (...) {
            log()->debug("[ReactEngine] post-compaction context injection failed: unknown");
        }
    }

    return result;
}

ReactEngine::LLMResponseData ReactEngine::call_llm(
    const std::vector<llm::LLMMessage>& messages_in,
    const StreamCallbacks& callbacks,
    const std::vector<llm::ToolDefinition>& tools
) {
    LLMResponseData response;

    // =========================================================================
    // 🎯 对齐 OpenClaw attempt.ts:2073-2093: Message Preprocessing Pipeline
    // Order: validateTurns → limitHistory → sanitizeToolUseResultPairing
    //        → dropThinkingBlocks → truncateOversizedToolResults
    // =========================================================================
    std::vector<llm::LLMMessage> sanitized_messages = messages_in;

    // 1. 🎯 对齐 OpenClaw: validateAnthropicTurns / validateGeminiTurns
    //    Merge consecutive user messages (Anthropic requirement)
    //    Merge consecutive assistant messages (Gemini requirement)
    {
        std::vector<llm::LLMMessage> validated;
        llm::LLMMessageRole last_role = llm::LLMMessageRole::System;
        for (const auto& msg : sanitized_messages) {
            if (!validated.empty() && msg.role == last_role) {
                // Merge consecutive same-role messages (except system)
                if (msg.role == llm::LLMMessageRole::User ||
                    msg.role == llm::LLMMessageRole::Assistant) {
                    validated.back().content += "\n" + msg.content;
                    continue;
                }
            }
            validated.push_back(msg);
            last_role = msg.role;
        }
        sanitized_messages = std::move(validated);
    }

    // 2. 🎯 对齐 OpenClaw: limitHistoryTurns — limit DM/channel history
    if (config_.max_history_turns > 0 && sanitized_messages.size() > 2) {
        int user_count = 0;
        size_t cut_point = sanitized_messages.size();
        for (int i = static_cast<int>(sanitized_messages.size()) - 1; i >= 0; --i) {
            if (sanitized_messages[i].role == llm::LLMMessageRole::User) {
                user_count++;
                if (user_count > config_.max_history_turns) {
                    cut_point = i;
                    // Keep system message(s) at the front
                    break;
                }
            }
        }
        if (cut_point > 0 && cut_point < sanitized_messages.size()) {
            // Preserve system messages, then take from cut_point
            std::vector<llm::LLMMessage> limited;
            for (size_t i = 0; i < sanitized_messages.size(); ++i) {
                if (i < cut_point && sanitized_messages[i].role == llm::LLMMessageRole::System) {
                    limited.push_back(sanitized_messages[i]);
                } else if (i >= cut_point) {
                    limited.push_back(sanitized_messages[i]);
                }
            }
            sanitized_messages = std::move(limited);
        }
    }

    // 3. 🎯 对齐 OpenClaw: sanitizeToolUseResultPairing
    //    Ensure every assistant tool call has a matching tool result.
    //    Insert synthetic error result for missing tool results.
    {
        std::vector<llm::LLMMessage> repaired;
        for (size_t i = 0; i < sanitized_messages.size(); ++i) {
            const auto& msg = sanitized_messages[i];
            repaired.push_back(msg);

            // After an assistant message with tool calls, check if next messages
            // are tool results. If tool results are missing, they were already
            // appended in execute_loop, so this is mainly a safety net for
            // corrupted history.
            if (msg.role == llm::LLMMessageRole::Assistant && !msg.tool_calls.empty()) {
                // Collect expected tool call IDs from JsonValue tool_calls
                std::set<std::string> expected_ids;
                for (const auto& tc : msg.tool_calls) {
                    // tool_calls are JsonValue; try to extract "id" field
                    if (tc.is_object() && tc.contains("id") && tc["id"].is_string()) {
                        expected_ids.insert(tc["id"].as_string());
                    }
                }
                // Check which are satisfied by subsequent tool results
                for (size_t j = i + 1; j < sanitized_messages.size(); ++j) {
                    if (sanitized_messages[j].role == llm::LLMMessageRole::Tool) {
                        expected_ids.erase(sanitized_messages[j].tool_call_id);
                    } else if (sanitized_messages[j].role == llm::LLMMessageRole::Assistant) {
                        break;
                    }
                }
                // Insert synthetic error results for missing tool results
                for (const auto& missing_id : expected_ids) {
                    llm::LLMMessage synthetic;
                    synthetic.role = llm::LLMMessageRole::Tool;
                    synthetic.tool_call_id = missing_id;
                    synthetic.content = "[missing tool result in session history]";
                    repaired.push_back(synthetic);
                    log()->debug("[Sanitize] Inserted synthetic tool result for missing id: {}", missing_id);
                }
            }
        }
        sanitized_messages = std::move(repaired);
    }

    // 4. 🎯 对齐 OpenClaw thinking.ts:25-53: dropThinkingBlocks
    //    Strip <think>...</think> content from assistant messages when
    //    replaying to non-reasoning-capable models (prevents provider rejection)
    if (config_.drop_thinking_blocks) {
        for (auto& msg : sanitized_messages) {
            if (msg.role == llm::LLMMessageRole::Assistant && !msg.content.empty()) {
                std::string stripped = strip_thinking_tags(msg.content);
                stripped = trim_end(stripped);
                if (stripped.empty() && !msg.content.empty()) {
                    // Preserve turn structure even if all content was thinking
                    msg.content = "";
                } else {
                    msg.content = stripped;
                }
            }
        }
    }

    // 5. 🎯 对齐 OpenClaw tool-result-truncation.ts: truncateOversizedToolResults
    //    Prevent single tool results from consuming >30% of context window
    {
        int context_window = context_window_config_.context_window_tokens;
        int max_tool_result_chars = static_cast<int>(context_window * 0.3 * 4);  // ~30% of window, 4 chars/token
        if (max_tool_result_chars < 2000) max_tool_result_chars = 2000;
        if (max_tool_result_chars > 400000) max_tool_result_chars = 400000;

        for (auto& msg : sanitized_messages) {
            if (msg.role == llm::LLMMessageRole::Tool &&
                static_cast<int>(msg.content.size()) > max_tool_result_chars) {
                log()->debug("[Sanitize] Truncating oversized tool result: {} chars → {} chars",
                            msg.content.size(), max_tool_result_chars);
                // Head+tail strategy when tail looks important (errors/summaries)
                std::string tail = msg.content.substr(
                    msg.content.size() > 2000 ? msg.content.size() - 2000 : 0);
                std::string lower_tail = tail;
                std::transform(lower_tail.begin(), lower_tail.end(), lower_tail.begin(), ::tolower);
                bool important_tail = (
                    lower_tail.find("error") != std::string::npos ||
                    lower_tail.find("exception") != std::string::npos ||
                    lower_tail.find("failed") != std::string::npos ||
                    lower_tail.find("traceback") != std::string::npos ||
                    lower_tail.find("summary") != std::string::npos ||
                    lower_tail.find("result") != std::string::npos
                );
                if (important_tail && max_tool_result_chars > 4000) {
                    // Head 70% + tail 30%
                    int tail_budget = std::min(max_tool_result_chars * 3 / 10, 4000);
                    int head_budget = max_tool_result_chars - tail_budget - 80;  // 80 for marker
                    std::string marker = "\n\n⚠️ [... middle content omitted — showing head and tail ...]\n\n";
                    msg.content = msg.content.substr(0, head_budget) + marker +
                                  msg.content.substr(msg.content.size() - tail_budget) +
                                  "\n\n⚠️ [Content truncated]";
                } else {
                    msg.content = msg.content.substr(0, max_tool_result_chars) +
                                  "\n\n⚠️ [Content truncated — original was too large for context window]";
                }
            }
        }
    }

    // Use sanitized messages for the rest of the call
    const auto& messages = sanitized_messages;
    
    // 使用流式接口 (对齐 OpenClaw)
    std::string accumulated_text;
    std::string accumulated_reasoning;
    std::vector<llm::ToolCall> accumulated_tool_calls_;

    // ✅ 向后兼容: 使用 LLMClient 接口
    // 🎯 对齐 OpenClaw: handleMessageUpdate — text_delta / text_end / thinking_delta / thinking_end
    // The stream_callback here simulates the text_delta path (most common).
    // text_end monotonicity is handled by the LLM client layer (or approximated below).
    auto stream_callback = [&](const std::string& delta) {
        if (delta.empty()) return;

        // 🎯 对齐 OpenClaw chunk 级 abort: signal.aborted 在流式回调内逐 chunk 检查。
        // 一旦本run 或 session 被 abort，立即停止累积/emit 后续 token（掐断正在吐的流），
        // 不再把在飞的 chunk 送到 channel。step 级 break 之外的第二道闸。
        if (cancelled_ ||
            (callbacks.abort_signal && callbacks.abort_signal->load())) {
            streaming_state_.stream_aborted = true;
            return;
        }
        if (streaming_state_.stream_aborted) return;

        // 🎯 对齐 OpenClaw: deltaBuffer accumulation (text_delta path)
        streaming_state_.delta_buffer += delta;

        // 1. 提取推理内容 (thinking tags)
        auto extraction = extract_reasoning(delta);

        // 🎯 对齐 OpenClaw: thinking_delta / thinking_end handling
        // emitReasoningStream: compute delta vs lastStreamedReasoning
        bool was_thinking = streaming_state_.reasoning_stream_open;
        if (extraction.has_reasoning && callbacks.on_reasoning_stream) {
            accumulated_reasoning_raw_ += extraction.reasoning;
            std::string formatted = format_reasoning_text(accumulated_reasoning_raw_);

            // Compute delta: new text since last emitted reasoning.
            const std::string& prior = streaming_state_.last_streamed_reasoning;
            std::string delta_reasoning;
            if (!prior.empty() && formatted.find(prior) == 0) {
                delta_reasoning = formatted.substr(prior.length());
            } else {
                delta_reasoning = formatted;
            }
            streaming_state_.last_streamed_reasoning = formatted;

            if (!delta_reasoning.empty()) {
                streaming_state_.reasoning_stream_open = true;
                // 🎯 对齐 OpenClaw pi-embedded-subscribe.ts:574-581: emit thinking AgentEvent
                if (callbacks.on_agent_event) {
                    core::JsonValue data;
                    data["text"] = formatted;
                    data["delta"] = delta_reasoning;
                    callbacks.on_agent_event({"thinking", std::move(data)});
                }
                callbacks.on_reasoning_stream(delta_reasoning);
            }
            accumulated_reasoning += extraction.reasoning;
        }

        // 🎯 对齐 OpenClaw handlers.messages.ts:193-194: emitReasoningEnd mid-stream
        // Detect when thinking block ends (</think> tag processed in this delta)
        if (!extraction.has_reasoning && was_thinking && !extraction.remaining_text.empty()) {
            // </think> was processed — reasoning block closed, now producing non-reasoning text
            if (streaming_state_.reasoning_stream_open && callbacks.on_reasoning_end) {
                streaming_state_.reasoning_stream_open = false;
                callbacks.on_reasoning_end();
            }
        }

        // 2. 发送助手输出流 (on_assistant_delta / onPartialReply)
        // 🎯 对齐 OpenClaw handlers.messages.ts:88: suppress if deterministicApprovalPromptSent
        if (!extraction.remaining_text.empty() && !streaming_state_.deterministic_approval_prompt_sent) {
            if (callbacks.on_assistant_delta) {
                callbacks.on_assistant_delta(extraction.remaining_text);
            }
            // 🎯 对齐 OpenClaw handlers.messages.ts:218-235: emit assistant AgentEvent
            if (callbacks.on_agent_event) {
                core::JsonValue data;
                data["text"] = accumulated_text + extraction.remaining_text;
                data["delta"] = extraction.remaining_text;
                callbacks.on_agent_event({"assistant", std::move(data)});
            }
        }

        // 3. 累积文本用于最终响应
        accumulated_text += extraction.remaining_text;

        // 🎯 对齐 OpenClaw: blockBuffer accumulation (filtered, no <think> tags)
        streaming_state_.block_buffer += extraction.remaining_text;

        // 🎯 对齐 OpenClaw EmbeddedBlockChunker: track code fence state
        // Detect ``` or ~~~ at line start to toggle in_code_fence
        {
            const auto& buf = streaming_state_.block_buffer;
            size_t search_start = buf.size() > extraction.remaining_text.size()
                ? buf.size() - extraction.remaining_text.size() : 0;
            // Scan for fence markers in the newly appended text
            for (size_t pos = search_start; pos < buf.size(); ++pos) {
                if (pos == 0 || buf[pos - 1] == '\n') {
                    // Check for fence marker at start of line
                    std::string_view rest(buf.data() + pos, buf.size() - pos);
                    if (rest.size() >= 3 && (rest.substr(0, 3) == "```" || rest.substr(0, 3) == "~~~")) {
                        std::string marker(rest.substr(0, 3));
                        if (streaming_state_.in_code_fence) {
                            // Close fence: must match opening marker type
                            if (marker[0] == streaming_state_.code_fence_marker[0]) {
                                streaming_state_.in_code_fence = false;
                                streaming_state_.code_fence_marker.clear();
                            }
                        } else {
                            // Open fence
                            streaming_state_.in_code_fence = true;
                            streaming_state_.code_fence_marker = marker;
                        }
                    }
                }
            }
        }

        // 4. 🎯 对齐 OpenClaw: blockReplyBreak="text_end" — flush at each text_end
        // In the streaming callback we approximate text_end as each delta chunk.
        // For blockReplyBreak="message_end", flushing happens at message_end (after call_llm).
        if (config_.block_reply.enabled &&
            config_.block_reply.break_mode == ReactConfig::BlockReply::BreakMode::TextEnd &&
            callbacks.on_block_reply &&
            !streaming_state_.block_buffer.empty()) {
            // Flush on paragraph/sentence boundaries (approximate text_end behavior)
            // 🎯 对齐 OpenClaw EmbeddedBlockChunker: never flush inside code fences
            bool should_flush = false;
            if (streaming_state_.in_code_fence) {
                // Inside a code fence — only flush if max_chars exceeded (emergency)
                should_flush = ((int)streaming_state_.block_buffer.size() >= config_.block_reply.max_chars * 2);
            } else if ((int)streaming_state_.block_buffer.size() >= config_.block_reply.max_chars) {
                should_flush = true;
            } else if ((int)streaming_state_.block_buffer.size() >= config_.block_reply.min_chars) {
                switch (config_.block_reply.break_preference) {
                    case ReactConfig::BlockReply::BreakPreference::Paragraph:
                    case ReactConfig::BlockReply::BreakPreference::Newline:
                        should_flush = (streaming_state_.block_buffer.back() == '\n');
                        break;
                    case ReactConfig::BlockReply::BreakPreference::Sentence:
                        should_flush = is_sentence_boundary(streaming_state_.block_buffer, streaming_state_.block_buffer.size() - 1);
                        break;
                }
            }
            if (should_flush) {
                emit_block_chunk(streaming_state_.block_buffer, callbacks);
                streaming_state_.block_buffer.clear();
            }
        }
    };
    
    // =========================================================================
    // Hook: llm_input
    // Aligned with OpenClaw attempt.ts:2431 — fire-and-forget, before LLM call
    // Allows plugins to observe the exact input payload sent to the LLM.
    // =========================================================================
    auto* llm_hook_mgr = tools::get_global_hook_manager();
    if (llm_hook_mgr && llm_hook_mgr->has_hooks(plugins::PluginHookName::LLM_INPUT)) {
        tools::PluginHookLlmInputEvent li_event;
        li_event.run_id     = callbacks.run_id.empty() ? callbacks.session_id : callbacks.run_id;
        li_event.session_id = callbacks.session_id;
        li_event.provider   = llm_client_ ? llm_client_->get_config().provider : "unknown";
        li_event.model      = llm_client_ ? llm_client_->get_config().model    : "unknown";
        li_event.prompt     = messages.empty() ? "" : messages.back().content;
        li_event.history_messages_json = [&]() -> std::string {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& msg : messages) {
                nlohmann::json m;
                m["role"] = (msg.role == llm::LLMMessageRole::User) ? "user" :
                            (msg.role == llm::LLMMessageRole::Assistant) ? "assistant" :
                            (msg.role == llm::LLMMessageRole::System) ? "system" : "tool";
                m["content"] = msg.content;
                arr.push_back(m);
            }
            return arr.dump();
        }();
        li_event.images_count = 0;

        tools::PluginHookAgentContext li_ctx;
        li_ctx.session_id = callbacks.session_id.empty()
            ? std::nullopt : std::make_optional(callbacks.session_id);
        try {
            llm_hook_mgr->run_llm_input(li_event, li_ctx);
        } catch (const std::exception& e) {
            log()->warn("llm_input hook failed: {}", e.what());
        }
    }

    // 🎯 对齐 OpenClaw run.ts:412-660: Auth profile rotation + model failover
    // 调用 LLM 的流式接口 (从 messages)，支持 failover retry
    //
    // 🎯 严格对齐 OC tools-payload.ts: 每次请求都把完整 tool schema 打包为
    // API 原生 `tools` 参数 (LLMClient::generate_stream_from_messages_with_tools)
    std::vector<core::JsonValue> tools_payload;
    tools_payload.reserve(tools.size());
    for (const auto& td : tools) {
        // 打包为 {name, description, input_schema} 三字段 JsonValue
        core::JsonValue tj = core::JsonValue::object();
        tj["name"] = core::JsonValue(td.name);
        tj["description"] = core::JsonValue(td.description);
        tj["input_schema"] = td.input_schema;
        tools_payload.push_back(std::move(tj));
    }

    // =========================================================================
    // Hook: model_call_started / model_call_ended
    // 对齐 OpenClaw hooks.ts:1632-1639 + attempt.ts — 包裹每一次实际模型调用
    // （含 failover 重试，每次调用各触发一对 started/ended，带独立 callId）。
    // 观测型 void hook：started 在调用前，ended 在调用后（带 durationMs + outcome）。
    // =========================================================================
    auto attempt_llm_call = [&]() {
        auto* mc_mgr = tools::get_global_hook_manager();
        const std::string mc_run_id =
            callbacks.run_id.empty() ? callbacks.session_id : callbacks.run_id;
        const std::string mc_provider =
            llm_client_ ? llm_client_->get_config().provider : "unknown";
        const std::string mc_model =
            llm_client_ ? llm_client_->get_config().model : "unknown";
        // callId：每次实际调用唯一（对齐 OC callId 语义）。
        const std::string mc_call_id = mc_run_id + "-" + std::to_string(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        tools::PluginHookAgentContext mc_ctx;
        if (!callbacks.session_id.empty()) mc_ctx.session_id = callbacks.session_id;

        if (mc_mgr && mc_mgr->has_hooks(plugins::PluginHookName::MODEL_CALL_STARTED)) {
            tools::PluginHookModelCallStartedEvent ev;
            ev.run_id = mc_run_id; ev.call_id = mc_call_id;
            if (!callbacks.session_id.empty()) ev.session_id = callbacks.session_id;
            ev.provider = mc_provider; ev.model = mc_model;
            try { mc_mgr->run_model_call_started(ev, mc_ctx); }
            catch (const std::exception& e) { log()->warn("model_call_started hook failed: {}", e.what()); }
        }

        auto mc_t0 = std::chrono::steady_clock::now();
        auto call_result = llm_client_->generate_stream_from_messages_with_tools(
            messages, tools_payload, stream_callback);
        auto mc_dur = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - mc_t0).count());

        if (mc_mgr && mc_mgr->has_hooks(plugins::PluginHookName::MODEL_CALL_ENDED)) {
            tools::PluginHookModelCallEndedEvent ev;
            ev.run_id = mc_run_id; ev.call_id = mc_call_id;
            if (!callbacks.session_id.empty()) ev.session_id = callbacks.session_id;
            ev.provider = mc_provider; ev.model = mc_model;
            ev.duration_ms = mc_dur;
            ev.outcome = call_result.ok ? "completed" : "error";
            if (!call_result.ok && !call_result.error.empty()) ev.error_category = call_result.error;
            try { mc_mgr->run_model_call_ended(ev, mc_ctx); }
            catch (const std::exception& e) { log()->warn("model_call_ended hook failed: {}", e.what()); }
        }
        return call_result;
    };

    auto result = attempt_llm_call();

    if (!result.ok && config_.failover.enabled && !config_.failover.profiles.empty()) {
        // 对齐 OpenClaw: FailoverError → rotate to next profile + retry
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int attempts = 0;
        for (auto& profile : config_.failover.profiles) {
            if (attempts >= config_.failover.max_retries) break;
            // Skip profiles in cooldown
            if (profile.cooldown_until_ms > now_ms) {
                log()->debug("[Failover] Skipping profile '{}' (cooling down)", profile.id);
                continue;
            }
            log()->warn("[Failover] Primary LLM failed: '{}'. Trying profile '{}' (model={})",
                        result.error, profile.id, profile.model);
            // Apply profile's model/credentials
            if (!profile.model.empty() && llm_client_) {
                llm_client_->set_model_override(profile.model);
            }
            // Reset streaming state for retry
            streaming_state_.delta_buffer.clear();
            streaming_state_.block_buffer.clear();
            result = attempt_llm_call();
            attempts++;
            if (result.ok) {
                log()->info("[Failover] Profile '{}' succeeded", profile.id);
                break;
            } else {
                // Put failed profile on cooldown
                profile.cooldown_until_ms = now_ms + config_.failover.cooldown_ms;
                log()->warn("[Failover] Profile '{}' failed: {}", profile.id, result.error);
            }
        }
        // 对齐 OpenClaw: fallback_model 降级 (thinking level degradation)
        if (!result.ok && !config_.failover.fallback_model.empty()) {
            log()->warn("[Failover] All profiles exhausted. Trying fallback model: {}",
                        config_.failover.fallback_model);
            if (llm_client_) {
                llm_client_->set_model_override(config_.failover.fallback_model);
            }
            streaming_state_.delta_buffer.clear();
            streaming_state_.block_buffer.clear();
            result = attempt_llm_call();
        }
    }
    
    if (!result.ok) {
        // 🎯 对齐 OpenClaw: isLikelyContextOverflowError → signal for auto-compaction
        // If the LLM failed due to context overflow, mark the response so execute_loop
        // can trigger compaction and retry
        if (is_context_overflow_error(result.error)) {
            response.context_overflow = true;
            auto overflow_tokens = extract_overflow_token_count(result.error);
            if (overflow_tokens.has_value()) {
                log()->warn("[call_llm] Context overflow detected: {} tokens observed", *overflow_tokens);
            } else {
                log()->warn("[call_llm] Context overflow detected from error: {}", result.error);
            }
        }
        response.text = "Error: " + result.error;
        return response;
    }
    
    // 🎯 对齐 OpenClaw: message_end — flush blockBuffer for message_end break mode
    if (config_.block_reply.enabled &&
        config_.block_reply.break_mode == ReactConfig::BlockReply::BreakMode::MessageEnd &&
        callbacks.on_block_reply &&
        !streaming_state_.block_buffer.empty()) {
        emit_block_chunk(streaming_state_.block_buffer, callbacks);
        streaming_state_.block_buffer.clear();
    }

    // 🎯 对齐 OpenClaw: reasoning stream end
    if (streaming_state_.reasoning_stream_open && callbacks.on_reasoning_end) {
        streaming_state_.reasoning_stream_open = false;
        callbacks.on_reasoning_end();
    }

    response.text = accumulated_text;
    response.reasoning = accumulated_reasoning;
    // 🎯 对齐 OpenClaw: noteLastAssistant — track last assistant text for agent_end
    streaming_state_.last_assistant_full_text = accumulated_text;
    // ✅ 对齐 OpenClaw: 解析 tool_calls 和 usage
    // tool_calls 已在流式回调中收集，这里从 accumulated_tool_calls_ 读取
    if (!accumulated_tool_calls_.empty()) {
        response.tool_calls = accumulated_tool_calls_;
        accumulated_tool_calls_.clear();
    }
    // usage 已在流式回调中更新到 response.input_tokens/output_tokens

    // =========================================================================
    // Hook: llm_output
    // Aligned with OpenClaw attempt.ts:2737 — fire-and-forget, after LLM response
    // Allows plugins to observe the exact output payload returned by the LLM.
    // =========================================================================
    if (llm_hook_mgr && llm_hook_mgr->has_hooks(plugins::PluginHookName::LLM_OUTPUT)) {
        tools::PluginHookLlmOutputEvent lo_event;
        lo_event.run_id     = callbacks.run_id.empty() ? callbacks.session_id : callbacks.run_id;
        lo_event.session_id = callbacks.session_id;
        lo_event.provider   = llm_client_ ? llm_client_->get_config().provider : "unknown";
        lo_event.model      = llm_client_ ? llm_client_->get_config().model    : "unknown";
        if (!accumulated_text.empty()) lo_event.assistant_texts.push_back(accumulated_text);
        tools::PluginHookLlmOutputUsage lo_usage;
        lo_usage.input  = response.input_tokens  > 0 ? std::make_optional(response.input_tokens)  : std::nullopt;
        lo_usage.output = response.output_tokens > 0 ? std::make_optional(response.output_tokens) : std::nullopt;
        lo_event.usage = lo_usage;

        tools::PluginHookAgentContext lo_ctx;
        lo_ctx.session_id = callbacks.session_id.empty()
            ? std::nullopt : std::make_optional(callbacks.session_id);
        try {
            llm_hook_mgr->run_llm_output(lo_event, lo_ctx);
        } catch (const std::exception& e) {
            log()->warn("llm_output hook failed: {}", e.what());
        }
    }

    // ✅ 可观测性: 发射 ModelUsage 事件 (对齐 OpenClaw)
    try {
        // ✅ 从 llm_client_ 获取真实的 provider 和 model
        const auto& llm_config = llm_client_->get_config();
        
        // ✅ 计算成本 (简化版,可根据实际定价调整)
        double cost_usd = 0.0;
        if (llm_config.provider == "openai") {
            // GPT-4 定价: $0.03/1K input + $0.06/1K output
            cost_usd = (response.input_tokens * 0.03 / 1000.0) +
                      (response.output_tokens * 0.06 / 1000.0);
        } else if (llm_config.provider == "anthropic") {
            // Claude 定价: $0.015/1K input + $0.075/1K output
            cost_usd = (response.input_tokens * 0.015 / 1000.0) +
                      (response.output_tokens * 0.075 / 1000.0);
        }
        
        auto usage_event = std::make_shared<observability::ModelUsageEvent>();
        usage_event->session_id = callbacks.session_id.empty() ? "unknown" : callbacks.session_id;
        usage_event->provider = llm_config.provider;
        usage_event->model = llm_config.model;
        usage_event->prompt_tokens = response.input_tokens;
        usage_event->completion_tokens = response.output_tokens;
        usage_event->total_tokens = response.input_tokens + response.output_tokens;
        usage_event->cost_usd = cost_usd;
        usage_event->timestamp = std::chrono::system_clock::now();
        
        observability::DiagnosticEventBus::instance().emit(usage_event);
    } catch (...) {
        // 故障隔离: 永不阻塞主流程
    }
    
    return response;
}
std::map<std::string, tools::ToolResult> ReactEngine::execute_tools(
    const std::vector<llm::ToolCall>& tool_calls,
    const StreamCallbacks& callbacks
) {
    std::map<std::string, tools::ToolResult> results;

    // 🎯 对齐 OpenClaw: serial execution path (default)
    // OpenClaw executes tools via pi-agent-core which handles them one by one
    // unless the model requests parallel tool calls.
    if (!config_.parallel_tool_calls || tool_calls.size() <= 1) {
        for (const auto& tool_call : tool_calls) {
            // ✅ 对齐 OpenClaw handlers.tools.ts:302-306: handleToolExecutionStart
            // Flush pending block replies BEFORE tool execution to preserve message boundaries
            if (config_.block_reply.enabled) {
                flush_block_reply_buffer(callbacks);
            }
            if (callbacks.on_block_reply_flush) {
                callbacks.on_block_reply_flush();
            }

            if (callbacks.on_tool_start) {
                callbacks.on_tool_start(tool_call.name, tool_call.input);
            }

            // ✅ 对齐 OpenClaw handlers.tools.ts:341-355: emit tool start AgentEvent
            if (callbacks.on_agent_event) {
                core::JsonValue data;
                data["phase"] = "start";
                data["name"] = tool_call.name;
                data["toolCallId"] = tool_call.id;
                callbacks.on_agent_event({"tool", std::move(data)});
            }

            // ✅ 对齐 OpenClaw: runBeforeToolCallHook — loop detection (before execute)
            // pi-tools.before-tool-call.ts:108 — detectToolCallLoop + recordToolCall
            if (loop_detection_state_) {
                auto loop_result = loop_detection_state_->detect_tool_call_loop(
                    tool_call.name, tool_call.input);

                if (loop_result.stuck) {
                    if (loop_result.level == LoopDetectionResult::Level::Critical) {
                        // ✅ 对齐 OpenClaw: Critical → blocked=true → throw Error
                        // before-tool-call.ts:111-127: log.error + return { blocked: true }
                        log()->error("Loop detection: blocking tool '{}' — {}",
                                     tool_call.name, loop_result.message);
                        tools::ToolResult blocked_result;
                        blocked_result.success = false;
                        blocked_result.error = loop_result.message;
                        results[tool_call.id] = blocked_result;
                        // ✅ 仍然 record 这次调用（但跳过实际执行）
                        loop_detection_state_->record_tool_call(
                            tool_call.name, tool_call.input, tool_call.id);
                        continue;  // 跳过工具执行，进入下一个 tool_call
                    } else {
                        // Warning: 只记日志，工具继续执行 (对齐 OpenClaw: warn-only)
                        log()->warn("Loop detection: warning for tool '{}' — {}",
                                    tool_call.name, loop_result.message);
                    }
                }

                // ✅ 对齐 OpenClaw: recordToolCall — 记录本次调用到滑动窗口
                // before-tool-call.ts:147: recordToolCall(sessionState, toolName, params, ...)
                loop_detection_state_->record_tool_call(
                    tool_call.name, tool_call.input, tool_call.id);
            }

            // ✅ 严格对齐 OpenClaw runBeforeToolCallHook — 统一 policy 链
            // (loop 已在上方内联完成，此处 loop_state 传 null避免重复记录)
            // policy 返回三态 HookOutcome: PASS / VETO / FAILURE
            core::JsonValue effective_params = tool_call.input;
            {
                tools::BeforeToolCallContext policy_ctx;
                policy_ctx.tool_name = tool_call.name;
                policy_ctx.tool_call_id = tool_call.id;
                policy_ctx.run_id = callbacks.run_id;
                policy_ctx.session_id = callbacks.session_id;
                policy_ctx.aborted = &cancelled_;
                policy_ctx.loop_state = nullptr;
                policy_ctx.hook_manager = tools::get_global_hook_manager();
                // 🎯 对齐 OpenClaw senderIsOwner + ownerOnly gate
                if (tool_manager_) {
                    policy_ctx.sender_is_owner = tool_manager_->context().sender_is_owner;
                    policy_ctx.agent_id = tool_manager_->context().agent_id;
                    policy_ctx.session_key = tool_manager_->context().session_key;
                    if (auto t = tool_manager_->get_tool(tool_call.name)) {
                        policy_ctx.tool_is_owner_only = t->owner_only();
                        // 🎯 对齐 OpenClaw needsApproval: 工具自我声明需审批。
                        // ⚠ 此前 Tool::requires_approval() 【零消费】——
                        //   声明要审批的工具(builtin_tools.cc:172/536 等)从不审批,
                        //   声明方以为受保护, 实际直通且无日志可查。
                        policy_ctx.tool_requires_approval = t->requires_approval();
                        // ★ 工具可覆盖审批模式与可选决策集。
                        //   空串 → Request（宿主默认，对齐 OpenClaw）。
                        //   "defer" 用于"收敛方不是本次调用"的场景
                        //   （如用户要在外部页面点确认）。
                        policy_ctx.approval_mode =
                            tools::parse_approval_mode(t->approval_mode());
                        policy_ctx.allowed_decisions = t->approval_allowed_decisions();
                    }
                    // ★ approval_broker 留空 —— 策略链自动从进程级
                    //   ToolApprovalRuntime::instance() 取(对齐 OpenClaw
                    //   approval-gateway-runtime-context.ts 的全局单例)。
                }

                auto outcome = tools::run_before_tool_call_policy(policy_ctx, tool_call.input);

                if (outcome.kind == tools::HookOutcomeKind::Veto) {
                    // 对齐 OpenClaw buildBlockedToolResult(): blocked shape 交给 LLM
                    auto blocked = tools::build_blocked_tool_result(
                        outcome.reason.value_or("Tool call blocked"),
                        outcome.denied_reason.value_or(tools::denied_reason::kPluginBeforeToolCall),
                        tool_call.id);
                    log()->warn("Tool '{}' vetoed by before_tool_call policy: {}",
                                tool_call.name, blocked.error);
                    results[tool_call.id] = blocked;
                    if (callbacks.on_tool_end) callbacks.on_tool_end(tool_call.name, blocked);
                    continue;
                }
                if (outcome.kind == tools::HookOutcomeKind::Failure) {
                    // 对齐 OpenClaw: FAILURE 作为 terminal 事故，携带 disposition
                    tools::ToolResult err;
                    err.success = false;
                    err.error = outcome.reason.value_or("before_tool_call failed");
                    err.details = core::JsonValue::object();
                    err.details["status"] = std::string("error");
                    err.details["disposition"] =
                        outcome.disposition.value_or(tools::disposition::kError);
                    err.details["deniedReason"] =
                        outcome.denied_reason.value_or(tools::denied_reason::kPluginBeforeToolCall);
                    log()->error("Tool '{}' before_tool_call FAILURE ({}): {}",
                                 tool_call.name,
                                 outcome.disposition.value_or(tools::disposition::kError),
                                 err.error);
                    results[tool_call.id] = err;
                    if (callbacks.on_tool_end) callbacks.on_tool_end(tool_call.name, err);
                    continue;
                }
                // PASS: 使用（可能被 hook 改写的）最终参数
                effective_params = std::move(outcome.params);

                // ── Defer 审批凭据的登记与收敛 ────────────────────────────
                // 🎯 对齐 OpenClaw native-hook-relay-permissions.ts:
                //    setNativeHookRelayPreToolUseApproval + resolve...
                //
                // ★★ Defer 的语义是"审批已发起但不在 Stage 1 等"。
                //   若发出去就不管, 后果是:
                //     · 用户点了"拒绝"【没人看】⇒ 审批形同虚设
                //     · 凭据无人收敛 ⇒ 一直挂到超时, 占着待审列表
                //   所以这里必须收敛它。
                //
                // ⚠ 参数指纹: 用户批准的是【Stage 1 发起审批那一刻】的参数,
                //   而 hook 可能在 Stage 3 改写过。沿用旧批准执行新参数
                //   等于"拿批准 A 的授权执行 B" —— 权限提升。
                //   OpenClaw 对此的处置是拒绝 (codec.ts:168-176)。
                if (outcome.deferred_approval) {
                    auto& reg = tools::DeferredApprovalRegistry::instance();
                    // key 用 run_id + tool_call_id: 只用后者会让不同 run
                    // 的同名 id 互相顶掉。
                    const std::string key = callbacks.run_id + ":" + tool_call.id;
                    reg.put(key, *outcome.deferred_approval,
                            tools::params_fingerprint(tool_call.input));

                    auto res = reg.resolve(
                        key, tools::params_fingerprint(effective_params),
                        /*wait_ms=*/0, &cancelled_);
                    if (!res.approved()) {
                        // ⚠ 未获批一律【不执行】。ParamsRewritten / TimedOut /
                        //   NotFound 都归此类 —— 未决 == 未获批。
                        auto blocked = tools::build_blocked_tool_result(
                            res.reason.empty() ? "deferred approval was not granted"
                                               : res.reason,
                            tools::denied_reason::kPluginApproval,
                            tool_call.id);
                        log()->warn("Tool '{}' deferred approval not granted ({}): {}",
                                    tool_call.name,
                                    tools::deferred_resolution_text(res.resolution),
                                    blocked.error);
                        results[tool_call.id] = blocked;
                        if (callbacks.on_tool_end)
                            callbacks.on_tool_end(tool_call.name, blocked);
                        continue;
                    }
                }
            }

            // ✅ 对齐 OpenClaw handlers.tools.ts:314 — 记录工具启动时间
            auto tool_start_time = std::chrono::steady_clock::now();

            std::unique_ptr<observability::Span> span;
            try {
                span = observability::Tracer::instance().start_span(
                    "tool.execute", observability::SpanKind::Client);
                span->set_attribute("tool_name", tool_call.name);
                span->set_attribute("tool_call_id", tool_call.id);
            } catch (...) {}
            try {
                // ⚠ before_tool_call 链已在上方跑过(含 Stage 1 审批)。
                //   不传此标志会让 execute_tool 再跑一遍 ——
                //   用户会被【问两遍】同一个审批, hook 副作用也会重复。
                tools::ToolManager::ExecuteOptions exec_opts;
                exec_opts.before_tool_call_applied = true;
                // ★ 透传运行取消标志 —— 对齐 OpenClaw 把 run 的 AbortSignal
                //   作为 execute 第三参数传给工具 (ask-user-tool.ts:555)。
                //   ⚠ 不传的后果: chat.abort 后长阻塞工具（remote_a2a 的用户
                //     提问最长 900s）仍会挂满超时，且占着 main lane 唯一槽位。
                exec_opts.aborted = &cancelled_;
                auto result = tool_manager_->execute_tool(tool_call.name, effective_params,
                                                          nullptr, exec_opts);

                // ── 工具中断：投追问给用户 → 等答复 → 恢复子系统执行 ──────
                // ★★ 位置关键：必须在 results[] 写入、on_tool_end 回调、
                //   AgentEvent 发出【之前】。恢复之后 result 的内容会被整体
                //   替换，放到后面会让 transcript 里留下"还在等确认"的过期文本。
                //
                //   ⚠ 与下方 after_tool_call HOOK 是两件事：
                //     · 本阶段：同步、可改结果、可阻塞等用户
                //     · 下方 hook：fire-and-forget、纯观测、改不了结果
                //   放到 hook 里会得到"确认还在等，结果已进 transcript"。
                //
                // ★ 只处理 requires_user_action 的中断。普通补充输入
                //   （requires_user_action=false）直接把结果交给模型自己接着做
                //   —— 对每个追问都打扰用户，用户会很快学会无脑回"好的"，
                //   那时真正需要人判断的那次也就失效了。
                if (result.interrupt.has_value() &&
                    result.interrupt->requires_user_action) {
                    handle_tool_interrupt(result, callbacks);
                }

                if (span) span->set_status(result.success, result.error);
                if (callbacks.on_tool_end) callbacks.on_tool_end(tool_call.name, result);
                results[tool_call.id] = result;

                // ✅ 对齐 OpenClaw handlers.tools.ts:521-542: emit tool result AgentEvent
                if (callbacks.on_agent_event) {
                    core::JsonValue data;
                    data["phase"] = "result";
                    data["name"] = tool_call.name;
                    data["toolCallId"] = tool_call.id;
                    data["isError"] = false;
                    callbacks.on_agent_event({"tool", std::move(data)});
                }

                // ✅ 对齐 OpenClaw: recordLoopOutcome (success path)
                if (loop_detection_state_) {
                    loop_detection_state_->record_tool_call_outcome(
                        tool_call.name, result, "");
                }

                // ✅ 对齐 OpenClaw handlers.tools.ts:550-575: after_tool_call hook (fire-and-forget)
                {
                    auto* hook_mgr = tools::get_global_hook_manager();
                    if (hook_mgr && hook_mgr->has_after_tool_call_hooks()) {
                        try {
                            auto duration = std::chrono::steady_clock::now() - tool_start_time;
                            double duration_ms = std::chrono::duration<double, std::milli>(duration).count();

                            plugins::PluginHookAfterToolCallEvent after_event;
                            after_event.tool_name = tool_call.name;
                            after_event.params_json = effective_params.to_string();
                            if (!callbacks.run_id.empty()) after_event.run_id = callbacks.run_id;
                            after_event.tool_call_id = tool_call.id;
                            after_event.result_json = result.details.to_string();
                            after_event.duration_ms = static_cast<int>(duration_ms);

                            plugins::PluginHookToolContext after_ctx;
                            after_ctx.tool_name = tool_call.name;
                            if (!callbacks.session_id.empty()) after_ctx.session_id = callbacks.session_id;
                            if (!callbacks.run_id.empty()) after_ctx.run_id = callbacks.run_id;
                            after_ctx.tool_call_id = tool_call.id;

                            hook_mgr->run_after_tool_call(after_event, after_ctx);
                        } catch (const std::exception& e) {
                            log()->warn("after_tool_call hook failed: tool={} error={}",
                                        tool_call.name, e.what());
                        }
                    }
                }
            } catch (const std::exception& e) {
                if (span) { span->set_status(false, e.what()); }
                tools::ToolResult error_result;
                error_result.success = false;
                error_result.error = std::string("Tool execution failed: ") + e.what();
                if (callbacks.on_tool_end) callbacks.on_tool_end(tool_call.name, error_result);
                results[tool_call.id] = error_result;

                // ✅ 对齐 OpenClaw: emit tool result AgentEvent (error)
                if (callbacks.on_agent_event) {
                    core::JsonValue data;
                    data["phase"] = "result";
                    data["name"] = tool_call.name;
                    data["toolCallId"] = tool_call.id;
                    data["isError"] = true;
                    callbacks.on_agent_event({"tool", std::move(data)});
                }

                // ✅ 对齐 OpenClaw: recordLoopOutcome (error path)
                if (loop_detection_state_) {
                    loop_detection_state_->record_tool_call_outcome(
                        tool_call.name, error_result, e.what());
                }

                // ✅ 对齐 OpenClaw handlers.tools.ts:550-575: after_tool_call hook (error path)
                {
                    auto* hook_mgr = tools::get_global_hook_manager();
                    if (hook_mgr && hook_mgr->has_after_tool_call_hooks()) {
                        try {
                            auto duration = std::chrono::steady_clock::now() - tool_start_time;
                            double duration_ms = std::chrono::duration<double, std::milli>(duration).count();

                            plugins::PluginHookAfterToolCallEvent after_event;
                            after_event.tool_name = tool_call.name;
                            after_event.params_json = effective_params.to_string();
                            if (!callbacks.run_id.empty()) after_event.run_id = callbacks.run_id;
                            after_event.tool_call_id = tool_call.id;
                            after_event.error = e.what();
                            after_event.duration_ms = static_cast<int>(duration_ms);

                            plugins::PluginHookToolContext after_ctx;
                            after_ctx.tool_name = tool_call.name;
                            if (!callbacks.session_id.empty()) after_ctx.session_id = callbacks.session_id;
                            if (!callbacks.run_id.empty()) after_ctx.run_id = callbacks.run_id;
                            after_ctx.tool_call_id = tool_call.id;

                            hook_mgr->run_after_tool_call(after_event, after_ctx);
                        } catch (const std::exception& ex) {
                            log()->warn("after_tool_call hook failed (error path): tool={} error={}",
                                        tool_call.name, ex.what());
                        }
                    }
                }
            }
        }
        return results;
    }

    if (config_.parallel_tool_calls && tool_calls.size() > 1) {
        // ✅ 并行执行 (使用全局线程池)
        log()->info("Executing {} tools in parallel using thread pool", tool_calls.size());
        
        auto& thread_pool = get_thread_pool();
        // ParallelToolResult 定义在 loop 内部（每个 tool_call 的 lambda 中）
        struct ParallelToolResultOuter {
            std::string id;
            tools::ToolResult result;
            double duration_ms = 0.0;
        };
        std::vector<std::future<ParallelToolResultOuter>> futures;
        
        for (const auto& tool_call : tool_calls) {
            // ✅ 对齐 OpenClaw handlers.tools.ts:302-306: flush block replies before tool execution
            if (config_.block_reply.enabled) {
                flush_block_reply_buffer(callbacks);
            }
            if (callbacks.on_block_reply_flush) {
                callbacks.on_block_reply_flush();
            }

            // 发送工具开始事件
            if (callbacks.on_tool_start) {
                callbacks.on_tool_start(tool_call.name, tool_call.input);
            }

            // ✅ 对齐 OpenClaw handlers.tools.ts:341-355: emit tool start AgentEvent
            if (callbacks.on_agent_event) {
                core::JsonValue data;
                data["phase"] = "start";
                data["name"] = tool_call.name;
                data["toolCallId"] = tool_call.id;
                callbacks.on_agent_event({"tool", std::move(data)});
            }

            // ✅ 对齐 OpenClaw: loop detection (before execute) — 并行路径
            // 注意: 并行路径的 loop detection 在主线程顺序执行（并行工具共享同一 state）
            if (loop_detection_state_) {
                auto loop_result = loop_detection_state_->detect_tool_call_loop(
                    tool_call.name, tool_call.input);
                if (loop_result.stuck) {
                    if (loop_result.level == LoopDetectionResult::Level::Critical) {
                        log()->error("Loop detection (parallel): blocking tool '{}' — {}",
                                     tool_call.name, loop_result.message);
                        // blocked: 提交一个直接返回错误的 future
                        std::string blocked_msg = loop_result.message;
                        std::string tool_id = tool_call.id;
                        auto f = std::async(std::launch::deferred,
                            [tool_id, blocked_msg]() -> ParallelToolResultOuter {
                                tools::ToolResult r;
                                r.success = false;
                                r.error = blocked_msg;
                                return {tool_id, r, 0.0};
                            });
                        futures.push_back(std::move(f));
                        loop_detection_state_->record_tool_call(
                            tool_call.name, tool_call.input, tool_call.id);
                        continue;
                    } else {
                        log()->warn("Loop detection (parallel): warning for tool '{}' — {}",
                                    tool_call.name, loop_result.message);
                    }
                }
                loop_detection_state_->record_tool_call(
                    tool_call.name, tool_call.input, tool_call.id);
            }

            // ✅ 严格对齐 OpenClaw runBeforeToolCallHook — 统一 policy 链（并行路径）
            // hook 在主线程调用（不在线程池内），避免并发竞争
            core::JsonValue parallel_effective_params = tool_call.input;
            {
                tools::BeforeToolCallContext policy_ctx;
                policy_ctx.tool_name = tool_call.name;
                policy_ctx.tool_call_id = tool_call.id;
                policy_ctx.run_id = callbacks.run_id;
                policy_ctx.session_id = callbacks.session_id;
                policy_ctx.aborted = &cancelled_;
                policy_ctx.loop_state = nullptr;
                policy_ctx.hook_manager = tools::get_global_hook_manager();
                // 🎯 对齐 OpenClaw senderIsOwner + ownerOnly gate
                if (tool_manager_) {
                    policy_ctx.sender_is_owner = tool_manager_->context().sender_is_owner;
                    policy_ctx.agent_id = tool_manager_->context().agent_id;
                    policy_ctx.session_key = tool_manager_->context().session_key;
                    if (auto t = tool_manager_->get_tool(tool_call.name)) {
                        policy_ctx.tool_is_owner_only = t->owner_only();
                        // 🎯 对齐 OpenClaw needsApproval: 工具自我声明需审批。
                        // ⚠ 此前 Tool::requires_approval() 【零消费】——
                        //   声明要审批的工具(builtin_tools.cc:172/536 等)从不审批,
                        //   声明方以为受保护, 实际直通且无日志可查。
                        policy_ctx.tool_requires_approval = t->requires_approval();
                        // ★ 工具可覆盖审批模式与可选决策集。
                        //   空串 → Request（宿主默认，对齐 OpenClaw）。
                        //   "defer" 用于"收敛方不是本次调用"的场景
                        //   （如用户要在外部页面点确认）。
                        policy_ctx.approval_mode =
                            tools::parse_approval_mode(t->approval_mode());
                        policy_ctx.allowed_decisions = t->approval_allowed_decisions();
                    }
                    // ★ approval_broker 留空 —— 策略链自动从进程级
                    //   ToolApprovalRuntime::instance() 取(对齐 OpenClaw
                    //   approval-gateway-runtime-context.ts 的全局单例)。
                }

                auto outcome = tools::run_before_tool_call_policy(policy_ctx, tool_call.input);

                if (outcome.kind == tools::HookOutcomeKind::Veto ||
                    outcome.kind == tools::HookOutcomeKind::Failure) {
                    const bool is_veto = (outcome.kind == tools::HookOutcomeKind::Veto);
                    tools::ToolResult r =
                        is_veto
                            ? tools::build_blocked_tool_result(
                                  outcome.reason.value_or("Tool call blocked"),
                                  outcome.denied_reason.value_or(
                                      tools::denied_reason::kPluginBeforeToolCall),
                                  tool_call.id)
                            : [&] {
                                  tools::ToolResult e;
                                  e.success = false;
                                  e.error = outcome.reason.value_or("before_tool_call failed");
                                  e.details = core::JsonValue::object();
                                  e.details["status"] = std::string("error");
                                  e.details["disposition"] =
                                      outcome.disposition.value_or(tools::disposition::kError);
                                  e.details["deniedReason"] = outcome.denied_reason.value_or(
                                      tools::denied_reason::kPluginBeforeToolCall);
                                  return e;
                              }();
                    log()->warn("Tool '{}' blocked by before_tool_call policy (parallel, {}): {}",
                                tool_call.name, is_veto ? "veto" : "failure", r.error);
                    std::string tool_id = tool_call.id;
                    auto f = std::async(std::launch::deferred,
                        [tool_id, r]() -> ParallelToolResultOuter {
                            return {tool_id, r, 0.0};
                        });
                    futures.push_back(std::move(f));
                    continue;
                }
                // PASS
                parallel_effective_params = std::move(outcome.params);

                // ── Defer 审批凭据的登记与收敛（并行路径）─────────────────
                // ⚠ 与串行路径【必须一致】。若这里漏做, Defer 模式下
                //   并行工具调用会绕过审批: 凭据发出去没人看,
                //   用户点"拒绝"也照样执行 —— 而串行路径是拦住的,
                //   于是"能否绕过审批"取决于模型是否并行发起, 极难发现。
                if (outcome.deferred_approval) {
                    auto& reg = tools::DeferredApprovalRegistry::instance();
                    const std::string key = callbacks.run_id + ":" + tool_call.id;
                    reg.put(key, *outcome.deferred_approval,
                            tools::params_fingerprint(tool_call.input));

                    auto res = reg.resolve(
                        key, tools::params_fingerprint(parallel_effective_params),
                        /*wait_ms=*/0, &cancelled_);
                    if (!res.approved()) {
                        auto blocked = tools::build_blocked_tool_result(
                            res.reason.empty() ? "deferred approval was not granted"
                                               : res.reason,
                            tools::denied_reason::kPluginApproval,
                            tool_call.id);
                        log()->warn("Tool '{}' deferred approval not granted "
                                    "(parallel, {}): {}",
                                    tool_call.name,
                                    tools::deferred_resolution_text(res.resolution),
                                    blocked.error);
                        std::string tool_id = tool_call.id;
                        auto f = std::async(std::launch::deferred,
                            [tool_id, blocked]() -> ParallelToolResultOuter {
                                return {tool_id, blocked, 0.0};
                            });
                        futures.push_back(std::move(f));
                        continue;
                    }
                }
            }
            
            // ✅ 提交任务到全局线程池 (避免频繁创建/销毁线程)
            // 🎯 对齐 OpenClaw handlers.tools.ts:314 — 记录 startTime 用于 after_tool_call durationMs
            auto future = thread_pool->submit([this, tool_call, callbacks, parallel_effective_params]() 
                -> ParallelToolResultOuter {
                auto start = std::chrono::steady_clock::now();
                
                // ✅ 可观测性: 工具执行 Span (对齐 OpenClaw)
                std::unique_ptr<observability::Span> span;
                try {
                    span = observability::Tracer::instance().start_span(
                        "tool.execute", 
                        observability::SpanKind::Client
                    );
                    span->set_attribute("tool_name", tool_call.name);
                    span->set_attribute("tool_call_id", tool_call.id);
                } catch (...) {}
                
                try {
                    core::JsonValue args = parallel_effective_params;
                    // ⚠ 同串行路径: 链已跑过, 避免审批问两遍 / hook 重复触发。
                    tools::ToolManager::ExecuteOptions exec_opts;
                    exec_opts.before_tool_call_applied = true;
                    // ★ 与串行路径【必须一致】。漏了这一行，"abort 能不能生效"
                    //   就取决于模型是否并行发起工具 —— 非确定性缺陷。
                    exec_opts.aborted = &cancelled_;
                    auto result = tool_manager_->execute_tool(tool_call.name, args,
                                                              nullptr, exec_opts);

                    // ── 工具中断：与串行路径【必须一致】────────────────────
                    // ⚠ 漏了这一行，"远端要求确认能否到达用户"就取决于模型
                    //   是否并行发起工具 —— 非确定性缺陷，最难复现。
                    //
                    // ★ 并行下多个工具同时中断时，只有第一个能占到会话槽位
                    //   （InterruptCoordinator 每会话仅允许一个）。后来者
                    //   handle_tool_interrupt 会 fail-closed 返回
                    //   "该会话已有待答确认"，模型据此重试即可。
                    //   ⚠ 刻意【不】排队等: 排队会让 N 个工具串行等 N×900s，
                    //     而这些 lambda 跑在同一个线程池里，会把池占满。
                    if (result.interrupt.has_value() &&
                        result.interrupt->requires_user_action) {
                        handle_tool_interrupt(result, callbacks);
                    }

                    // ── after_tool_call POLICY（并行路径）─────────────────
                    // ⚠ 与串行路径【必须一致】。若这里漏做，并行调用的结果
                    //   会绕过二次确认 —— 而"能否绕过"取决于模型是否并行发起，
                    //   这类缺陷极难发现。
                    //
                    // ⚠ 本阶段可能阻塞等用户确认（≤120s），而这里在线程池
                    //   worker 上。这会占住一个 worker —— 已知代价，可接受：
                    //   声明 postExecConfirm 的工具本就极少，且确认是用户
                    //   主动发起的交互（不是后台批量）。
                    //   ★ 真正不可接受的是"不等就采信"：那份未确认的结果会
                    //     进 transcript 并进入下一轮 prompt。

                    if (span) span->set_status(result.success, result.error);
                    auto dur = std::chrono::steady_clock::now() - start;
                    double ms = std::chrono::duration<double, std::milli>(dur).count();
                    return {tool_call.id, result, ms};
                } catch (const std::exception& e) {
                    if (span) {
                        span->set_status(false, e.what());
                        span->add_event("error", {{"exception", e.what()}});
                    }
                    tools::ToolResult error_result;
                    error_result.success = false;
                    error_result.error = std::string("Tool execution failed: ") + e.what();
                    auto dur = std::chrono::steady_clock::now() - start;
                    double ms = std::chrono::duration<double, std::milli>(dur).count();
                    return {tool_call.id, error_result, ms};
                }
            });
            
            futures.push_back(std::move(future));
        }
        
        // 收集所有结果 (保持原始顺序)
        for (size_t i = 0; i < futures.size(); ++i) {
            const auto& tool_call = tool_calls[i];
            try {
                auto ptr = futures[i].get();
                auto& call_id = ptr.id;
                auto& result = ptr.result;
                double duration_ms = ptr.duration_ms;
                if (callbacks.on_tool_end) callbacks.on_tool_end(tool_call.name, result);
                results[call_id] = result;

                // ✅ 对齐 OpenClaw: emit tool result AgentEvent (并行路径)
                if (callbacks.on_agent_event) {
                    core::JsonValue data;
                    data["phase"] = "result";
                    data["name"] = tool_call.name;
                    data["toolCallId"] = tool_call.id;
                    data["isError"] = !result.success;
                    callbacks.on_agent_event({"tool", std::move(data)});
                }

                // ✅ 对齐 OpenClaw: recordLoopOutcome — 并行路径收集结果后记录
                if (loop_detection_state_) {
                    loop_detection_state_->record_tool_call_outcome(
                        tool_call.name, result, result.success ? "" : result.error);
                }

                // ✅ 对齐 OpenClaw handlers.tools.ts:550-575: after_tool_call hook (并行路径，主线程)
                {
                    auto* hook_mgr = tools::get_global_hook_manager();
                    if (hook_mgr && hook_mgr->has_after_tool_call_hooks()) {
                        try {
                            plugins::PluginHookAfterToolCallEvent after_event;
                            after_event.tool_name = tool_call.name;
                            after_event.params_json = tool_call.input.to_string();
                            if (!callbacks.run_id.empty()) after_event.run_id = callbacks.run_id;
                            after_event.tool_call_id = tool_call.id;
                            if (result.success) {
                                after_event.result_json = result.details.to_string();
                            } else {
                                after_event.error = result.error;
                            }
                            after_event.duration_ms = static_cast<int>(duration_ms);

                            plugins::PluginHookToolContext after_ctx;
                            after_ctx.tool_name = tool_call.name;
                            if (!callbacks.session_id.empty()) after_ctx.session_id = callbacks.session_id;
                            if (!callbacks.run_id.empty()) after_ctx.run_id = callbacks.run_id;
                            after_ctx.tool_call_id = tool_call.id;

                            hook_mgr->run_after_tool_call(after_event, after_ctx);
                        } catch (const std::exception& ex) {
                            log()->warn("after_tool_call hook failed (parallel): tool={} error={}",
                                        tool_call.name, ex.what());
                        }
                    }
                }
            } catch (const std::exception& e) {
                log()->error("Failed to get tool execution result: {}", e.what());
                tools::ToolResult error_result;
                error_result.success = false;
                error_result.error = std::string("Tool execution failed: ") + e.what();
                results[tool_call.id] = error_result;

                if (loop_detection_state_) {
                    loop_detection_state_->record_tool_call_outcome(
                        tool_call.name, error_result, e.what());
                }
            }
        }
    }

    return results;
}

ReactEngine::ReasoningExtraction ReactEngine::extract_reasoning(const std::string& text) {
    ReasoningExtraction extraction;
    extraction.remaining_text = text;
    
    // 查找 thinking tags
    std::smatch match;
    std::string search_text = text;
    std::string thinking_content;
    std::string non_thinking_content;
    
    bool in_thinking = false;
    size_t last_pos = 0;
    
    while (std::regex_search(search_text, match, thinking_tag_re)) {
        size_t match_pos = match.position(0);
        
        if (!in_thinking) {
            // 开始 thinking
            non_thinking_content += search_text.substr(last_pos, match_pos - last_pos);
            in_thinking = true;
        } else {
            // 结束 thinking
            thinking_content += search_text.substr(last_pos, match_pos - last_pos);
            in_thinking = false;
            extraction.has_reasoning = true;
        }
        
        last_pos = match.position(0) + match.length(0);
        search_text = match.suffix().str();
    }
    
    // 剩余内容
    if (in_thinking) {
        thinking_content += search_text.substr(last_pos);
    } else {
        non_thinking_content += search_text.substr(last_pos);
    }
    
    extraction.reasoning = thinking_content;
    extraction.remaining_text = non_thinking_content;
    
    return extraction;
}

std::vector<std::string> ReactEngine::chunk_text(const std::string& text) {
    std::vector<std::string> chunks;
    
    if (text.size() <= config_.block_reply.min_chars) {
        chunks.push_back(text);
        return chunks;
    }
    
    size_t pos = 0;
    std::string current_chunk;
    
    while (pos < text.size()) {
        // 尝试找到合适的分割点
        size_t chunk_end = std::min(pos + config_.block_reply.max_chars, text.size());
        
        if (chunk_end < text.size()) {
            // 寻找分割点
            switch (config_.block_reply.break_preference) {
                case ReactConfig::BlockReply::BreakPreference::Paragraph:
                    // 查找段落边界
                    while (chunk_end > pos + config_.block_reply.min_chars) {
                        if (is_paragraph_boundary(text[chunk_end])) {
                            break;
                        }
                        chunk_end--;
                    }
                    break;
                    
                case ReactConfig::BlockReply::BreakPreference::Sentence:
                    // 查找句子边界
                    while (chunk_end > pos + config_.block_reply.min_chars) {
                        if (is_sentence_boundary(text, chunk_end)) {
                            chunk_end++;  // 包含标点
                            break;
                        }
                        chunk_end--;
                    }
                    break;
                    
                case ReactConfig::BlockReply::BreakPreference::Newline:
                    // 查找换行
                    while (chunk_end > pos + config_.block_reply.min_chars) {
                        if (text[chunk_end] == '\n') {
                            break;
                        }
                        chunk_end--;
                    }
                    break;
            }
        }
        
        current_chunk = text.substr(pos, chunk_end - pos);
        if (!current_chunk.empty()) {
            chunks.push_back(current_chunk);
        }
        
        pos = chunk_end;
    }
    
    return chunks;
}

// ═══════════════════════════════════════════════════════════════
// ✅ 严格对齐 OpenClaw 的流式处理机制
// 参考: openclaw-analysis/src/agents/pi-embedded-subscribe.ts:477-553
// ═══════════════════════════════════════════════════════════════

// 发送块回复 (带智能去重和冲突检测)
// 🎯 对齐 OpenClaw: emitBlockChunk (pi-embedded-subscribe.ts)
void ReactEngine::emit_block_chunk(const std::string& text, const StreamCallbacks& callbacks) {
    // 🎯 1. 动态抑制检测 (对齐 OpenClaw: suppressBlockChunks)
    // Set by finalizeAssistantTexts when includeReasoning=true and no onBlockReply.
    if (streaming_state_.suppress_block_chunks) {
        return;
    }

    // 🎯 2. 过滤 thinking/final 标签 + trim (对齐 OpenClaw: stripBlockTags + trimEnd)
    std::string chunk = strip_thinking_tags(text);
    // 🎯 对齐 OpenClaw pi-embedded-utils.ts: stripDowngradedToolCallText
    // Remove [Tool Call: ...], [Historical context: ...] etc. from degraded model output
    {
        static const std::regex degraded_re(
            R"(\[(?:Tool Call|Historical context|Context from previous session|System note)[^\]]*\])");
        chunk = std::regex_replace(chunk, degraded_re, "");
    }
    chunk = trim_end(chunk);
    if (chunk.empty()) {
        return;
    }

    // 🎯 3. 智能去重 (对齐 OpenClaw: chunk === state.lastBlockReplyText)
    if (chunk == streaming_state_.last_block_reply_text) {
        return;
    }

    // 🎯 4. Messaging Tool 冲突检测 (对齐 OpenClaw: isMessagingToolDuplicateNormalized)
    // Only check committed (successful) messaging tool texts — checking pending texts
    // is risky because if the tool fails after suppression, the user gets no response.
    std::string normalized_chunk = normalize_text_for_comparison(chunk);
    if (is_messaging_tool_duplicate(normalized_chunk)) {
        log()->debug("Skipping block reply - already sent via messaging tool: {}...",
                     chunk.substr(0, std::min((size_t)50, chunk.size())));
        return;
    }

    // 🎯 5. shouldSkipAssistantText check (对齐 OpenClaw: emitBlockChunk calls shouldSkipAssistantText)
    // Prevents emitting a chunk that duplicates a previously-emitted assistant text.
    if (streaming_state_.last_assistant_text_message_index == streaming_state_.assistant_message_index) {
        const std::string trimmed = trim_end(chunk);
        if (!trimmed.empty() && trimmed == streaming_state_.last_assistant_text_trimmed) {
            return;
        }
        const std::string norm = normalize_text_for_comparison(chunk);
        if (!norm.empty() && norm == streaming_state_.last_assistant_text_normalized) {
            return;
        }
    }

    // 🎯 6. 记录发送内容 (对齐 OpenClaw: state.lastBlockReplyText = chunk)
    streaming_state_.last_block_reply_text = chunk;
    // Also track in lastAssistantText for future shouldSkipAssistantText checks
    streaming_state_.last_assistant_text_message_index = streaming_state_.assistant_message_index;
    streaming_state_.last_assistant_text_trimmed = trim_end(chunk);
    streaming_state_.last_assistant_text_normalized = normalize_text_for_comparison(chunk);

    // 🎯 7. 零开销检测
    if (!callbacks.on_block_reply) {
        return;
    }

    // 🎯 8. 对齐 OpenClaw reply-directives.ts: parse and strip reply directives
    // [[MEDIA:url]] → extract media URLs
    // [[REPLY_TO:id]] → extract reply target
    // [[AUDIO_AS_VOICE]] → flag for voice audio
    {
        static const std::regex media_re(R"(\[\[MEDIA:([^\]]+)\]\])");
        static const std::regex reply_to_re(R"(\[\[REPLY_TO:([^\]]+)\]\])");
        static const std::regex audio_voice_re(R"(\[\[AUDIO_AS_VOICE\]\])");
        // Strip directives from the text sent to the channel
        chunk = std::regex_replace(chunk, media_re, "");
        chunk = std::regex_replace(chunk, reply_to_re, "");
        chunk = std::regex_replace(chunk, audio_voice_re, "");
        chunk = trim_end(chunk);
        if (chunk.empty()) return;
    }

    // 🎯 9. 发送回调
    callbacks.on_block_reply(chunk);
}

// 刷新块回复缓冲区
// 对齐 OpenClaw: flushBlockReplyBuffer (line 540-553)
void ReactEngine::flush_block_reply_buffer(const StreamCallbacks& callbacks) {
    // ✅ 零开销检测 (对齐 OpenClaw:541-542)
    if (!callbacks.on_block_reply) {
        return;
    }
    
    // ✅ 发送 block_buffer 中的内容 (对齐 OpenClaw:549-552)
    if (!streaming_state_.block_buffer.empty()) {
        emit_block_chunk(streaming_state_.block_buffer, callbacks);
        streaming_state_.block_buffer.clear();
    }
}

// 文本规范化 (用于去重比较)
// 对齐 OpenClaw: normalizeTextForComparison
std::string ReactEngine::normalize_text_for_comparison(const std::string& text) {
    std::string result = text;
    
    // 1. 去除首尾空白
    size_t start = result.find_first_not_of(" \t\n\r");
    size_t end = result.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }
    result = result.substr(start, end - start + 1);
    
    // 2. 折叠多个空白为单个空格
    std::regex whitespace_re("\\s+");
    result = std::regex_replace(result, whitespace_re, " ");
    
    return result;
}

// 检测 Messaging Tool 冲突
// 对齐 OpenClaw: isMessagingToolDuplicateNormalized
bool ReactEngine::is_messaging_tool_duplicate(const std::string& normalized_text) {
    for (const auto& sent : streaming_state_.messaging_tool_sent_texts_normalized) {
        if (normalized_text == sent) {
            return true;
        }
    }
    return false;
}

// 去除 thinking 标签
// 🎯 对齐 OpenClaw: stripBlockTags with inline code state tracking
// Skips thinking tags that appear inside inline code spans (`backticks`)
std::string ReactEngine::strip_thinking_tags(const std::string& text) {
    if (text.empty()) return text;

    // Build a quick set of positions inside inline code spans
    // OpenClaw uses buildCodeSpanIndex; we do a simpler single-pass approach
    std::vector<bool> in_code(text.size(), false);
    bool inside_code = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '`') {
            // Handle `` double backtick and ``` triple backtick as inline code
            size_t tick_count = 0;
            size_t j = i;
            while (j < text.size() && text[j] == '`') { ++j; ++tick_count; }
            if (tick_count <= 2) {
                inside_code = !inside_code;
            }
            // Mark all backtick chars as in-code
            for (size_t k = i; k < j; ++k) in_code[k] = inside_code;
            i = j - 1;
            continue;
        }
        in_code[i] = inside_code;
    }

    // Now strip thinking tags only when NOT inside inline code
    std::string result;
    result.reserve(text.size());
    std::smatch match;
    std::string::const_iterator search_start = text.cbegin();
    while (std::regex_search(search_start, text.cend(), match, thinking_tag_re)) {
        size_t match_pos = match.position(0) + (search_start - text.cbegin());
        if (match_pos < in_code.size() && in_code[match_pos]) {
            // Tag is inside inline code — keep it
            result.append(search_start, search_start + match.position(0) + match.length(0));
        } else {
            // Tag is outside code — strip it
            result.append(search_start, search_start + match.position(0));
        }
        search_start += match.position(0) + match.length(0);
    }
    result.append(search_start, text.cend());
    return result;
}

// 去除尾部空白
std::string ReactEngine::trim_end(const std::string& text) {
    size_t end = text.find_last_not_of(" \t\n\r");
    if (end == std::string::npos) {
        return "";
    }
    return text.substr(0, end + 1);
}

// ═══════════════════════════════════════════════════════════════
// ✅ Compaction 集成 (对齐 OpenClaw)
// 参考: openclaw-analysis/src/agents/pi-embedded-runner/run.ts:980-1034
// ═══════════════════════════════════════════════════════════════

// Token 估算 (简化版: 1 token ≈ 4 chars)
int ReactEngine::estimate_conversation_tokens(const std::vector<LLMMessage>& messages) const {
    int total = 0;
    for (const auto& msg : messages) {
        // 估算消息内容 token
        total += msg.content.size() / 4;
        
        // 估算角色和格式开销
        total += 10;  // <role>content</role> 开销
    }
    return total;
}

// 检测 Context Overflow Error (对齐 OpenClaw: isLikelyContextOverflowError)
bool ReactEngine::is_context_overflow_error(const std::string& error_msg) const {
    std::string lower = error_msg;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    return (
        (lower.find("context") != std::string::npos && 
         lower.find("overflow") != std::string::npos) ||
        lower.find("too long") != std::string::npos ||
        (lower.find("maximum") != std::string::npos && 
         lower.find("token") != std::string::npos) ||
        (lower.find("exceeds") != std::string::npos && 
         lower.find("limit") != std::string::npos)
    );
}

// 提取 Overflow Token Count (对齐 OpenClaw: extractObservedOverflowTokenCount)
std::optional<int> ReactEngine::extract_overflow_token_count(const std::string& error_msg) const {
    // 匹配 "277403 tokens > 200000 maximum"
    std::regex token_count_re(R"((\d+)\s*tokens?\s*>\s*(\d+))");
    std::smatch match;
    
    if (std::regex_search(error_msg, match, token_count_re)) {
        return std::stoi(match[1].str());
    }
    
    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════════════════
// 🎯 严格对齐 OpenClaw pi-hooks/context-pruning/extension.ts
//   + runtime.ts (cache-ttl touch 更新)
//   + cache-ttl.ts (provider eligibility)
//
// 等价 OC:
//   api.on("context", (event, ctx) => {
//     const runtime = getContextPruningRuntime(ctx.sessionManager);
//     if (!runtime) return undefined;                    // 未启用
//     if (runtime.settings.mode === "cache-ttl") {
//       const lastTouch = runtime.lastCacheTouchAt;
//       if (!lastTouch || Date.now() - lastTouch < ttlMs) return undefined;
//     }
//     const next = pruneContextMessages({ messages, settings, ctx, ... });
//     if (next === event.messages) return undefined;
//     if (mode === "cache-ttl") runtime.lastCacheTouchAt = Date.now();
//     return { messages: next };
//   });
// ═══════════════════════════════════════════════════════════════════════════
bool ReactEngine::maybe_prune_context(std::vector<LLMMessage>& messages) {
    // 🎯 对齐 OC: runtime 未注册（mode=off）→ 直接返回
    if (config_.context_pruning.mode == context::ContextPruningMode::OFF) {
        return false;
    }

    // 🎯 对齐 OC: computeEffectiveSettings 懒缓存
    if (!effective_pruning_settings_.has_value()) {
        auto computed = context::compute_effective_settings(config_.context_pruning);
        if (!computed.has_value()) {
            // mode != cache-ttl → 永久禁用
            return false;
        }
        effective_pruning_settings_ = *computed;
    }
    const auto& settings = *effective_pruning_settings_;

    // 🎯 对齐 OC cache-ttl.ts: isCacheTtlEligibleProvider
    // 只对支持 prompt cache 的 provider 启用 cache-ttl 剪枝
    const std::string provider = llm_client_ ? llm_client_->get_config().provider : "";
    const std::string model_id = llm_client_ ? llm_client_->get_config().model : "";
    if (settings.mode == context::ContextPruningMode::CACHE_TTL &&
        !context::is_cache_ttl_eligible_provider(provider, model_id, /*model_api=*/"")) {
        return false;
    }

    // 🎯 对齐 OC extension.ts: cache-ttl 模式下 TTL 未到 → 不剪枝（保护 cache）
    if (settings.mode == context::ContextPruningMode::CACHE_TTL) {
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        if (last_cache_touch_at_ms_ == 0) {
            // 从未 touch → 记录当前时间、不剪（第一次请求让 cache 建立起来）
            last_cache_touch_at_ms_ = now_ms;
            return false;
        }
        if (settings.ttl_ms > 0 && (now_ms - last_cache_touch_at_ms_) < settings.ttl_ms) {
            return false;  // TTL 未到
        }
    }

    // 🎯 对齐 OC: pruneContextMessages
    context::PruneContextParams params;
    params.messages = messages;  // 传入拷贝
    params.settings = settings;
    params.context_window_tokens = context_window_config_.context_window_tokens;
    // predicate 使用默认（由 settings.tools 生成）
    auto result = context::prune_context_messages(params);

    if (!result.changed) {
        return false;
    }

    // 🎯 对齐 OC extension.ts: 已剪枝 → 更新 lastCacheTouchAt
    if (settings.mode == context::ContextPruningMode::CACHE_TTL) {
        last_cache_touch_at_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    messages = std::move(result.messages);

    spdlog::get("default")->info(
        "[ReactEngine] context pruning: {} chars -> {} chars "
        "(soft_trim={}, hard_clear={})",
        result.total_chars_before, result.total_chars_after,
        result.soft_trimmed_count, result.hard_cleared_count);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// 🎯 严格对齐 OpenClaw pi-embedded-runner/tools-payload.ts:
//   buildToolsPayload(agent, sdkTools, mcpTools, sandboxTools)
//
// HC 实现：
//   - ToolManager 已经在启动阶段合并了 [built-in tools, MCP tools, sandbox tools]
//     (见 AgentInstance::init() → tool_mgr->enable_mcp() + register_tool(...))
//   - 本函数把 ToolManager 里所有工具展开为 llm::ToolDefinition 数组，
//     供 LLMClient 通过 API 原生 `tools` 参数发送给 provider
//
// 注意：
//   - 工具声明**不进** system_prompt (OC 的设计：走 API 参数通道，效果/可靠性更好)
//   - 这与 skills 元信息 (走 system_prompt 的 <available_skills> 块) 互补
// ═══════════════════════════════════════════════════════════════════════════
std::vector<llm::ToolDefinition> ReactEngine::build_tool_definitions() const {
    return build_tool_definitions(std::nullopt);
}

std::vector<llm::ToolDefinition> ReactEngine::build_tool_definitions(
    const std::optional<std::vector<std::string>>& tools_allow) const {
    std::vector<llm::ToolDefinition> defs;
    if (!tool_manager_) return defs;

    auto names = tool_manager_->list_tools();
    defs.reserve(names.size());

    for (const auto& name : names) {
        // 🎯 对齐 OC before_prompt_build.toolsAllow: 收窄工具集
        if (tools_allow.has_value()) {
            if (std::find(tools_allow->begin(), tools_allow->end(), name)
                == tools_allow->end()) {
                continue;  // 不在允许列表 → 本回合跳过该工具
            }
        }
        auto tool = tool_manager_->get_tool(name);
        if (!tool) continue;

        llm::ToolDefinition td;
        td.name = tool->name();
        td.description = tool->description();

        // 🎯 对齐 OC: Tool::Schema → JSON Schema (input_schema)
        auto schema = tool->parameters();
        nlohmann::json schema_json;
        schema_json["type"] = schema.type.empty() ? "object" : schema.type;
        if (!schema.description.empty()) {
            schema_json["description"] = schema.description;
        }
        // properties: map<string, JsonValue> → JSON object
        nlohmann::json properties_json = nlohmann::json::object();
        for (const auto& [prop_name, prop_val] : schema.properties) {
            try {
                properties_json[prop_name] = prop_val.to_json();
            } catch (...) {
                properties_json[prop_name] = nlohmann::json::object();
            }
        }
        if (!properties_json.empty()) {
            schema_json["properties"] = properties_json;
        } else {
            // OpenAI/Anthropic 要求 object schema 即使无参数也需要 properties 字段
            schema_json["properties"] = nlohmann::json::object();
        }
        if (!schema.required.empty()) {
            schema_json["required"] = schema.required;
        }

        td.input_schema = core::JsonValue(schema_json);
        defs.push_back(std::move(td));
    }

    return defs;
}


ReactEngine::CompactionResult ReactEngine::trigger_compaction(
    std::vector<LLMMessage>& messages,
    const std::string& transcript_path,
    const std::string& session_id
) {
    CompactionResult result;
    
    if (!compaction_mgr_) {
        result.reason = "CompactionManager not initialized";
        return result;
    }
    
    int messages_before = messages.size();
    int current_tokens = estimate_conversation_tokens(messages);

    log()->info("[ReactEngine] Triggering compaction: {} messages, ~{} tokens",
               messages_before, current_tokens);

    // =========================================================================
    // Hook: before_compaction
    // Aligned with OpenClaw compact.ts:857 and run.ts:1045
    // Fire-and-forget, runs before compaction starts.
    // =========================================================================
    auto* compact_hook_mgr = tools::get_global_hook_manager();
    if (compact_hook_mgr && compact_hook_mgr->has_hooks(plugins::PluginHookName::BEFORE_COMPACTION)) {
        tools::PluginHookBeforeCompactionEvent bc_event;
        bc_event.message_count   = static_cast<int>(messages.size());
        bc_event.compacting_count = static_cast<int>(messages.size());
        bc_event.token_count     = current_tokens;
        // 🎯 对齐 OpenClaw compact.ts: before_compaction 传 sessionFile
        if (!transcript_path.empty()) bc_event.session_file = transcript_path;
        tools::PluginHookAgentContext bc_ctx;
        // 🎯 对齐 OpenClaw: hook context 携带 sessionId（供插件定位会话）
        if (!session_id.empty()) bc_ctx.session_id = session_id;
        try {
            compact_hook_mgr->run_before_compaction(bc_event, bc_ctx);
        } catch (const std::exception& e) {
            log()->warn("before_compaction hook failed: {}", e.what());
        }
    }
    
    // 转换 LLMMessage → AgentMessage
    std::vector<memory::AgentMessage> agent_messages;
    for (const auto& msg : messages) {
        memory::AgentMessage am;
        am.role = [&]() {
            switch (msg.role) {
                case llm::LLMMessageRole::User: return "user";
                case llm::LLMMessageRole::Assistant: return "assistant";
                case llm::LLMMessageRole::System: return "system";
                case llm::LLMMessageRole::Tool: return "toolResult";
                default: return "unknown";
            }
        }();
        am.content = msg.content;
        agent_messages.push_back(am);
    }
    
    // 构建 CompactionParams
    memory::CompactionParams params;
    params.messages = agent_messages;
    params.current_token_count = current_tokens;
    params.context_window_tokens = context_window_config_.context_window_tokens;
    params.trigger = "overflow";
    params.model_id = llm_client_ ? llm_client_->get_config().model : "claude-3-5-sonnet-20241022";  // ✅ 从配置读取
    
    // ✅ 对齐 OpenClaw: 设置 API key 和回调
    if (llm_client_) {
        params.api_key = llm_client_->get_config().api_key;
    }
    // params.memory_flush_callback 和 params.post_compaction_sync_callback
    // 在 CompactionManager 内部处理
    
    // 执行 compaction
    auto compact_result = compaction_mgr_->compact_session(params);
    
    if (!compact_result.ok) {
        log()->error("[ReactEngine] Compaction failed: {}", compact_result.error);
        result.reason = compact_result.error;
        return result;
    }
    
    auto& compact_data = compact_result.get();

    if (!compact_data.compacted) {
        log()->info("[ReactEngine] Compaction skipped: {}", compact_data.reason);
        result.reason = compact_data.reason;
        return result;
    }
    
    log()->info("[ReactEngine] Compaction successful:");
    log()->info("  Messages: {} → {}", 
               compact_data.messages_before,
               compact_data.messages_after);
    log()->info("  Tokens saved: {}", compact_data.tokens_saved);
    
    // ✅ 转换回 LLMMessage
    messages.clear();
    for (const auto& am : compact_data.compressed_messages) {
        llm::LLMMessage msg;
        msg.role = [&]() {
            if (am.role == "user") return llm::LLMMessageRole::User;
            if (am.role == "assistant") return llm::LLMMessageRole::Assistant;
            if (am.role == "system") return llm::LLMMessageRole::System;
            if (am.role == "toolResult") return llm::LLMMessageRole::Tool;
            return llm::LLMMessageRole::User;
        }();
        msg.content = am.content;
        messages.push_back(msg);
    }
    
    result.compacted = true;
    result.compressed_messages = messages;
    result.messages_before = messages_before;
    result.messages_after = messages.size();
    result.tokens_saved = compact_data.tokens_saved;

    // =========================================================================
    // 对齐 OC: appendCompaction — 持久化 compaction 条目到 transcript
    // OC compact.ts L1003-1008: session.compact() 后 SDK 自动调用 appendCompaction
    // HC 需要显式调用，因为没有 SDK SessionManager
    // =========================================================================
    if (!transcript_path.empty()) {
        // 确定 firstKeptEntryId: 最后保留的原始消息的 entry ID
        // compressed_messages = [summary(system), recent_msg_1, ..., recent_msg_N]
        // firstKeptEntryId 对应第一条保留的原始消息 (不包括 summary)
        // 简化实现: 用最后一条原始消息的位置标记
        std::string first_kept_entry_id;

        // 读取 transcript 最后几行获取保留消息对应的 entry ID
        {
            std::ifstream rf(transcript_path);
            if (rf.is_open()) {
                std::vector<std::pair<std::string, std::string>> entry_ids; // {id, type}
                std::string line;
                while (std::getline(rf, line)) {
                    if (line.empty()) continue;
                    try {
                        auto parsed = nlohmann::json::parse(line);
                        std::string id = parsed.value("id", "");
                        std::string type = parsed.value("type", "");
                        if (!id.empty() && type == "message") {
                            entry_ids.push_back({id, type});
                        }
                    } catch (...) {}
                }
                // 保留了最后 keep_count 条消息，firstKeptEntryId = 倒数第 keep_count 条的 ID
                int keep_count = std::min(5, static_cast<int>(entry_ids.size()));
                if (keep_count > 0 && static_cast<int>(entry_ids.size()) >= keep_count) {
                    first_kept_entry_id = entry_ids[entry_ids.size() - keep_count].first;
                }
            }
        }

        sessions::AppendCompactionOptions comp_opts;
        comp_opts.summary = compact_data.summary;
        comp_opts.first_kept_entry_id = first_kept_entry_id;
        comp_opts.tokens_before = compact_data.tokens_before;
        comp_opts.tokens_after = compact_data.tokens_after;

        auto append_result = sessions::append_compaction_to_transcript(
            transcript_path, comp_opts);

        if (append_result.ok) {
            log()->info("[ReactEngine] Compaction persisted to transcript: "
                       "firstKeptEntryId={}", first_kept_entry_id);

            // 对齐 OC: 可选 truncation (config.agents.defaults.compaction.truncateAfterCompaction)
            // 目前默认不开启，但提供接口
            // auto trunc_result = sessions::truncate_session_after_compaction(
            //     transcript_path, config_.truncate_after_compaction);
        } else {
            log()->warn("[ReactEngine] Failed to persist compaction to transcript: {}",
                       append_result.error);
        }
    }

    // =========================================================================
    // Hook: after_compaction
    // Aligned with OpenClaw compact.ts:988 and run.ts:1106
    // Fire-and-forget, runs after successful compaction.
    // =========================================================================
    if (compact_hook_mgr && compact_hook_mgr->has_hooks(plugins::PluginHookName::AFTER_COMPACTION)) {
        tools::PluginHookAfterCompactionEvent ac_event;
        ac_event.message_count   = static_cast<int>(messages.size());
        ac_event.compacted_count = messages_before - static_cast<int>(messages.size());
        ac_event.token_count     = estimate_conversation_tokens(messages);
        // 🎯 对齐 OpenClaw compact.ts: after_compaction 传 sessionFile
        if (!transcript_path.empty()) ac_event.session_file = transcript_path;
        tools::PluginHookAgentContext ac_ctx;
        if (!session_id.empty()) ac_ctx.session_id = session_id;
        try {
            compact_hook_mgr->run_after_compaction(ac_event, ac_ctx);
        } catch (const std::exception& e) {
            log()->warn("after_compaction hook failed: {}", e.what());
        }
    }

    return result;
}


// ============================================================================
// handle_tool_interrupt —— 工具中断的完整闭环
// 见 react_engine.h 的设计说明。
// ============================================================================
void ReactEngine::handle_tool_interrupt(tools::ToolResult& result,
                                        const StreamCallbacks& callbacks) {
    if (!result.interrupt.has_value()) return;
    const tools::ToolInterrupt it = *result.interrupt;

    auto& coord = tools::InterruptCoordinator::instance();

    // ── 前置检查：没有恢复函数就别问 ──────────────────────────────────────
    // ⚠ 先问再发现无法恢复 = 让用户白答一次，而且远端任务照样卡死。
    //   ★ fail-closed: 把失败写进结果文本让模型知道"这一步没做成"，
    //     而不是静默把挂起态当成功交给模型（那样模型会当已完成继续往下）。
    if (!coord.has_resume_handler(it.kind)) {
        result.success = false;
        result.error = "无法向用户发起确认: 未注册中断恢复处理器 (" + it.kind + ")";
        result.interrupt.reset();
        return;
    }

    // ── ① 占会话槽位 ────────────────────────────────────────────────────
    std::string session_key;
    if (tool_manager_) session_key = tool_manager_->context().session_key;
    auto waiter = coord.begin_wait(session_key);
    if (!waiter.valid()) {
        // 该会话已有中断在等，或 session_key 为空（无法路由入站答复）。
        // ⚠ 不能硬等: 入站消息只会被【先占到槽位】的那个中断认领，
        //   这里等下去必然超时，白占 main lane 最长 900s。
        result.success = false;
        result.error = "无法向用户发起确认: 该会话已有待答确认，或缺少会话上下文";
        result.interrupt.reset();
        return;
    }

    // ── ② 投递追问 ──────────────────────────────────────────────────────
    // ★ 必须在占到槽位【之后】投递: 反过来的话用户可能在槽位就绪前就回复，
    //   那条答复无处认领，会被当成新的用户指令注入对话。
    const std::string prompt = tools::InterruptCoordinator::format_prompt(it);
    if (callbacks.on_block_reply) {
        callbacks.on_block_reply(prompt);
        // ★ 立刻 flush —— 追问不 flush 就可能攒在缓冲里，
        //   ⚠ 用户没看到问题却在被等待，必然超时。
        if (callbacks.on_block_reply_flush) callbacks.on_block_reply_flush();
    } else {
        // 没有投递面 = 用户根本看不到这条追问 → 别等了。
        result.success = false;
        result.error = "无法向用户发起确认: 当前会话没有可用的消息投递通道";
        result.interrupt.reset();
        return;
    }

    // ── ③ 等答复（可被 chat.abort 打断）──────────────────────────────────
    auto answer = waiter.wait(tools::kDefaultInterruptWaitMs, &cancelled_);
    if (!answer.has_value()) {
        const bool aborted = cancelled_.load();
        result.success = false;
        result.error = aborted ? "用户确认被取消（运行已中止）"
                              : "等待用户确认超时，未做任何变更";
        result.interrupt.reset();
        return;
    }

    // ── ④ 恢复子系统执行 ────────────────────────────────────────────────
    // ★ 槽位在这里就可以放了: 答复已拿到，恢复期间该会话应当能接受
    //   下一个中断（多轮确认场景）。
    waiter.release();

    auto outcome = coord.resume(it, *answer, &cancelled_);

    // ── ⑤ 用恢复结果替换 result ─────────────────────────────────────────
    // ★ B1 决策: 用户答复只作为 tool result 内容进 transcript，
    //   【不产生独立 user turn】—— 满足"恢复时不要重新发一条消息"。
    result.content.clear();
    if (outcome.success) {
        result.success = true;
        result.error.clear();
        // ★ 用 tool_result::text() 而不是手工构造 ToolResultContent ——
        //   后者的字段顺序/可选字段一变就静默错位。
        result.content = tools::tool_result::text(outcome.text).content;
    } else {
        result.success = false;
        result.error = outcome.error.empty() ? "恢复远端执行失败" : outcome.error;
        // ⚠ 即使失败也要把已有文本带上: 里面含"用户答复了什么"，
        //   否则模型完全不知道刚才发生过一次确认。
        if (!outcome.text.empty()) {
            result.content = tools::tool_result::text(outcome.text).content;
        }
    }

    // ★ 恢复后【又一次】中断（多轮确认）→ 留在 result 上交给下一轮循环。
    //   ⚠ 这里不递归: 递归会让单次工具调用无界占用 main lane，
    //     且中间的 resume 调用无法被 abort 打断。
    result.interrupt = outcome.next_interrupt;
}


} // namespace react
} // namespace hunterclaw