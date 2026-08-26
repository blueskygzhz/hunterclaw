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

# ----------------------------------------------------------------------------
# ACP 对齐验证
# ----------------------------------------------------------------------------
# 校验 ACP 实现与 OpenClaw 真源的语义一致性：
#   * acp_client            <- acp/approval-classifier.ts + policy.ts
#                              + client-helpers.ts
#   * acp_tool_status       <- acp/tool-status.ts
#   * acp_conversation_id   <- acp/conversation-id.ts
#   * acp_session_mapper    <- acp/session-mapper.ts
#
# 重点覆盖**安全相关**语义（这些曾是实际存在的漏洞）：
#   - 工具名三源交叉校验（防对端伪造身份绕过审批）
#   - 路径 cwd 范围判定（防越界读取 /etc/shadow 之类）
#   - control_plane 类工具不得自动批准
#
# 刻意只链纯函数实现（不含 acp_session_mapper_gateway.cc），
# 避免拖入 ixwebsocket 依赖链，保证测试可独立快速运行。
cc_test(
    name = "test_acp_alignment",
    srcs = ["test_acp_alignment.cc"],
    copts = [
        "-std=c++17",
        "-Iinclude",
        "-Iexternal/json/single_include",
        "-Ithird_party",
    ],
    deps = [
        "//:hunterclaw_headers",
        "//src:hunterclaw_acp",
    ],
)
