# ============================================================================
# HunterClaw Source BUILD File
# ============================================================================

package(default_visibility = ["//visibility:public"])

# ============================================================================
# Memory Library - 严格对齐 OpenClaw Memory 实现
# ============================================================================
cc_library(
    name = "hunterclaw_memory",
    srcs = [
        "memory/memory_manager.cc",
        "memory/sqlite_storage.cc",
        "memory/search_algorithms.cc",
        "memory/embedding_provider.cc",
        "memory/http_client.cc",
        "memory/memory_index_manager.cc",
        "memory/search_manager_factory.cc",
        "memory/provider_registry.cc",
        "memory/qmd_manager.cc",
        "memory/auto_reindex.cc",
        "memory/chunking.cc",
        "memory/manager_search.cc",
        "memory/session_files.cc",
        "memory/manager_sync_ops.cc",
        "memory/file_watcher_linux.cc",
        "memory/manager_embedding_ops.cc",
        "memory/qmd_query_parser.cc",
        "memory/qmd_process.cc",
        "memory/qmd_scope.cc",
        "memory/backend_config.cc",
        "memory/query_expansion.cc",
        "memory/temporal_decay.cc",
        "memory/multimodal.cc",
        "memory/json_parser.cc",
        "memory/pruning.cc",
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
