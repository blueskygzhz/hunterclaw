// ============================================================================
// acp_session_mapper_gateway.cc
// GatewayClient* 便捷重载 —— 单独成文件。
//
// 原因：acp_session_mapper.cc 的核心逻辑是纯函数（只依赖 GatewayRequestFn
// 回调），可被轻量单测直接链接。而 GatewayClient::request 是模板，会实例化
// request_async 并拖入 ixwebsocket 整条依赖链。拆开后测试只链前者。
// ============================================================================

#include "hunterclaw/acp/acp_session_mapper.h"
#include "hunterclaw/gateway/client.h"

#include <stdexcept>

namespace hunterclaw {
namespace acp {

namespace {

GatewayRequestFn make_gateway_request_fn(gateway::GatewayClient* gw) {
    if (!gw) return GatewayRequestFn();
    return [gw](const std::string& method, const nlohmann::json& params) -> nlohmann::json {
        auto result = gw->request<nlohmann::json>(method, params);
        if (!result.is_ok()) {
            // 让上层"resolve 失败即抛错"的语义得以生效（対齐 session-mapper.ts:46）
            throw std::runtime_error(method + " failed: " + result.error);
        }
        return result.get();
    };
}

}  // namespace

std::string resolve_session_key(const AcpSessionMeta_Parsed& meta,
                                const std::string& fallback_key,
                                const SessionKeyResolutionOptions& opts,
                                gateway::GatewayClient* gw) {
    return resolve_session_key(meta, fallback_key, opts, make_gateway_request_fn(gw));
}

bool reset_session_if_needed(const AcpSessionMeta_Parsed& meta,
                             const std::string& session_key,
                             const SessionKeyResolutionOptions& opts,
                             gateway::GatewayClient* gw) {
    return reset_session_if_needed(meta, session_key, opts, make_gateway_request_fn(gw));
}

} // namespace acp
} // namespace hunterclaw
