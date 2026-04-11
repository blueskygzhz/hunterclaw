#include "hunterclaw/context/context_engine_maintenance.h"

#include <iostream>

namespace hunterclaw {
namespace context {

// ============================================================================
// Build Runtime Context (对齐 context-engine-maintenance.ts:16-40)
// ============================================================================

ContextEngineRuntimeContext build_context_engine_maintenance_runtime_context(
    const std::string& session_id,
    const std::string& session_file,
    const std::optional<std::string>& session_key,
    const ContextEngineRuntimeContext& base_context
) {
  // 对齐 buildContextEngineMaintenanceRuntimeContext
  // 将 transcript rewrite 能力注入到 runtime context 中
  ContextEngineRuntimeContext ctx = base_context;
  ctx["sessionId"] = session_id;
  ctx["sessionFile"] = session_file;
  if (session_key) {
    ctx["sessionKey"] = *session_key;
  }
  // rewriteTranscriptEntries 在 C++ 中通过 sessionFile 直接操作
  // 而非通过 JS 的闭包回调。此处设置标记供 engine 使用。
  ctx["hasTranscriptRewriteSupport"] = true;
  return ctx;
}

// ============================================================================
// Run Maintenance (对齐 context-engine-maintenance.ts:45-83)
// ============================================================================

std::string maintenance_reason_to_string(MaintenanceReason reason) {
  switch (reason) {
    case MaintenanceReason::BOOTSTRAP: return "bootstrap";
    case MaintenanceReason::COMPACTION: return "compaction";
    case MaintenanceReason::TURN: return "turn";
  }
  return "unknown";
}

std::optional<ContextEngineMaintenanceResult> run_context_engine_maintenance(
    ContextEngine* engine,
    const std::string& session_id,
    const std::string& session_file,
    MaintenanceReason reason,
    const std::optional<std::string>& session_key,
    const ContextEngineRuntimeContext& runtime_context
) {
  // 对齐 context-engine-maintenance.ts:53-83
  // 在 OpenClaw 中，maintain 是 optional 方法。
  // HunterClaw 的 ContextEngine 基类没有 maintain()，
  // 因此这里检查是否是支持 maintain 的扩展引擎。
  if (!engine) {
    return std::nullopt;
  }
  
  // 目前 LegacyContextEngine 不支持 maintain()。
  // 第三方 context engine 需要继承并实现。
  // 此处返回空结果表示 "engine doesn't support maintain"。
  //
  // 未来可通过 dynamic_cast 检测是否支持 maintain 接口:
  // auto* maintainable = dynamic_cast<MaintainableContextEngine*>(engine);
  
  std::string reason_str = maintenance_reason_to_string(reason);
  
  try {
    // Build runtime context with transcript rewrite support
    auto ctx = build_context_engine_maintenance_runtime_context(
        session_id, session_file, session_key, runtime_context
    );
    
    // 对齐 context-engine-maintenance.ts:71-77:
    // 如果 maintain 返回了 changed=true，记录日志
    // 当前 LegacyContextEngine 不支持 maintain，返回 nullopt
    return std::nullopt;
  } catch (const std::exception& e) {
    std::cerr << "[context-engine] maintain failed (" << reason_str << "): "
              << e.what() << std::endl;
    return std::nullopt;
  }
}

}  // namespace context
}  // namespace hunterclaw
