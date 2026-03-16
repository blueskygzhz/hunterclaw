#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/channels/manager.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Channel Methods - 完全对齐 OpenClaw
// ============================================================================

// channels.list - 列出所有渠道账户 (对齐 OpenClaw channelsListCommand)
core::Result<json> method_channel_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    auto channel_mgr = server->channel_manager();
    auto runtime_snapshot = channel_mgr->get_runtime_snapshot();

    json result;
    result["chat"] = json::object();
    result["auth"] = json::object();

    // 遍历所有渠道
    for (const auto& [channel_id, default_account] : runtime_snapshot.channels) {
        json channel_info;
        channel_info["channel"] = channel_id;
        channel_info["accounts"] = json::object();

        // 如果有多账户,遍历该渠道的所有账户
        auto multi_accounts_it = runtime_snapshot.channel_accounts.find(channel_id);
        if (multi_accounts_it != runtime_snapshot.channel_accounts.end()) {
            for (const auto& [account_id, account_state] : multi_accounts_it->second) {
                json account_info;
                account_info["accountId"] = account_id;
                account_info["enabled"] = account_state.enabled;
                account_info["configured"] = account_state.configured;
                account_info["running"] = account_state.running;
                account_info["connected"] = account_state.connected;
                
                channel_info["accounts"][account_id] = account_info;
            }
        } else {
            // 只有默认账户
            json account_info;
            account_info["accountId"] = default_account.account_id;
            account_info["enabled"] = default_account.enabled;
            account_info["configured"] = default_account.configured;
            account_info["running"] = default_account.running;
            account_info["connected"] = default_account.connected;
            
            channel_info["accounts"][default_account.account_id] = account_info;
        }
        
        channels.push_back(channel_info);
    }

    result["channels"] = channels;
    return core::Result<json>::Ok(result);
}

// list - 列出所有渠道
core::Result<json> method_channel_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    auto channel_mgr = server->get_channel_manager();
    if (!channel_mgr) {
        return core::Result<json>::Err("ChannelManager not available");
    }
    
    auto runtime_snapshot = channel_mgr->get_runtime_snapshot();

    json result;
    result["channels"] = json::array();

    // 遍历所有渠道账户
    for (const auto& [channel_id, default_account] : runtime_snapshot.channels) {
        // 如果有多账户
        auto multi_accounts_it = runtime_snapshot.channel_accounts.find(channel_id);
        if (multi_accounts_it != runtime_snapshot.channel_accounts.end()) {
            for (const auto& [account_id, account_state] : multi_accounts_it->second) {
            
            // Token source 信息
            if (!account_state.token_source.empty()) {
                account_info["tokenSource"] = account_state.token_source;
            }
            
            // 认证年龄（毫秒）
            if (account_state.auth_age_ms > 0) {
                account_info["authAgeMs"] = account_state.auth_age_ms;
            }

            channel_info["accounts"][account_id] = account_info;
        }

        result["chat"][channel_id] = channel_info;
    }

    // 可选: 使用统计 (如果请求了)
    if (params.contains("usage") && params["usage"].get<bool>()) {
        result["usage"] = json::object();
        // TODO: 实现 provider 使用统计
    }

    return core::Result<json>::Ok(result);
}

// channels.status - 获取渠道状态 (对齐 OpenClaw channelsStatusCommand)
core::Result<json> method_channel_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    auto channel_mgr = server->channel_manager();
    auto runtime_snapshot = channel_mgr->get_runtime_snapshot();
    
    // 是否进行探测 (probe)
    bool do_probe = params.value("probe", false);
    int timeout_ms = params.value("timeout", 5000);

    json result = json::object();

    // 遍历所有渠道账户
    for (const auto& [channel_id, channel_state] : runtime_snapshot.channels) {
        for (const auto& [account_id, account_state] : channel_state.accounts) {
            std::string key = channel_id + ":" + account_id;
            
            json status;
            status["channel"] = channel_id;
            status["accountId"] = account_id;
            status["enabled"] = account_state.enabled;
            status["configured"] = account_state.configured;
            status["linked"] = account_state.linked;

            // Probe 探测
            if (do_probe) {
                json probe_result;
                probe_result["ok"] = account_state.running;
                probe_result["connected"] = account_state.connected;
                
                // 最后入站/出站消息时间
                if (account_state.last_inbound_at > 0) {
                    probe_result["lastInboundAt"] = account_state.last_inbound_at;
                }
                if (account_state.last_outbound_at > 0) {
                    probe_result["lastOutboundAt"] = account_state.last_outbound_at;
                }
                
                // Bot 信息
                if (!account_state.bot_username.empty()) {
                    json bot_info;
                    bot_info["username"] = account_state.bot_username;
                    bot_info["id"] = account_state.bot_id;
                    probe_result["bot"] = bot_info;
                }
                
                // 错误信息
                if (!account_state.error.empty()) {
                    probe_result["error"] = account_state.error;
                }
                
                status["probe"] = probe_result;
            }

            result[key] = status;
        }
    }

    return core::Result<json>::Ok(result);
}

// channels.capabilities - 获取渠道能力 (对齐 OpenClaw channelsCapabilitiesCommand)
core::Result<json> method_channel_capabilities(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("channel")) {
        return core::Result<json>::Err("Missing required parameter: channel");
    }

    std::string channel_id = params["channel"];
    std::string account_id = params.value("account", "");
    std::string target = params.value("target", "");

    auto channel_mgr = server->channel_manager();
    auto runtime_snapshot = channel_mgr->get_runtime_snapshot();

    // 查找渠道
    auto channel_it = runtime_snapshot.channels.find(channel_id);
    if (channel_it == runtime_snapshot.channels.end()) {
        return core::Result<json>::Err("Channel not found: " + channel_id);
    }

    json result;
    result["channel"] = channel_id;
    result["accountId"] = account_id;

    // 基础状态
    if (!account_id.empty()) {
        auto account_it = channel_it->second.accounts.find(account_id);
        if (account_it != channel_it->second.accounts.end()) {
            result["configured"] = account_it->second.configured;
            result["enabled"] = account_it->second.enabled;
        }
    }

    // 能力信息 (support)
    json support;
    
    // 聊天类型
    support["chatTypes"] = json::array({"direct", "group"});
    
    // 功能支持
    support["polls"] = false;
    support["reactions"] = true;
    support["edit"] = true;
    support["unsend"] = true;
    support["reply"] = true;
    support["threads"] = false;
    support["media"] = true;
    support["attachments"] = true;
    support["voice"] = false;
    support["video"] = false;
    support["location"] = false;
    support["stickers"] = false;
    support["typing"] = true;
    support["presence"] = true;
    
    // 根据渠道类型定制能力
    if (channel_id == "discord") {
        support["threads"] = true;
        support["stickers"] = true;
    } else if (channel_id == "slack") {
        support["threads"] = true;
    } else if (channel_id == "telegram") {
        support["polls"] = true;
        support["stickers"] = true;
        support["location"] = true;
    }

    result["support"] = support;

    // 可用操作
    json actions = json::array();
    actions.push_back("send");
    actions.push_back("receive");
    if (support["edit"].get<bool>()) actions.push_back("edit");
    if (support["unsend"].get<bool>()) actions.push_back("unsend");
    if (support["reactions"].get<bool>()) actions.push_back("react");
    result["actions"] = actions;

    // 探测结果 (如果请求了)
    if (params.contains("probe") && params["probe"].get<bool>()) {
        json probe;
        if (!account_id.empty()) {
            auto account_it = channel_it->second.accounts.find(account_id);
            if (account_it != channel_it->second.accounts.end()) {
                probe["ok"] = account_it->second.running && account_it->second.connected;
                
                // Bot 信息
                if (!account_it->second.bot_username.empty()) {
                    json bot;
                    bot["username"] = account_it->second.bot_username;
                    bot["id"] = account_it->second.bot_id;
                    probe["bot"] = bot;
                }
                
                // 错误信息
                if (!account_it->second.error.empty()) {
                    probe["error"] = account_it->second.error;
                }
            }
        }
        result["probe"] = probe;
    }

    return core::Result<json>::Ok(result);
}

// channels.logs - 获取渠道日志 (对齐 OpenClaw channelsLogsCommand)
core::Result<json> method_channel_logs(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    std::string channel_filter = params.value("channel", "");
    int lines = params.value("lines", 100);
    
    // 限制最大行数
    if (lines > 1000) lines = 1000;

    json result;
    result["logs"] = json::array();
    result["channel"] = channel_filter;
    result["lines"] = lines;
    
    // TODO: 从日志文件读取最后 N 行
    // 日志格式: [timestamp] [level] [subsystem] message
    
    return core::Result<json>::Ok(result);
}

// channels.resolve - 解析渠道目标标识符 (对齐 OpenClaw channelsResolveCommand)
core::Result<json> method_channel_resolve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("channel")) {
        return core::Result<json>::Err("Missing required parameter: channel");
    }

    std::string channel_id = params["channel"];
    std::string account_id = params.value("account", "");
    json entries = params.value("entries", json::array());
    std::string kind = params.value("kind", "auto");

    json results = json::array();

    // 解析每个条目
    for (const auto& entry : entries) {
        std::string input = entry.is_string() ? entry.get<std::string>() : entry.dump();
        
        json resolve_result;
        resolve_result["input"] = input;
        resolve_result["resolved"] = true;
        
        // 自动检测类型
        if (kind == "auto") {
            // 根据格式推断类型
            if (input.find("@") == 0) {
                resolve_result["kind"] = "user";
                resolve_result["id"] = input.substr(1);
            } else if (input.find("#") == 0) {
                resolve_result["kind"] = "channel";
                resolve_result["id"] = input.substr(1);
            } else if (input.find("-") != std::string::npos) {
                resolve_result["kind"] = "group";
                resolve_result["id"] = input;
            } else {
                resolve_result["kind"] = "user";
                resolve_result["id"] = input;
            }
        } else {
            resolve_result["kind"] = kind;
            resolve_result["id"] = input;
        }
        
        resolve_result["name"] = input;  // Placeholder
        
        results.push_back(resolve_result);
    }

    return core::Result<json>::Ok(results);
}

// 保留原有的基础方法
core::Result<json> method_channel_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("channel_id")) {
        return core::Result<json>::Err("Missing channel_id");
    }

    std::string channel_id = params["channel_id"];
    auto channel_mgr = server->channel_manager();
    auto runtime_snapshot = channel_mgr->get_runtime_snapshot();

    auto it = runtime_snapshot.channels.find(channel_id);
    if (it == runtime_snapshot.channels.end()) {
        return core::Result<json>::Err("Channel not found");
    }

    json result;
    result["id"] = channel_id;
    result["accounts"] = json::object();

    for (const auto& [account_id, account_state] : it->second.accounts) {
        json account_info;
        account_info["accountId"] = account_id;
        account_info["enabled"] = account_state.enabled;
        account_info["configured"] = account_state.configured;
        account_info["linked"] = account_state.linked;
        account_info["running"] = account_state.running;
        result["accounts"][account_id] = account_info;
    }

    return core::Result<json>::Ok(result);
}

core::Result<json> method_channel_start(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("channel_id")) {
        return core::Result<json>::Err("Missing channel_id");
    }

    std::string channel_id = params["channel_id"];
    std::string account_id = params.value("account_id", "");

    auto channel_mgr = server->channel_manager();
    auto result = channel_mgr->start_channel(channel_id, account_id);

    if (result.is_err()) {
        return core::Result<json>::Err(result.error);
    }

    json response;
    response["ok"] = true;
    response["channel_id"] = channel_id;
    response["account_id"] = account_id.empty() ? "default" : account_id;

    return core::Result<json>::Ok(response);
}

core::Result<json> method_channel_stop(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("channel_id")) {
        return core::Result<json>::Err("Missing channel_id");
    }

    std::string channel_id = params["channel_id"];
    std::string account_id = params.value("account_id", "");

    auto channel_mgr = server->channel_manager();
    auto result = channel_mgr->stop_channel(channel_id, account_id);

    if (result.is_err()) {
        return core::Result<json>::Err(result.error);
    }

    json response;
    response["ok"] = true;
    response["channel_id"] = channel_id;

    return core::Result<json>::Ok(response);
}

core::Result<json> method_channel_send(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("channel_id") || !params.contains("to") || !params.contains("message")) {
        return core::Result<json>::Err("Missing required parameters: channel_id, to, message");
    }

    // TODO: 实现消息发送
    // auto channel_mgr = server->channel_manager();
    // auto result = channel_mgr->deliver_message(...);
    
    return core::Result<json>::Err("channel.send not implemented yet");
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
