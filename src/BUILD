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
        "-lcurl",
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
        ":hunterclaw_llm",
        ":hunterclaw_memory",
        ":hunterclaw_sessions",
        ":hunterclaw_observability",  # ✅ 添加 observability 依赖
        ":hunterclaw_queue",          # ✅ 添加 queue 依赖 (CommandQueue)
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
    srcs = glob(["sandbox/*.cc"]),
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
    srcs = glob(["core/*.cc"]),
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
# ReAct Library - ReAct Engine, Reasoning, Tool Orchestration (对齐 OpenClaw)
# ============================================================================
cc_library(
    name = "hunterclaw_react",
    srcs = glob(["react/*.cc"]),
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
# Agents Library - Agent Pool, Instance, Scope, Subagent Management
# ============================================================================
cc_library(
    name = "hunterclaw_agents",
    srcs = glob(["agents/*.cc"]),
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_memory",
        ":hunterclaw_llm",
        ":hunterclaw_skills",
        ":hunterclaw_tools",   # ✅ 添加 tools 依赖 (ToolManager::create)
        ":hunterclaw_react",   # ✅ 添加 react 依赖 (ReactEngine::create)
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
        "hooks/*.cc",
        "routing/*.cc",
        "pairing/*.cc",
        "security/*.cc",
        "secrets/*.cc",
        "auto_reply/*.cc",
        # ✅ 移除 observability/*.cc - 已移至独立的 hunterclaw_observability 库
        # ✅ 移除 infra/*.cc - 已移至独立的 hunterclaw_infra 库
        "markdown/*.cc",
        "shared/*.cc",
        "nodes/*.cc",
    ], exclude = [
        "main.cc",
    ]),
    deps = [
        ":hunterclaw_core_base",  # ✅ 依赖 core_base (core/*.cc)
        ":hunterclaw_logging",   # ✅ 添加 logging 依赖 (create_subsystem_logger)
        ":hunterclaw_agents",    # ✅ 添加 agents 依赖
        ":hunterclaw_cron",      # ✅ 添加 cron 依赖
        ":hunterclaw_infra",     # ✅ 添加 infra 依赖 (heartbeat_runner 等)
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
# Channel Plugins
# ============================================================================
# All channel plugins (discord, telegram, slack, feishu, google_chat) are
# compiled as part of hunterclaw_core via glob(["channels/*.cc"]).
#
# Each src/channels/xxx_channel.cc contains:
#   1. make_xxx_plugin() — ChannelPlugin descriptor implementation
#   2. Static registrar  — registers into ChannelPluginRegistry at startup
#
# No separate plugin registration files needed (plugins/channels/ removed).
# This aligns with OpenClaw: src/channels/plugins/ is the canonical location.
