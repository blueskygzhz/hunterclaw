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
# Cron Library - Cron Job Scheduling and Management
# ============================================================================
cc_library(
    name = "hunterclaw_cron",
    srcs = glob(["cron/*.cc"]),
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
        "-lcurl",      # HTTP delivery
        "-lssl",       # OpenSSL SHA256
        "-lcrypto",    # OpenSSL Crypto
        "-ljsoncpp",
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
        # ✅ spdlog 是 header-only，不需要链接
        # ✅ fmt 也是 header-only 或已包含在 spdlog 中
    ],
    linkstatic = False,
    visibility = ["//visibility:public"],
)

# ============================================================================
# MCP Library - MCP Tool Integration
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
# ReAct Library - ReAct Engine, Reasoning, Tool Orchestration (对齐 OpenClaw)
# ============================================================================
cc_library(
    name = "hunterclaw_react",
    srcs = glob(["react/*.cc"]),  # ✅ 不再排除 message_processor.cc - 所有依赖已实现
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_llm",
        ":hunterclaw_tools",
        ":hunterclaw_memory",
        ":hunterclaw_sessions",
        ":hunterclaw_agents",  # ✅ 添加 agents 依赖 (message_processor 需要 AgentConfig)
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
        "-lssl",       # ✅ message_processor 需要 OpenSSL (SHA256 用于 echo 检测)
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
# Core Aggregate Library - All HunterClaw modules
# ============================================================================
cc_library(
    name = "hunterclaw_core",
    srcs = glob([
        "core/*.cc",
        "utils/*.cc",
        "config/*.cc",
        "channels/*.cc",
        "context/*.cc",
        "plugins/*.cc",
        "process/*.cc",
        "hooks/*.cc",
        "routing/*.cc",
        "queue/*.cc",
        "pairing/*.cc",
        "security/*.cc",
        "secrets/*.cc",
        "auto_reply/*.cc",
        "observability/*.cc",
        "infra/*.cc",
        "logging/*.cc",
        "markdown/*.cc",
        "shared/*.cc",
        "nodes/*.cc",
    ], exclude = [
        "main.cc",
        "infra/exec_approvals.cc",  # TODO: 需要更新以匹配新的 API
    ]),
    deps = [
        ":hunterclaw_agents",    # ✅ 添加 agents 依赖
        ":hunterclaw_cron",      # ✅ 添加 cron 依赖
        ":hunterclaw_memory",
        ":hunterclaw_llm",
        ":hunterclaw_mcp",
        ":hunterclaw_sessions",
        ":hunterclaw_skills",  # ✅ 修正拼写错误
        ":hunterclaw_tools",  # ✅ 添加 tools 依赖
        ":hunterclaw_sandbox",  # ✅ 添加 sandbox 依赖
        ":hunterclaw_react",  # ✅ 添加 react 依赖
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
# Plugin Libraries (commented out until implementation is complete)
# ============================================================================

# Feishu channel plugin
# cc_library(
#     name = "hunterclaw_channel_feishu",
#     srcs = [
#         "channels/feishu_channel.cc",
#         "channels/feishu_stub.cc",
#     ],
#     deps = [
#         ":hunterclaw_core",
#     ],
#     linkopts = ["-lcurl"],
#     linkstatic = False,
# )

# Telegram channel plugin
# cc_library(
#     name = "hunterclaw_channel_telegram",
#     srcs = ["//plugins/channels/telegram:telegram_channel.cc"],
#     deps = [":hunterclaw_core"],
#     linkstatic = False,
# )
