#include "hunterclaw/core/types.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/client.h"
#include "hunterclaw/plugins/plugin.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/channels/manager.h"
#include "hunterclaw/agents/agent_instance.h"       // ✅ set_model_context_discovery_hook
#include "hunterclaw/llm/model_discovery.h"          // ✅ discover_and_apply_provider_context_windows
#include "hunterclaw/infra/http_client.h"            // ✅ HttpClient
#include "hunterclaw/agents/types.h"
#include "hunterclaw/queue/delivery_queue.h"
#include "hunterclaw/outbound/deliver.h"
#include "hunterclaw/acp/acp_server.h"
#include "hunterclaw/acp/acp_translator.h"
#include "hunterclaw/cli/mcp_cli.h"  // 🎯 mcp login/logout 子命令
// #include "hunterclaw/cli/command.h"  // Temporarily disabled
// #include "hunterclaw/cli/daemon_commands.h"  // Temporarily disabled
#include <iostream>
#include <csignal>
#include <atomic>
#include <cstdlib>
#include <cstring>

using namespace hunterclaw;

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

// ============================================================================
// 🎯 ACP stdio server 模式入口（对齐 OpenClaw serveAcpGateway）
//   用法: hunterclaw acp [--gateway ws://host:port] [--token X] [--verbose]
//   作为 gateway client，通过 stdin/stdout 的 ndjson JSON-RPC 提供 ACP Agent 服务。
// ============================================================================
static int run_acp_stdio(int argc, char** argv) {
    acp::AcpServerOptions opts;
  std::string gw_url = "ws://127.0.0.1:18789";
    std::string token;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--gateway" && i + 1 < argc) gw_url = argv[++i];
        else if (a == "--token" && i + 1 < argc) token = argv[++i];
   else if (a == "--verbose" || a == "-v") opts.verbose = true;
    }

    // 🎯 gateway 事件回流转发器：serve_acp_stdio 内部创建 agent 后写入，
    //   gateway on_event 通过它把 chat 流 / approval 事件回流到 translator。
    auto event_forwarder = std::make_shared<std::function<void(const nlohmann::json&)>>();

    // 连接 gateway（作为 client）。所有诊断写 stderr，stdout 专供 ndjson。
    gateway::GatewayClientOptions gopts;
    gopts.url = gw_url;
    gopts.token = token;
    gopts.client_name = "hunterclaw-acp";
    gopts.mode = "interactive";
    // 🎯 on_event: 把 gateway 事件回流到 ACP translator（驱动 session/update
    //   与 session/request_permission 权限中继）。
    gopts.on_event = [event_forwarder](const gateway::EventFrame& ev) {
        if (event_forwarder && *event_forwarder) {
   nlohmann::json j = ev.to_json();
     try { (*event_forwarder)(j); } catch (...) {}
   }
    };

    auto gw = std::make_shared<gateway::GatewayClient>(gopts);

    // ── prompt 派发：通过 gateway chat.send 发送并等待最终结果 ──────────────────
    acp::AcpStdioServerDeps deps;
    deps.prompt_dispatch = [gw](const std::string& session_key,
      const std::string& prompt_text,
      const nlohmann::json& attachments,
    const std::string& idempotency_key) -> acp::ChatResult {
 acp::ChatResult result;
        nlohmann::json params{
{"sessionKey", session_key},
          {"text", prompt_text},
    {"attachments", attachments},
        };
        if (!idempotency_key.empty()) params["idempotencyKey"] = idempotency_key;
        try {
    auto rr = gw->request("chat.send", params);
  if (rr.ok && rr.value.has_value() && rr.value->is_object()) {
     const auto& resp = *rr.value;
   result.run_id = resp.value("runId", resp.value("run_id", std::string{}));
      result.stop_reason = resp.value("stopReason", resp.value("stop_reason", std::string{}));
      result.text = resp.value("text", std::string{});
            }
   } catch (const std::exception& e) {
   std::cerr << "[acp] chat.send failed: " << e.what() << "\n";
            result.stop_reason = "end_turn";
        }
        return result;
    };

    // ── cancel 派发：abort 指定 run ────────────────────────────────────────────
    deps.cancel_dispatch = [gw](const std::string& session_key, const std::string& run_id) {
   nlohmann::json params{{"sessionKey", session_key}};
        if (!run_id.empty()) params["runId"] = run_id;
    try { gw->request("chat.abort", params); } catch (...) {}
    };

  // ── approval 详情查询：对齐 OC exec.approval.get ─────────────────────────────
    deps.approval_details = [gw](const std::string& approval_id) -> nlohmann::json {
 try {
   auto rr = gw->request("exec.approval.get", nlohmann::json{{"id", approval_id}});
     if (rr.ok && rr.value.has_value() && rr.value->is_object()) return *rr.value;
        } catch (...) {}
        return nlohmann::json::object();
    };

    // ── approval 解决：对齐 OC exec.approval.resolve ─────────────────────────────
    deps.approval_resolve = [gw](const std::string& approval_id,
     const std::string& decision) -> bool {
        try {
     auto rr = gw->request("exec.approval.resolve",
   nlohmann::json{{"id", approval_id}, {"decision", decision}});
   return rr.ok;
   } catch (...) { return false; }
  };

    // 🎯 gateway 事件回流出参：serve_acp_stdio 内部创建 agent 后写入 forwarder，
    //   gopts.on_event 通过 event_forwarder 把事件回流到 translator。
    deps.event_forwarder = event_forwarder;

  if (opts.verbose) std::cerr << "[acp] connecting gateway " << gw_url << "\n";
  gw->start();

    // 阻塞式运行 ACP stdio server（stdin EOF 退出）。
    acp::serve_acp_stdio(opts, deps);

    gw->stop();
    return 0;
}


void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

void print_banner() {
    std::cout << R"(
   _   _             _            ____ _               
  | | | |_   _ _ __ | |_ ___ _ __/ ___| | __ _ __      __
  | |_| | | | | '_ \| __/ _ \ '__| |   | |/ _` \ \ /\ / /
  |  _  | |_| | | | | ||  __/ |  | |___| | (_| |\ V  V / 
  |_| |_|\__,_|_| |_|\__\___|_|   \____|_|\__,_| \_/\_/  
                                                          
  High-Performance AI Agent Platform (C++)
  Compatible with OpenClaw
  Version: 0.1.0
    )" << std::endl;
}

void print_help() {
    std::cout << "Usage: hunterclaw [command] [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  daemon install     Install HunterClaw as a system daemon" << std::endl;
    std::cout << "  daemon uninstall   Uninstall HunterClaw daemon" << std::endl;
    std::cout << "  daemon start       Start HunterClaw daemon" << std::endl;
    std::cout << "  daemon stop        Stop HunterClaw daemon" << std::endl;
    std::cout << "  daemon restart     Restart HunterClaw daemon" << std::endl;
    std::cout << "  daemon status      Show daemon status" << std::endl;
    std::cout << "  daemon logs        View daemon logs" << std::endl;
    std::cout << std::endl;
    std::cout << "Options (when running directly):" << std::endl;
    std::cout << "  --config <path>    Configuration file path (default: ~/.hunterclaw/config.json)" << std::endl;
    std::cout << "  --port <port>      Gateway port (default: 18789)" << std::endl;
    std::cout << "  --daemon           Run in daemon mode (used by daemon service)" << std::endl;
    std::cout << "  --help             Show this help message" << std::endl;
    std::cout << "  --version          Show version information" << std::endl;
    std::cout << std::endl;
    std::cout << "Run 'hunterclaw daemon <command> --help' for more information on a command." << std::endl;
}

int main(int argc, char** argv) {
    // 🎯 ACP stdio server 子命令（对齐 OpenClaw serveAcpGateway）
    if (argc >= 2 && std::strcmp(argv[1], "acp") == 0) {
   return run_acp_stdio(argc, argv);
    }

    // 🎯 MCP OAuth 子命令（对齐 OpenClaw mcp login/logout）
    if (argc >= 2 && std::strcmp(argv[1], "mcp") == 0) {
        return cli::dispatch_mcp_cli(argc, argv);
    }

    // Temporarily disabled CLI commands
    /*
    // Register daemon CLI commands
    cli::register_daemon_commands();
    
    // Check for daemon subcommands first
    if (argc >= 2 && std::string(argv[1]) == "daemon") {
        if (argc >= 3) {
            std::string subcommand = argv[2];
            
            // Build subcommand args
            std::vector<std::string> sub_args;
            for (int i = 3; i < argc; i++) {
                sub_args.push_back(argv[i]);
            }
            
            // Execute daemon subcommand
            auto& registry = cli::CommandRegistry::instance();
            auto result = registry.execute(subcommand, sub_args);
            
            if (result.is_err()) {
                std::cerr << "\033[31mError:\033[0m " << result.error << std::endl;
                return 1;
            }
            
            return 0;
        } else {
            std::cerr << "\033[31mError:\033[0m Missing daemon subcommand" << std::endl;
            std::cerr << "Run 'hunterclaw daemon --help' for available commands" << std::endl;
            return 1;
        }
    }
    */
    
    // Parse command line arguments for direct execution
    std::string config_path = core::utils::expand_path("~/.hunterclaw/config.json");
    uint16_t port = 18789;
    bool daemon_mode = false;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        }
        
        if (arg == "--version" || arg == "-v") {
            std::cout << "HunterClaw version 0.1.0" << std::endl;
            return 0;
        }
        
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
        
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        }
        
        if (arg == "--daemon") {
            daemon_mode = true;
        }
    }
    
    if (daemon_mode) {
        // Daemon mode: suppress banner, use structured logging
        std::cout << "[HunterClaw] Starting in daemon mode" << std::endl;
    } else {
        print_banner();
    }
    
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Load configuration
    core::Config config;
    if (core::utils::file_exists(config_path)) {
        std::cout << "Loading configuration from: " << config_path << std::endl;
        auto config_result = core::load_config(config_path);
        if (config_result.is_err()) {
            std::cerr << "Failed to load config: " << config_result.error << std::endl;
            std::cout << "Using default configuration" << std::endl;
            config = core::create_default_config();
        } else {
            config = config_result.get();
        }
    } else {
        std::cout << "Configuration file not found, using defaults" << std::endl;
        config = core::create_default_config();
        
        // Create config directory
        std::string config_dir = core::utils::expand_path("~/.hunterclaw");
        core::utils::mkdir_p(config_dir);
        
        // Save default config
        auto save_result = core::save_config(config_path, config);
        if (save_result.is_ok()) {
            std::cout << "Default configuration saved to: " << config_path << std::endl;
        }
    }
    
    // Override port if specified
    if (port != 18789) {
        config.gateway.port = port;
    }
    
    std::cout << "\n=== HunterClaw Initialization ===" << std::endl;

    // 🎯 注册 Live Model Catalog 发现钩子 (对齐 openclaw catalog-build-time
    // discovery)。agent_instance 创建 LLMClient 时会按 provider 从其 /models
    // 端点动态拉取上下文窗口并灌入 ContextTokenCache; 此处在顶层二进制注册真实
    // 实现 (core 层), 规避 agents→core 循环依赖。
    agents::set_model_context_discovery_hook(
        [](const std::string& provider_id,
           const std::string& base_url,
           const std::string& api_key) {
            auto http = std::make_shared<infra::HttpClient>();
            llm::discover_and_apply_provider_context_windows(
                http, provider_id, base_url, api_key);
        });

    // Initialize Plugin Manager
    std::cout << "[1/4] Initializing Plugin Manager..." << std::endl;
    auto plugin_mgr = plugins::PluginManager::create();
    
    // Load plugins from ~/.hunterclaw/plugins
    std::string plugins_dir = core::utils::expand_path("~/.hunterclaw/plugins");
    if (core::utils::file_exists(plugins_dir)) {
        // 🎯 对齐 OpenClaw: 扫描插件目录并加载
        std::cout << "  Loading plugins from: " << plugins_dir << std::endl;
        try {
            for (const auto& entry : std::filesystem::directory_iterator(plugins_dir)) {
                if (entry.is_directory()) {
                    std::cout << "    Found plugin: " << entry.path().filename().string() << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cout << "  Warning: Plugin scan failed: " << e.what() << std::endl;
        }
    }
    
    // Initialize Skill Manager
    std::cout << "[2/4] Initializing Skill Manager..." << std::endl;
    auto skill_mgr = skills::SkillManager::create(config.skills);
    
    auto load_result = skill_mgr->load_from_roots();
    if (load_result.is_ok()) {
        auto skills_list = skill_mgr->list();
        std::cout << "  Loaded " << skills_list.size() << " skills" << std::endl;
    } else {
        std::cout << "  Warning: Failed to load skills: " << load_result.error << std::endl;
    }
    
    // Initialize Channel Manager
    std::cout << "[3/4] Initializing Channel Manager..." << std::endl;
    auto channel_mgr = channels::ChannelManager::create();

    // ── Delivery Queue + Crash Recovery ─────────────────────────────────────
    // Aligned with openclaw: recoverPendingDeliveries called at startup.
    // Scans the write-ahead queue for deliveries that were enqueued but not
    // acked before the last crash, and retries them (skipQueue=true to avoid
    // re-enqueueing).
    std::string queue_dir = core::utils::expand_path("~/.hunterclaw/delivery-queue");
    auto delivery_queue = queue::DeliveryQueue::create(queue_dir);

    std::cout << "  Recovering pending deliveries from: " << queue_dir << std::endl;
    auto recovery_result = delivery_queue->recover(
        [&delivery_queue](const queue::QueuedDelivery& queued) -> core::Result<bool> {
            // Aligned with openclaw: skipQueue=true to prevent re-enqueueing
            outbound::DeliverOutboundPayloadsParams params;
            params.channel = queued.target.channel;
            params.to = queued.target.to;
            params.account_id = queued.target.account_id;
            params.thread_id = queued.target.thread_id;
            params.reply_to_id = queued.target.reply_to_id;
            params.payloads = queued.payloads;
            // Aligned with openclaw: restore all delivery options from queued entry
            params.best_effort = queued.best_effort;
            params.silent = queued.silent;
            params.gif_playback = queued.target.gif_playback;
            params.skip_queue = true;  // Crash recovery: don't re-enqueue
            params.delivery_queue = delivery_queue;

            auto r = outbound::deliver_outbound_payloads(params);
            if (r.is_ok()) {
                return core::Result<bool>::Ok(true);
            }
            return core::Result<bool>::Err(r.error);
        }
    );
    if (recovery_result.is_ok()) {
        const auto& summary = recovery_result.get();
        if (summary.recovered > 0 || summary.failed > 0) {
            std::cout << "  Recovery: recovered=" << summary.recovered
                      << " failed=" << summary.failed
                      << " skipped_max_retries=" << summary.skipped_max_retries
                      << " deferred_backoff=" << summary.deferred_backoff << std::endl;
        } else {
            std::cout << "  No pending deliveries to recover" << std::endl;
        }
    } else {
        std::cout << "  Warning: Recovery scan failed: " << recovery_result.error << std::endl;
    }
    
    // 🎯 对齐 OpenClaw: channel 消息回调由 Gateway inbound worker 处理
    // channel_mgr 的消息路由在 Gateway 启动时通过 inbound plugin hooks 注册
    
    // Initialize Gateway Server
    std::cout << "[4/4] Starting Gateway Server..." << std::endl;
    auto gateway = gateway::GatewayServer::create(config.gateway);
    
    auto start_result = gateway->start();
    if (start_result.is_err()) {
        std::cerr << "Failed to start gateway: " << start_result.error << std::endl;
        return 1;
    }
    
    std::cout << "\n==================================" << std::endl;
    std::cout << "🚀 HunterClaw is running!" << std::endl;
    std::cout << "Gateway: ws://" << config.gateway.host << ":" << config.gateway.port << std::endl;
    std::cout << "Config: " << config_path << std::endl;
    std::cout << "Skills: " << skill_mgr->list().size() << " loaded" << std::endl;
    // std::cout << "Agents: " << agent_mgr->list_agents().size() << " loaded" << std::endl;
    std::cout << "==================================" << std::endl;
    std::cout << "\nPress Ctrl+C to stop" << std::endl;
    
    // Main loop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // Graceful shutdown
    std::cout << "\n=== Shutting down ===" << std::endl;
    
    std::cout << "Stopping Gateway..." << std::endl;
    gateway->stop();
    
    std::cout << "Stopping Channels..." << std::endl;
    auto channels_list = channel_mgr->list_channels();
    for (const auto& channel_id : channels_list) {
        channel_mgr->stop_channel(channel_id, "default");
    }
    
    std::cout << "Unloading Plugins..." << std::endl;
    auto plugins_list = plugin_mgr->list();
    for (const auto& plugin_id : plugins_list) {
        plugin_mgr->unload(plugin_id);
    }
    
    std::cout << "Goodbye! 👋" << std::endl;
    
    return 0;
}
