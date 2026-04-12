#pragma once

// ============================================================================
// acp_server.h
// 🎯 对齐 OpenClaw: src/acp/server.ts + src/acp/session.ts
// ============================================================================

#include "hunterclaw/acp/acp_translator.h"
#include <memory>

namespace hunterclaw {
namespace acp {

/// 🎯 对齐: serveAcpGateway(opts)
void serve_acp_gateway(const AcpServerOptions& opts);

/// 🎯 对齐: defaultAcpSessionStore
std::unique_ptr<AcpSessionStore> create_default_session_store();
std::unique_ptr<AcpSessionStore> create_session_store(int max_sessions, int64_t idle_ttl_ms);

} // namespace acp
} // namespace hunterclaw
