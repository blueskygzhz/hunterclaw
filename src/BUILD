# ============================================================================
# HunterClaw Source BUILD File
# ============================================================================

package(default_visibility = ["//visibility:public"])

# ============================================================================
# Memory Library - 严格对齐 OpenClaw Memory 实现
# ============================================================================
cc_library(
    name = "hunterclaw_memory",
    srcs = glob(
        ["memory/*.cc"],
        exclude = [
            "memory/plugin_manager.cc",  # 需要更多适配工作
            "memory/sqlite_plugin.cc",   # 需要更多适配工作
        ],
    ),
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
# Core Library
# ============================================================================
cc_library(
    name = "hunterclaw_core",
    srcs = glob(
        [
            "core/*.cc",
            "agents/*.cc",
            "llm/*.cc",
            "channels/*.cc",
            "gateway/*.cc",
            "gateway/methods/*.cc",
            "sessions/*.cc",
            "queue/*.cc",
            "routing/*.cc",
            "cron/*.cc",
            "auto_reply/*.cc",
            "config/*.cc",
            "memory/*.cc",
        ],
        exclude = [
            "main.cc",
            # Exclude files with missing dependencies
            "plugins/**",
            "skills/**",
            "tools/**",
            "mcp/**",
            "react/**",
            "sandbox/**",
            # Exclude daemon implementations (platform-specific)
            "daemon/*.cc",
        ],
    ),
    deps = [
        "//:hunterclaw_headers",
        "//third_party:subprocess",
        "//third_party:httplib",
        "@nlohmann_json//:json",
        "@yaml_cpp//:yaml-cpp",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    copts = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-I/usr/include/jsoncpp",  # For <json/json.h>
    ],
    linkopts = [
        "-lpthread",
        "-ldl",
        "-luuid",
        "-ljsoncpp",
    ] + select({
        "@platforms//os:linux": [
            "-lssl",
            "-lcrypto",
            "-lcurl",
        ],
        "//conditions:default": [],
    }),
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
