#pragma once

// ============================================================================
// remote_a2a/callback_route.h
// 把 CallbackEndpoint 挂到 HttpGateway 上。
//
// ★ 单独一个文件的理由：remote_a2a 核心【不应】依赖 httplib。
//   core 只用 std + nlohmann::json，可以单独编译与単测（现有 7 个测试套
//   都不链 httplib）。把 gateway 胶水隔离在这里，core 保持干净。
//
// ── 使用方式 ────────────────────────────────────────────────────────────────
//   auto reg = ...;                      // enable_remote_a2a 产出
//   mount_remote_a2a_callback(*gateway, reg);
//
// ⚠ 该函数同时做两件事，缺一个都会静默失效：
//   ① register_route("POST", "/remote-a2a/callback/:handle", ...)
//   ② add_auth_bypass_prefix("/remote-a2a/callback/")
//   只做 ① → 所有回调 401；只做 ② → 所有回调 404。
// ============================================================================

#include <memory>

#include "hunterclaw/core/types.h"
#include "hunterclaw/gateway/http_gateway.h"
#include "hunterclaw/remote_a2a/agent_registry.h"

namespace hunterclaw {
namespace remote_a2a {

/// 把回调端点挂到 gateway。
///
/// @return 是否实际挂载（未启用回调时返回 false，这【不是】错误）
///
/// ★ 未启用回调时【不】挂路由也【不】开旁路 ——
///   一个存在的旁路路径就是一处攻击面，没人用时不该存在。
bool mount_remote_a2a_callback(gateway::HttpGateway& gw,
                              const std::shared_ptr<RemoteA2aRegistry>& reg);

}  // namespace remote_a2a
}  // namespace hunterclaw
