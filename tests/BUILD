# ============================================================================
# HunterClaw Tests BUILD
# ============================================================================

package(default_visibility = ["//visibility:public"])

# ----------------------------------------------------------------------------
# A2A 对齐验证
# ----------------------------------------------------------------------------
# 校验 sessions_send A2A 流程与 OpenClaw 真源的语义一致性：
#   * support_tokens   <- sessions-send-tokens.ts + auto-reply/tokens.ts
#   * a2a_context      <- sessions-send-helpers.ts
#   * session key 解析  <- sessions/session-key-utils.ts
#   * input_provenance <- sessions/input-provenance.ts
#   * agent_lanes      <- agents/lanes.ts + process/lanes.ts
#                         + utils/message-channel-constants.ts
#   * run_wait_pure    <- agents/run-wait.ts（纯判定部分）
#
# 刻意只依赖纯函数模块（不拉 gateway/websocket），保证测试可独立快速运行。
# 涉及 GatewayClient 的流程逻辑（wait 往返、announce target 查表回落、
# agent_step 的 run 提交）需要起 gateway mock，属集成测试范畴，不在此目标内。
cc_test(
    name = "test_a2a_alignment",
    srcs = ["test_a2a_alignment.cc"],
    copts = [
        "-std=c++17",
        "-Iinclude",
        "-Iexternal/json/single_include",
        "-Ithird_party",
    ],
    deps = [
        "//:hunterclaw_headers",
        "//src:hunterclaw_agents",
        "//src:hunterclaw_sessions",
        "//src:hunterclaw_tools",
    ],
)
