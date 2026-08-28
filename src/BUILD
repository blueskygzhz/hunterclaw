# ============================================================================
# HunterClaw Source BUILD File
# ============================================================================

package(default_visibility = ["//visibility:public"])

# ============================================================================
# Device Library - Device Authentication and Management (使用系统 OpenSSL + RSA)
# ============================================================================
cc_library(
    name = "hunterclaw_device",
    srcs = glob(["device/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-lssl",       # System OpenSSL (使用 RSA，兼容 OpenSSL 1.0.2)
        "-lcrypto",
        "-ljsoncpp",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Infra Library - Infrastructure Utilities (heartbeat, http_client, events, etc.)
# ============================================================================
cc_library(
    name = "hunterclaw_infra",
    srcs = glob(["infra/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    defines = [
        # ★ 启用 HttpClient 的 HTTPS 支持。
        #
        # ⚠ 此前【只链了 -lssl -lcrypto 却没定义这个宏】，导致 http_client.cc
        #   里 15 处 #ifdef HUNTERCLAW_ENABLE_SSL 全走 else 分支，任何 https://
        #   请求都返回 "HTTPS not supported"。而 HttpClient 是唯一 HTTP 通路
        #   （无 curl 后备），影响所有出网功能：MCP HTTP transport / web_search
        #   / LLM provider / webhook / remote_a2a。
        #
        # ⚠ 打开后暴露出两个「从未被编译过」的缺陷，均已修：
        #   ① http_client.cc  socket_result.value 是 std::optional<int>，
        #      赋给 int 编译失败（应为 .get()）
        #   ② 代码按 OpenSSL 1.1.0+ 编写，而 RHEL/CentOS 7 系是 1.0.2k。
        #      已加 OPENSSL_VERSION_NUMBER 兼容层（见 http_client.cc 顶部），
        #      TLS 版本下限仍是 1.2，安全性不降级。
        #
        # ★ 用 defines 而非 copts：defines 会传递给所有依赖方，保证
        #   include http_client.h 的每个编译单元看到一致的 SSL 声明。
        #   只写 copts 会让头文件里的 #ifdef 在依赖方走 else 分支，
        #   造成 ODR 违规（同一个类在不同 TU 里成员布局不同）。
        "HUNTERCLAW_ENABLE_SSL",
    ],
    linkopts = [
        "-lpthread",
        "-lssl",
        "-lcrypto",
        "-lcurl",
        "-ljsoncpp",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Cron Library - Cron Job Scheduling and Management
# ============================================================================
cc_library(
    name = "hunterclaw_cron",
    srcs = glob(["cron/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_infra",          # ✅ 添加 infra 依赖 (heartbeat_runner 等)
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-lcurl",      # HTTP delivery
        "-lssl",       # OpenSSL SHA256
        "-lcrypto",    # OpenSSL Crypto
        "-ljsoncpp",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Observability Library - Tracing, Metrics, Diagnostic Events
# ============================================================================
cc_library(
    name = "hunterclaw_observability",
    srcs = glob(["observability/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-ljsoncpp",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Media Library - Media Store, Parse, MIME, Path Policy (对齐 OpenClaw media/)
# ============================================================================
cc_library(
    name = "hunterclaw_media",
    srcs = glob(["media/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        "@nlohmann_json//:json",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-luuid",
        "-lstdc++fs",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Memory Library - 严格对齐 OpenClaw Memory 实现
# ============================================================================
# ============================================================================
# Logging Library - Subsystem Logger (对齐 OpenClaw logging/subsystem.ts)
# ============================================================================
cc_library(
    name = "hunterclaw_logging",
    srcs = glob(["logging/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        "@nlohmann_json//:json",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Memory Library - 严格对齐 OpenClaw Memory 实现
# ============================================================================
cc_library(
    name = "hunterclaw_memory",
    srcs = glob(["memory/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_logging",  # ✅ 添加 logging 依赖 (create_subsystem_logger)
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
    ],
    linkopts = [
        "-lpthread",
        "-luuid",
        "-lsqlite3",
        "-ljsoncpp",
        "-lcurl",  # libcurl for HTTP
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# LLM Library - LLM Client and Provider Abstraction
# ============================================================================
cc_library(
    name = "hunterclaw_llm",
    srcs = glob(["llm/*.cc", "llm/providers/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_infra",     # ✅ HttpClient (替代 curl)
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
    ],
    linkopts = [
        "-lpthread",
        "-ljsoncpp",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Queue Library - CommandQueue, CommandLane, EnqueueOptions
# ============================================================================
cc_library(
    name = "hunterclaw_queue",
    srcs = glob(["queue/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        "@nlohmann_json//:json",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Gateway Library - OpenClaw 对齐的 HTTP/WebSocket Gateway
# ============================================================================
cc_library(
    name = "hunterclaw_gateway",
    srcs = glob([
        "gateway/*.cc",
        "gateway/methods/*.cc",
        "gateway/http/*.cc",  # ✅ OpenAI 兼容层
    ]),
    hdrs = glob([
        "gateway/**/*.h",
    ]),
    deps = [
        "//:hunterclaw_headers",
        "//third_party:httplib",
        ":hunterclaw_agents",        # ✅ 添加 agents 依赖
        ":hunterclaw_memory",        # ✅ 添加 memory 依赖
        ":hunterclaw_llm",           # ✅ 添加 llm 依赖
        ":hunterclaw_sessions",      # ✅ 添加 sessions 依赖
        ":hunterclaw_skills",        # ✅ 添加 skills 依赖
        ":hunterclaw_tools",         # ✅ 添加 tools 依赖
        ":hunterclaw_react",         # ✅ 添加 react 依赖
        ":hunterclaw_cron",          # ✅ 添加 cron 依赖
        ":hunterclaw_infra",         # ✅ 添加 infra 依赖 (heartbeat_runner 等)
        ":hunterclaw_observability", # ✅ 添加 observability 依赖 (WebhookReceivedEvent vtable)
        ":hunterclaw_queue",         # ✅ 添加 queue 依赖 (CommandQueue)
        ":hunterclaw_tasks",         # ✅ 添加 tasks 依赖 (TaskRegistry/TaskExecutor)
        "@ixwebsocket//:ixwebsocket",   # ✅ 添加 ixwebsocket 依赖 (WebSocket)
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",  # spdlog 支持
        "-Isrc",  # ✅ 添加 src 目录以支持 gateway/methods 头文件引用
    ],
    linkopts = [
        "-lpthread",
        "-lssl",       # OpenSSL (SHA256)
        "-lcrypto",    # OpenSSL Crypto
        "-lcurl",      # libcurl
        "-ljsoncpp",   # JSON
        "-lz",         # zlib (ixwebsocket)
        # ✅ spdlog 是 header-only，不需要链接
        # ✅ fmt 也是 header-only 或已包含在 spdlog 中
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# MCP Library# MCP Library - MCP Tool Integration
# ============================================================================
cc_library(
    name = "hunterclaw_mcp",
    srcs = glob(["mcp/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_infra",   # ✅ NativeMcpClient: HttpClient (SSE/StreamableHttp)
        ":hunterclaw_secguard",  # ✅ SSRF guard + env sanitize (MCP client安全)
        ":hunterclaw_logging",   # ✅ redact + secret redaction registry (传输日志脱敏)
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-ljsoncpp",
        "-lsqlite3",  # ✅ mcp_oauth_persistence: OAuth 凭证 SQLite 持久化
        "-lssl",# ✅ mcp_oauth_authorization: PKCE(SHA256/RAND)
        "-lcrypto",   # ✅ mcp_oauth_authorization: OpenSSL Crypto
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Sessions Library - Transcript 清理和修复 (对齐 OpenClaw)
# ============================================================================
cc_library(
    name = "hunterclaw_sessions",
    srcs = glob(["sessions/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_llm",
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-ljsoncpp",
        "-lsqlite3",
        "-luuid",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Skill Library - Skill Manager, Plugin System, File Watcher
# ============================================================================
cc_library(
    name = "hunterclaw_skills",
    srcs = glob(["skills/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_tools",  # ✅ skill_changed hook (get_global_hook_manager)
        "@nlohmann_json//:json",
        "@yaml_cpp//:yaml-cpp",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-ljsoncpp",
        "-lsqlite3",
        "-luuid",
        "-ldl",  # For dynamic loading
        "-lcrypto",  # ✅ parser.cc: SHA256 (compute_skill_prompt_version)
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Tools Library - Built-in Tools, MCP Tools, Tool Manager
# ============================================================================
cc_library(
    name = "hunterclaw_tools",
    srcs = glob(["tools/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_infra",              # ✅ 添加 infra 依赖 (HttpClient)
        ":hunterclaw_secguard",           # ✅ dangerous_tools (owner-only gate)
        ":hunterclaw_llm",
        ":hunterclaw_memory",
        ":hunterclaw_sessions",
        ":hunterclaw_observability",  # ✅ 添加 observability 依赖
        ":hunterclaw_queue",          # ✅ 添加 queue 依赖 (CommandQueue)
        ":hunterclaw_mcp",            # ✅ 添加 mcp 依赖 (ToolManager::enable_mcp)
        "@ixwebsocket//:ixwebsocket", # ✅ sessions_tool.cc 需要 gateway/client.h
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-ljsoncpp",
        "-lsqlite3",
        "-luuid",
        "-lcurl",
        "-lssl",
        "-lcrypto",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Sandbox Library - Docker/Process Sandbox, Code Execution (对齐 OpenClaw)
# ============================================================================
cc_library(
    name = "hunterclaw_sandbox",
    srcs = glob(["sandbox/*.cc"], exclude = [
        # ✅ 已移至 hunterclaw_secguard (供 MCP 复用, 避免符号重复)
        "sandbox/sanitize_env_vars.cc",
    ]),
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_secguard",  # ✅ sanitize_env_vars (已移出 sandbox glob)
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-luuid",
        "-ljsoncpp",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Core Base Library - Core utilities (ThreadPool, etc.) with no heavy deps
# ============================================================================
cc_library(
    name = "hunterclaw_core_base",
    srcs = glob(["core/*.cc"]) + [
        "shared/utils.cc",  # ✅ is_private_or_loopback_ip_address 等纯工具函数 (供 secguard 使用)
    ],
    deps = [
        "//:hunterclaw_headers",
        "//third_party:subprocess",  # ✅ subprocess.cc 需要 subprocess.h
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
        "-Ithird_party",  # ✅ 添加 third_party 路径以支持 subprocess.h
    ],
    linkopts = [
        "-lpthread",
        "-ljsoncpp",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# SecGuard Library - SSRF Guard + Env Sanitization (底层安全模块, 供 MCP 等复用)
# ============================================================================
cc_library(
    name = "hunterclaw_secguard",
    srcs = [
        "security/ssrf_guard.cc",
        "security/dangerous_tools.cc",
        "sandbox/sanitize_env_vars.cc",
    ],
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_core_base",  # ✅ shared/utils (is_private_or_loopback_ip_address)
        "@nlohmann_json//:json",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/spdlog",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# ReAct Library - ReAct Engine, Reasoning, Tool Orchestration (对齐 OpenClaw)
# ============================================================================
cc_library(
    name = "hunterclaw_react",
    srcs = glob(["react/*.cc"]) + [
        # ✅ 对齐 OC pi-hooks/context-pruning: ReactEngine 直接依赖 pruner，
        # 放在此处避免与 hunterclaw_core 形成循环依赖。
        "context/context_pruning.cc",
    ],
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_core_base",     # ✅ 添加 core_base 依赖 (ThreadPool)
        ":hunterclaw_llm",
        ":hunterclaw_tools",
        ":hunterclaw_memory",
        ":hunterclaw_sessions",
        ":hunterclaw_observability",  # ✅ 添加 observability 依赖 (Tracer, Span, DiagnosticEventBus)
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-ljsoncpp",
        "-lssl",       # loop_detection 需要 OpenSSL (SHA256)
        "-lcrypto",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Hooks Library - Hook Registry, Hook Events (独立库，无循环依赖)
# ============================================================================
cc_library(
    name = "hunterclaw_hooks",
    srcs = glob(["hooks/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        "@nlohmann_json//:json",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Routing Library - Agent Routing, Session Key Resolution (独立库，无循环依赖)
# ============================================================================
cc_library(
    name = "hunterclaw_routing",
    srcs = glob(["routing/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        "@nlohmann_json//:json",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# ACP Library - ACP Control Plane (对齐 OpenClaw acp/ control-plane/)
# ============================================================================
cc_library(
    name = "hunterclaw_acp",
    srcs = glob(["acp/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        "@ixwebsocket//:ixwebsocket",  # 新增: acp_session_mapper.cc 引用 gateway/client.h
        "@nlohmann_json//:json",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-lssl",       # 新增: acp_secret_file / acp_conversation_id 需要 SHA-256
        "-lcrypto",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Remote A2A Library - 远端 A2A agent 客户端（消费远端，不暴露自身为服务）
#
# ★ 刻意拆成两个 target：
#   hunterclaw_remote_a2a        核心，【不依赖 httplib / gateway】
#   hunterclaw_remote_a2a_route  gateway 胶水，仅 callback_route.cc
#
# 这不只是为了构建整洁，而是把「核心不碰 httplib」这条边界变成【构建约束】：
# 若哪天核心误引 httplib，本 target 会直接编译失败，而不是等到运行期
# 才发现依赖膨胀。tests/run_remote_a2a_tests.sh 里的 LOW/HIGH 分层同理。
#
# ⚠ 与现有 tools/sessions_send_a2a.cc（会话间 A2A）完全独立、零耦合。
# ============================================================================
cc_library(
    name = "hunterclaw_remote_a2a",
    srcs = glob(
        ["remote_a2a/*.cc"],
        exclude = ["remote_a2a/callback_route.cc"],  # ★ 需要 httplib，见下一个 target
    ),
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_infra",     # HttpClient（拉卡 / RPC / SSE）+ system_events（进度投递）
        ":hunterclaw_tools",     # Tool / ToolManager / ToolParams / tool_result
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "hunterclaw_remote_a2a_route",
    srcs = ["remote_a2a/callback_route.cc"],
    deps = [
        "//:hunterclaw_headers",
        "//third_party:httplib",
        ":hunterclaw_remote_a2a",
        ":hunterclaw_gateway",   # HttpGateway::register_route / add_auth_bypass_prefix
        "@nlohmann_json//:json",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Tasks Library - Background Task Registry (对齐 OpenClaw tasks/)
# ============================================================================
cc_library(
    name = "hunterclaw_tasks",
    srcs = glob(["tasks/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_core_base",
        "@nlohmann_json//:json",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-lsqlite3",  # ✅ SQLite persistence (task/flow stores)
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Agents Library - Agent Pool, Instance, Scope, Subagent Management
# ============================================================================
cc_library(
    name = "hunterclaw_agents",
    srcs = glob(["agents/*.cc"]) + glob(["auto_reply/*.cc"]),  # ✅ auto_reply 合并入 agents，避免循环依赖
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_memory",
        ":hunterclaw_llm",
        ":hunterclaw_sessions",  # ✅ SessionManager::create / SessionEntry (agent_instance.cc)
        ":hunterclaw_skills",
        ":hunterclaw_tools",   # ✅ 添加 tools 依赖 (ToolManager::create)
        ":hunterclaw_react",   # ✅ 添加 react 依赖 (ReactEngine::create)
        ":hunterclaw_mcp",     # ✅ 添加 mcp 依赖 (AgentInstance 自动启用 MCP)
        # ✅ remote_a2a: AgentInstance 把进程级 registry 的工具注册进本会话 ToolManager
        #    ⚠ 只依赖核心 target（不含 callback_route）——
        #      后者依赖 gateway，而 gateway 依赖 agents，会成环。
        ":hunterclaw_remote_a2a",
        ":hunterclaw_hooks",   # ✅ 添加 hooks 依赖 (auto_reply/message_dispatcher.cc)
        ":hunterclaw_routing", # ✅ 添加 routing 依赖 (auto_reply/message_dispatcher.cc)
        ":hunterclaw_acp",     # ✅ 添加 acp 依赖 (auto_reply/message_dispatcher.cc)
        "@ixwebsocket//:ixwebsocket",  # ✅ acp_spawn.cc 需要 gateway/client.h
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-ljsoncpp",
        "-luuid",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Outbound Library - deliver_outbound_payloads (对齐 openclaw deliver.ts)
# ============================================================================
cc_library(
    name = "hunterclaw_outbound",
    srcs = glob(["outbound/*.cc"]) + [
        "channels/channel_plugin_registry.cc",  # ChannelPluginRegistry implementation
    ],
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_observability",
        ":hunterclaw_hooks",   # ✅ 添加 hooks 依赖 (deliver.cc)
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-ljsoncpp",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Core Aggregate Library - All HunterClaw modules
# ============================================================================
cc_library(
    name = "hunterclaw_core",
    srcs = glob([
        "utils/*.cc",
        "config/*.cc",
        "channels/*.cc",
        "context/*.cc",
        "plugins/*.cc",
        "process/*.cc",
        # ✅ hooks/*.cc 已移至独立的 hunterclaw_hooks 库
        # ✅ routing/*.cc 已移至独立的 hunterclaw_routing 库
        "pairing/*.cc",
        "security/*.cc",
        "secrets/*.cc",
        # ✅ auto_reply/*.cc 已移至 hunterclaw_agents 的 srcs（避免循环依赖）
        # ✅ 移除 observability/*.cc - 已移至独立的 hunterclaw_observability 库
        # ✅ 移除 infra/*.cc - 已移至独立的 hunterclaw_infra 库
        "markdown/*.cc",
        "shared/*.cc",
        "nodes/*.cc",
        "cli/mcp_cli.cc",  # ✅ mcp login/logout 子命令（依赖 hunterclaw_mcp）
    ], exclude = [
        "main.cc",
        # ✅ context/context_pruning.cc 已挪至 hunterclaw_react 以避免循环依赖
        "context/context_pruning.cc",
        # ✅ auto_reply/*.cc 已全部移至 hunterclaw_agents
        "auto_reply/session_lifecycle_supplement.cc",
        # ✅ 已移至 hunterclaw_secguard (供 MCP 复用, 避免符号重复)
        "security/ssrf_guard.cc",
        "security/dangerous_tools.cc",
        "shared/utils.cc",
    ]),
    deps = [
        ":hunterclaw_core_base",  # ✅ 依赖 core_base (core/*.cc)
        ":hunterclaw_secguard",   # ✅ ssrf_guard + shared/utils (已移出core glob)
        ":hunterclaw_logging",   # ✅ 添加 logging 依赖 (create_subsystem_logger)
        ":hunterclaw_acp",       # ✅ 添加 ACP 控制平面依赖
        ":hunterclaw_agents",    # ✅ 添加 agents 依赖
        ":hunterclaw_hooks",   # ✅ 添加 hooks 依赖
        ":hunterclaw_routing", # ✅ 添加 routing 依赖
        ":hunterclaw_cron",        ":hunterclaw_infra",     # ✅ 添加 infra 依赖 (heartbeat_runner 等)
        ":hunterclaw_gateway",   # ✅ 添加 gateway 依赖
        ":hunterclaw_memory",
        ":hunterclaw_llm",
        ":hunterclaw_mcp",
        ":hunterclaw_sessions",
        ":hunterclaw_skills",  # ✅ 修正拼写错误
        ":hunterclaw_tools",  # ✅ 添加 tools 依赖
        ":hunterclaw_sandbox",  # ✅ 添加 sandbox 依赖
        ":hunterclaw_react",  # ✅ 添加 react 依赖
        ":hunterclaw_observability",  # ✅ 添加 observability 依赖
        ":hunterclaw_outbound",  # ✅ 添加 outbound 依赖 (deliver_outbound_payloads)
        ":hunterclaw_queue",     # ✅ 添加 queue 依赖 (CommandQueue)
        ":hunterclaw_tasks",     # ✅ 添加 tasks 依赖
        "@ixwebsocket//:ixwebsocket",  # ✅ 添加 ixwebsocket 依赖
        "//:hunterclaw_headers",
        "//third_party:httplib",
        "//third_party:subprocess",  # ✅ 添加 subprocess 依赖
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
        "-Ithird_party",  # ✅ 添加 third_party 路径以支持 subprocess.h
    ],
    linkopts = [
        "-lpthread",
        "-luuid",
        "-lsqlite3",
        "-ljsoncpp",
        "-lcurl",
        "-lssl",
        "-lcrypto",
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# Gateway Server Executable
# ============================================================================
cc_binary(
    name = "hunterclaw-gateway",
    srcs = ["main.cc"],
    deps = [
        ":hunterclaw_core",
    ],
    copts = [
        "-std=c++17",
    ],
    linkopts = [
        "-lpthread",
        "-lstdc++",
        "-lstdc++fs",  # C++17 filesystem
        "-lboost_system",  # Boost.System
    ],
    visibility = ["//visibility:public"],
)

# ============================================================================
# CLI Executable - Command Line Interface
# ============================================================================
cc_binary(
    name = "hunterclaw_cli",
    srcs = ["cli_main.cc"],
    deps = [
        "//:hunterclaw_headers",
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-lstdc++",
        "-lstdc++fs",  # C++17 filesystem
    ],
    visibility = ["//visibility:public"],
)

# ============================================================================
# Daemon Executable - Service Management Tool
# ============================================================================
cc_binary(
    name = "hunterclaw_daemon",
    srcs = [
        "daemon_main.cc",
        "daemon/daemon_service.cc",
        "daemon/daemon_system.cc",
        "daemon/systemd_impl.cc",
    ],
    deps = [
        "//:hunterclaw_headers",
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-I/usr/include/jsoncpp",
        "-I/usr/include/spdlog",
    ],
    linkopts = [
        "-lpthread",
        "-lstdc++",
        "-lstdc++fs",  # C++17 filesystem
    ],
    visibility = ["//visibility:public"],
)

# ============================================================================
# Channel Plugins (对齐 OpenClaw extensions/*/index.ts)
# ============================================================================
# All channel plugins (discord, telegram, slack) are compiled as part of
# hunterclaw_core via glob(["channels/*.cc"]).
#
# Architecture (对齐 OpenClaw):
#   Each channel is a bundled plugin that provides a PluginDefinition.
#   During startup, register_bundled_channel_plugins() loads each channel via
#   PluginLoader::load_plugin_definition(), which calls register_func(api).
#   The register_func calls api.register_channel(channel_id) — pushing into
#   PluginRegistry.channels (bookkeeping) and ChannelPluginRegistry (runtime).
#
# OpenClaw equivalent:
#   extensions/telegram/index.ts → { register(api) { api.registerChannel({ plugin }) } }
#   extensions/slack/index.ts    → { register(api) { api.registerChannel({ plugin }) } }
#   extensions/discord/index.ts  → { register(api) { api.registerChannel({ plugin }) } }
#
# Files:
#   src/channels/bundled_channel_plugins.cc — entry point for loading all bundled channels
#   src/channels/telegram_channel.cc — make_telegram_plugin_definition()
#   src/channels/slack_channel.cc    — make_slack_plugin_definition()
#   src/channels/discord_channel.cc  — make_discord_plugin_definition()
