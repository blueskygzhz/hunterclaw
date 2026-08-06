// ============================================================================
// acp_stdio_server.cc
// 🎯 严格对齐 OpenClaw: src/acp/server.ts (serveAcpGateway)
//   在 stdin/stdout 上以 ndjson JSON-RPC 提供 ACP Agent 服务，
//   方法名对齐 ACP 规范（slash 命名: session/new, session/prompt, ...）。
//
// 与 gateway 解耦：prompt/cancel 派发通过注入的回调完成，acp 库不依赖 gateway。
// ============================================================================

#include "hunterclaw/acp/acp_server.h"
#include "hunterclaw/acp/acp_stdio_transport.h"
#include "hunterclaw/acp/acp_translator.h"
#include "hunterclaw/acp/runtime_errors.h"

#include <memory>
#include <string>
#include <vector>

namespace hunterclaw {
namespace acp {

namespace {

// ── JSON 序列化辅助（对齐 OC translator.ts 的 ACP 响应字段，camelCase）───────────

nlohmann::json config_option_to_json(const SessionConfigOption& o) {
    nlohmann::json opts = nlohmann::json::array();
  for (const auto& v : o.options)
      opts.push_back({{"value", v.value}, {"name", v.name}});
    return nlohmann::json{
      {"type", o.type},
        {"id", o.id},
        {"name", o.name},
        {"category", o.category},
     {"description", o.description},
        {"currentValue", o.current_value},
      {"options", opts},
    };
}

nlohmann::json config_options_to_json(const std::vector<SessionConfigOption>& v) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& o : v) arr.push_back(config_option_to_json(o));
    return arr;
}

nlohmann::json modes_to_json(const SessionModeState& m) {
    nlohmann::json avail = nlohmann::json::array();
    for (const auto& mode : m.available_modes)
        avail.push_back({{"id", mode.id}, {"name", mode.name}, {"description", mode.description}});
    return nlohmann::json{
        {"currentModeId", m.current_mode_id},
      {"availableModes", avail},
    };
}

nlohmann::json agent_capabilities_to_json(const AcpAgentCapabilities& c) {
    // 🎯 对齐 OC initialize response.agentCapabilities
    nlohmann::json prompt_caps{
        {"image", c.prompt_image},
        {"audio", c.prompt_audio},
        {"embeddedContext", c.prompt_embedded_context},
    };
    nlohmann::json mcp_caps{
   {"http", c.mcp_http},
    {"sse", c.mcp_sse},
    };
    nlohmann::json session_caps = nlohmann::json::object();
    if (c.session_list_supported) session_caps["list"] = nlohmann::json::object();
    return nlohmann::json{
        {"loadSession", c.load_session},
        {"promptCapabilities", prompt_caps},
        {"mcpCapabilities", mcp_caps},
   {"sessionCapabilities", session_caps},
    };
}

// JSON-RPC error 从 AcpRuntimeError 派生（对齐 ACP 约定：业务错误用 InternalError/InvalidParams）
[[noreturn]] void rethrow_as_rpc(const AcpRuntimeError& e) {
 throw AcpRpcError(JsonRpcErrorCode::InternalError, e.what());
}

// 解析 prompt content blocks（ACP session/prompt 的 prompt 是 ContentBlock[]）
void parse_prompt_blocks(const nlohmann::json& params, PromptRequest& out) {
    // ACP: params.prompt = [{type:"text", text}, {type:"image", mimeType, data}, ...]
    if (params.contains("prompt") && params["prompt"].is_array()) {
for (const auto& blk : params["prompt"]) {
            std::string t = blk.value("type", "");
     if (t == "text") {
         out.text += blk.value("text", "");
   } else if (t == "image" || t == "audio" || t == "resource") {
         PromptAttachment a;
             a.type = t;
     a.mime_type = blk.value("mimeType", "");
           a.content = blk.value("data", blk.value("text", ""));
out.attachments.push_back(std::move(a));
          }
    }
    } else {
        // 兼容简化格式 params.text
        out.text = params.value("text", "");
    }
}

}  // namespace

// ── serve_acp_stdio ──────────────────────────────────────────────────────────
void serve_acp_stdio(const AcpServerOptions& opts, const AcpStdioServerDeps& deps) {
    auto store = create_default_session_store();
    auto agent = std::make_unique<AcpGatewayAgent>(nullptr, opts, store.get());

    AcpStdioConnection conn;
    conn.set_verbose(opts.verbose);
 agent->set_connection(&conn);
    if (deps.prompt_dispatch) agent->set_prompt_dispatch(deps.prompt_dispatch);
 if (deps.cancel_dispatch) agent->set_cancel_dispatch(deps.cancel_dispatch);

    AcpGatewayAgent* ag = agent.get();

    // ── initialize ────────────────────────────────────────────────────────────
    conn.register_method("initialize", [ag](const AcpRequestContext& ctx) -> nlohmann::json {
  InitializeRequest req;
        const auto& p = ctx.params;
  // ACP: clientInfo? / clientCapabilities?（HC 只取 name/version 做记录）
 if (p.contains("clientInfo") && p["clientInfo"].is_object()) {
            req.client_name = p["clientInfo"].value("name", "");
            req.client_version = p["clientInfo"].value("version", "");
        }
        auto r = ag->handle_initialize(req);
     nlohmann::json auth = nlohmann::json::array();
        for (auto& a : r.auth_methods) auth.push_back(a);
        return nlohmann::json{
            {"protocolVersion", r.protocol_version},
  {"agentInfo", {{"name", r.agent.name}, {"title", r.agent.title}, {"version", r.agent.version}}},
      {"agentCapabilities", agent_capabilities_to_json(r.agent_capabilities)},
   {"authMethods", auth},
        };
    });

    // ── authenticate ────────────────────────────────────────────────────────────
    conn.register_method("authenticate", [ag](const AcpRequestContext& ctx) -> nlohmann::json {
        AuthenticateRequest req;
        req.token = ctx.params.value("token", "");
   auto r = ag->handle_authenticate(req);
   return nlohmann::json{{"authenticated", r.authenticated}};
    });

  // ── session/new ─────────────────────────────────────────────────────────────
    conn.register_method("session/new", [ag](const AcpRequestContext& ctx) -> nlohmann::json {
        NewSessionRequest req;
        req.cwd = ctx.params.value("cwd", ".");
        req.meta = ctx.params.contains("_meta") ? ctx.params["_meta"]
       : ctx.params.value("meta", nlohmann::json::object());
     try {
    auto r = ag->handle_new_session(req);
            return nlohmann::json{
    {"sessionId", r.session_id},
             {"configOptions", config_options_to_json(r.config_options)},
   {"modes", modes_to_json(r.modes)},
         };
        } catch (const AcpRuntimeError& e) { rethrow_as_rpc(e); }
    });

    // ── session/load ────────────────────────────────────────────────────────────
    conn.register_method("session/load", [ag](const AcpRequestContext& ctx) -> nlohmann::json {
        LoadSessionRequest req;
        req.session_id = ctx.params.value("sessionId", "");
 req.replay = ctx.params.value("replay", true);
      try {
            auto r = ag->handle_load_session(req);
            nlohmann::json replay = nlohmann::json::array();
            for (auto& c : r.replay_chunks)
 replay.push_back({{"sessionUpdate", c.session_update}, {"text", c.text}});
  return nlohmann::json{
   {"sessionId", r.session_id},
      {"configOptions", config_options_to_json(r.config_options)},
    {"modes", modes_to_json(r.modes)},
     {"replayChunks", replay},
            };
  } catch (const AcpRuntimeError& e) { rethrow_as_rpc(e); }
    });

    // ── session/list ────────────────────────────────────────────────────────────
    conn.register_method("session/list", [ag](const AcpRequestContext& ctx) -> nlohmann::json {
        ListSessionsRequest req;
        req.limit = ctx.params.value("limit", 50);
        auto r = ag->handle_list_sessions(req);
        nlohmann::json arr = nlohmann::json::array();
 for (auto& s : r.sessions)
   arr.push_back({{"sessionId", s.session_id}, {"title", s.title}, {"updatedAt", s.updated_at}});
        return nlohmann::json{{"sessions", arr}};
    });

    // ── session/prompt ──────────────────────────────────────────────────────────
    conn.register_method("session/prompt", [ag](const AcpRequestContext& ctx) -> nlohmann::json {
        PromptRequest req;
        req.session_id = ctx.params.value("sessionId", "");
        req.idempotency_key = ctx.params.value("idempotencyKey",
          ctx.params.value("_meta", nlohmann::json::object()).value("idempotencyKey", ""));
        parse_prompt_blocks(ctx.params, req);
        try {
      auto r = ag->handle_prompt(req);
            // 🎯 对齐 OC: prompt response = { stopReason }
        return nlohmann::json{{"stopReason", r.stop_reason}};
        } catch (const AcpRuntimeError& e) { rethrow_as_rpc(e); }
    });

    // ── session/cancel (notification) ─────────────────────────────────────────────
 conn.register_method("session/cancel", [ag](const AcpRequestContext& ctx) -> nlohmann::json {
        std::string sid = ctx.params.value("sessionId", "");
        ag->handle_cancel(sid);
        return nlohmann::json::object();  // notification: 返回被忽略
    });

    // ── session/set_mode ──────────────────────────────────────────────────────────
    conn.register_method("session/set_mode", [ag](const AcpRequestContext& ctx) -> nlohmann::json {
        SetSessionModeRequest req;
        req.session_id = ctx.params.value("sessionId", "");
    req.mode_id = ctx.params.value("modeId", "");
    try {
            auto r = ag->handle_set_session_mode(req);
 return nlohmann::json{
             {"modes", modes_to_json(r.modes)},
    {"configOptions", config_options_to_json(r.config_options)},
            };
        } catch (const AcpRuntimeError& e) { rethrow_as_rpc(e); }
    });

    // ── session/set_config_option ────────────────────────────────────────────────
 conn.register_method("session/set_config_option", [ag](const AcpRequestContext& ctx) -> nlohmann::json {
        SetSessionConfigOptionRequest req;
   req.session_id = ctx.params.value("sessionId", "");
        req.option_id = ctx.params.value("optionId", "");
        req.value = ctx.params.value("value", "");
   try {
  auto r = ag->handle_set_session_config_option(req);
    return nlohmann::json{
                {"configOptions", config_options_to_json(r.config_options)},
 {"modes", modes_to_json(r.modes)},
     };
        } catch (const AcpRuntimeError& e) { rethrow_as_rpc(e); }
    });

    // 启动 stdin 读取循环并阻塞直到 EOF。
    conn.start();
    conn.wait_closed();
}

}  // namespace acp
}  // namespace hunterclaw
