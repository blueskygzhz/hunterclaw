# ============================================================================
# HunterClaw Source BUILD File
# ============================================================================

package(default_visibility = ["//visibility:public"])

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
