# ============================================================================
# HunterClaw Source BUILD File
# ============================================================================

package(default_visibility = ["//visibility:public"])

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
    srcs = glob(
        ["react/*.cc"],
        exclude = ["react/message_processor.cc"]  # MessageProcessor 依赖 sessions，属于更高层
    ),
    deps = [
        "//:hunterclaw_headers",
        ":hunterclaw_llm",
        ":hunterclaw_tools",
        ":hunterclaw_memory",
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
        "core/*.cc",
        "utils/*.cc",
        "config/*.cc",
        "channels/*.cc",
        "agents/*.cc",
        "context/*.cc",
        "plugins/*.cc",
        "process/*.cc",
        "hooks/*.cc",
        "routing/*.cc",
        "queue/*.cc",
        "pairing/*.cc",
        "security/*.cc",
        "secrets/*.cc",
        "device/*.cc",
        "auto_reply/*.cc",
        "daemon/*.cc",
        "observability/*.cc",
        "infra/*.cc",
        "logging/*.cc",
        "markdown/*.cc",
        "shared/*.cc",
        "nodes/*.cc",
        "cli/*.cc",
    ], exclude = ["main.cc"]),
    deps = [
        ":hunterclaw_cron",      # ✅ 添加 cron 依赖
        ":hunterclaw_memory",
        ":hunterclaw_llm",
        ":hunterclaw_gateway",
        ":hunterclaw_mcp",
        ":hunterclaw_sessions",
        ":hunterclaw_skills",  # ✅ 修正拼写错误
        ":hunterclaw_tools",  # ✅ 添加 tools 依赖
        ":hunterclaw_sandbox",  # ✅ 添加 sandbox 依赖
        ":hunterclaw_react",  # ✅ 添加 react 依赖
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
        "-I/usr/include/spdlog",
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
