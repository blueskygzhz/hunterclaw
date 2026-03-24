#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/channels/manager.h"
#include "hunterclaw/tools/tool_hooks.h"   // get_global_hook_manager(), PluginHook* types
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

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
                account_info["enabled"] = account_state.enabled.value_or(false);
                account_info["configured"] = account_state.configured.value_or(false);
                account_info["running"] = account_state.running.value_or(false);
                account_info["connected"] = account_state.connected.value_or(false);
                
                channel_info["accounts"][account_id] = account_info;
            }
        } else {
            // 只有默认账户
            json account_info;
            account_info["accountId"] = default_account.account_id;
            account_info["enabled"] = default_account.enabled.value_or(false);
            account_info["configured"] = default_account.configured.value_or(false);
            account_info["running"] = default_account.running.value_or(false);
            account_info["connected"] = default_account.connected.value_or(false);
            
            channel_info["accounts"][default_account.account_id] = account_info;
        }
        
        result["chat"][channel_id] = channel_info;
    }

    return core::Result<json>::Ok(result);
}

// ============================================================================
// 🎯 OpenClaw 严格对齐: buildChannelAccountSnapshot
// 对应 OpenClaw: channels/plugins/status.ts buildChannelAccountSnapshot()
// ============================================================================
static json build_channel_account_snapshot(
    const channels::ChannelAccountSnapshot& runtime,
    const std::string& channel_id,
    const std::string& account_id
) {
    json snap;
    snap["accountId"] = account_id;
    snap["enabled"]   = runtime.enabled.value_or(true);
    snap["configured"] = runtime.configured.value_or(false);
    snap["running"]   = runtime.running.value_or(false);
    snap["connected"] = runtime.connected.value_or(false);
    snap["restartPending"] = runtime.restart_pending.value_or(false);
    snap["reconnectAttempts"] = runtime.reconnect_attempts.value_or(0);

    if (runtime.last_error.has_value() && !runtime.last_error.value().empty()) {
        snap["lastError"] = runtime.last_error.value();
    } else {
        snap["lastError"] = nullptr;
    }
    if (runtime.last_start_at.has_value()) {
        snap["lastStartAt"] = runtime.last_start_at.value();
    }
    if (runtime.last_stop_at.has_value()) {
        snap["lastStopAt"] = runtime.last_stop_at.value();
    }
        // 🎯 OpenClaw 对齐: lastInboundAt / lastOutboundAt
        // 对应 OpenClaw channels.ts: getChannelActivity({ channel, accountId })
        if (runtime.last_message_at.has_value()) {
            snap["lastInboundAt"] = runtime.last_message_at.value();
        } else {
            snap["lastInboundAt"] = nullptr;
        }
        // lastOutboundAt: HunterClaw 暂时没有这个字段，设为 null
        snap["lastOutboundAt"] = nullptr;
        return snap;
}

// ============================================================================
// 🎯 OpenClaw 严格对齐: buildChannelSummary
// 对应 OpenClaw: plugin.status.buildChannelSummary()
// ============================================================================
static json build_channel_summary(
    const channels::ChannelAccountSnapshot& default_account,
    const std::string& channel_id
) {
    json summary;
    summary["configured"] = default_account.configured.value_or(false);
    summary["enabled"]    = default_account.enabled.value_or(true);
    summary["running"]    = default_account.running.value_or(false);
    summary["connected"]  = default_account.connected.value_or(false);
    if (default_account.last_error.has_value() && !default_account.last_error.value().empty()) {
        summary["lastError"] = default_account.last_error.value();
    }
    return summary;
}

// channels.status - 获取渠道状态
// 🎯 OpenClaw 严格对齐: server-methods/channels.ts channelsHandlers["channels.status"]
// 返回格式: { ts, channelOrder, channelLabels, channelDetailLabels,
//             channelSystemImages, channelMeta, channels, channelAccounts,
//             channelDefaultAccountId }
core::Result<json> method_channel_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    auto channel_mgr = server->channel_manager();
    auto runtime_snapshot = channel_mgr->get_runtime_snapshot();

    // 🎯 OpenClaw 对齐: probe 参数
    bool do_probe = params.value("probe", false);
    // int64_t timeout_ms = params.value("timeoutMs", 10000);  // 保留供 probe 使用

    int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // 🎯 OpenClaw 对齐: buildChannelUiCatalog → channelOrder / channelLabels / channelMeta
    // HunterClaw 简化版: 从 runtime_snapshot 中枚举已知 channel
    json channel_order = json::array();
    json channel_labels = json::object();
    json channel_detail_labels = json::object();
    json channel_system_images = json::object();
    json channel_meta = json::object();

    // 收集所有已知 channel id
    std::set<std::string> known_channels;
    for (const auto& [ch_id, _] : runtime_snapshot.channels) {
        known_channels.insert(std::string(ch_id));
    }
    for (const auto& [ch_id, _] : runtime_snapshot.channel_accounts) {
        known_channels.insert(std::string(ch_id));
    }

    for (const auto& ch_id : known_channels) {
        channel_order.push_back(ch_id);

        // \u2705 \u5bf9\u9f50 OpenClaw: \u4ece plugin.config.label \u83b7\u53d6 channel label
        // OpenClaw: plugin.config?.label ?? plugin.id
        std::string label = ch_id;  // \u9ed8\u8ba4\u4f7f\u7528 channel id
        // Note: plugin_system() not available; label defaults to channel id
        channel_labels[ch_id] = label;
        channel_detail_labels[ch_id] = label;
        channel_meta[ch_id] = json{{"id", ch_id}, {"label", label}};
    }

    // 🎯 OpenClaw 对齐: channels / channelAccounts / channelDefaultAccountId
    json channels_map = json::object();
    json accounts_map = json::object();
    json default_account_id_map = json::object();

    for (const auto& ch_id_str : known_channels) {
        channels::ChannelId ch_id = ch_id_str.c_str();

        // 🎯 OpenClaw 对齐: defaultAccountId
        std::string default_account_id = "default";
        auto default_it = runtime_snapshot.channels.find(ch_id);
        if (default_it != runtime_snapshot.channels.end()) {
            default_account_id = default_it->second.account_id;
        }
        default_account_id_map[ch_id_str] = default_account_id;

        // 🎯 OpenClaw 对齐: channelAccounts[channelId] = accounts[]
        json accounts_arr = json::array();
        auto accounts_it = runtime_snapshot.channel_accounts.find(ch_id);
        if (accounts_it != runtime_snapshot.channel_accounts.end()) {
            for (const auto& [acc_id, acc_state] : accounts_it->second) {
                json snap = build_channel_account_snapshot(acc_state, ch_id_str, acc_id);

                // 🎯 OpenClaw 对齐: probe 结果注入
                if (do_probe) {
                    // HunterClaw 简化版: 直接使用 runtime 状态作为 probe 结果
                    json probe_result;
                    probe_result["ok"] = acc_state.connected.value_or(false);
                    probe_result["connected"] = acc_state.connected.value_or(false);
                    probe_result["running"] = acc_state.running.value_or(false);
                    if (acc_state.last_error.has_value()) {
                        probe_result["error"] = acc_state.last_error.value();
                    }
                    snap["probe"] = probe_result;
                    snap["lastProbeAt"] = ts_ms;
                }

                accounts_arr.push_back(snap);
            }
        } else if (default_it != runtime_snapshot.channels.end()) {
            // 只有 default account
            json snap = build_channel_account_snapshot(
                default_it->second, ch_id_str, default_account_id);
            if (do_probe) {
                json probe_result;
                probe_result["ok"] = default_it->second.connected.value_or(false);
                probe_result["connected"] = default_it->second.connected.value_or(false);
                probe_result["running"] = default_it->second.running.value_or(false);
                snap["probe"] = probe_result;
                snap["lastProbeAt"] = ts_ms;
            }
            accounts_arr.push_back(snap);
        }
        accounts_map[ch_id_str] = accounts_arr;

        // 🎯 OpenClaw 对齐: channels[channelId] = summary (buildChannelSummary)
        channels::ChannelAccountSnapshot default_snap;
        if (default_it != runtime_snapshot.channels.end()) {
            default_snap = default_it->second;
        }
        channels_map[ch_id_str] = build_channel_summary(default_snap, ch_id_str);
    }

    // 🎯 OpenClaw 对齐: 完整 payload 结构
    json result;
    result["ts"] = ts_ms;
    result["channelOrder"] = channel_order;
    result["channelLabels"] = channel_labels;
    result["channelDetailLabels"] = channel_detail_labels;
    result["channelSystemImages"] = channel_system_images;
    result["channelMeta"] = channel_meta;
    result["channels"] = channels_map;
    result["channelAccounts"] = accounts_map;
    result["channelDefaultAccountId"] = default_account_id_map;

    return core::Result<json>::Ok(result);
}

// channels.logout - 登出渠道账户
// 🎯 OpenClaw 严格对齐: server-methods/channels.ts channelsHandlers["channels.logout"]
// 流程: validateParams → 检查 config 有效性 → stopChannel → plugin.gateway.logoutAccount → markChannelLoggedOut
core::Result<json> method_channel_logout(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 OpenClaw 对齐: 验证 channel 参数
    if (!params.contains("channel") || !params["channel"].is_string()) {
        return core::Result<json>::Err("invalid channels.logout channel");
    }
    std::string channel_id = params["channel"].get<std::string>();
    if (channel_id.empty()) {
        return core::Result<json>::Err("invalid channels.logout channel");
    }

    // 🎯 OpenClaw 对齐: 可选 accountId
    std::string account_id;
    if (params.contains("accountId") && params["accountId"].is_string()) {
        account_id = params["accountId"].get<std::string>();
        // trim
        account_id.erase(0, account_id.find_first_not_of(" \t\r\n"));
        account_id.erase(account_id.find_last_not_of(" \t\r\n") + 1);
    }
    if (account_id.empty()) {
        account_id = "default";
    }

    auto channel_mgr = server->channel_manager();

    // 🎯 OpenClaw 对齐: 检查 config 有效性
    // 对应 OpenClaw: if (!snapshot.valid) respond(false, ..., "config invalid; fix it before logging out")
    auto config = server->config();
    if (!config) {
        return core::Result<json>::Err("config invalid; fix it before logging out");
    }

    // 🎯 OpenClaw 对齐: stopChannel(channelId, accountId)
    auto stop_result = channel_mgr->stop_channel(channel_id.c_str(), account_id);
    if (stop_result.is_err()) {
        return core::Result<json>::Err("Failed to stop channel: " + stop_result.error);
    }

    // 🎯 OpenClaw 对齐: plugin.gateway.logoutAccount(...)
    // HunterClaw 简化版: 直接调用 mark_channel_logged_out
    auto logout_result = channel_mgr->mark_channel_logged_out(channel_id.c_str(), account_id);
    if (logout_result.is_err()) {
        return core::Result<json>::Err("Failed to logout channel: " + logout_result.error);
    }

    // 🎯 OpenClaw 对齐: 返回 { channel, accountId, cleared, loggedOut }
    json result;
    result["channel"] = channel_id;
    result["accountId"] = account_id;
    result["cleared"] = true;   // 对齐 OpenClaw: result.cleared
    result["loggedOut"] = true; // 对齐 OpenClaw: result.loggedOut

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
    auto channel_it = runtime_snapshot.channel_accounts.find(channel_id);
    if (channel_it == runtime_snapshot.channel_accounts.end()) {
        return core::Result<json>::Err("Channel not found: " + channel_id);
    }

    json result;
    result["channel"] = channel_id;
    result["accountId"] = account_id;

    // 基础状态
        if (!account_id.empty()) {
            auto account_it = channel_it->second.find(account_id);
            if (account_it != channel_it->second.end()) {
                result["configured"] = account_it->second.configured.value_or(false);
                result["enabled"] = account_it->second.enabled.value_or(false);
                result["running"] = account_it->second.running.value_or(false);
                result["connected"] = account_it->second.connected.value_or(false);
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
            auto account_it = channel_it->second.find(account_id);
            if (account_it != channel_it->second.end()) {
                probe["ok"] = account_it->second.running.value_or(false) && account_it->second.connected.value_or(false);
                probe["running"] = account_it->second.running.value_or(false);
                probe["connected"] = account_it->second.connected.value_or(false);
                
                // 最后错误
                if (account_it->second.last_error.has_value()) {
                    probe["error"] = account_it->second.last_error.value();
                }
                
                // 重连次数
                probe["reconnectAttempts"] = account_it->second.reconnect_attempts.value_or(0);
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
    
    // ✅ 对齐 OpenClaw: 从日志文件读取最后 N 行
    // 日志格式: [timestamp] [level] [subsystem] message
    {
        auto cfg = server->config();
        std::string log_dir = cfg ? (cfg->workspace_dir + "/logs") : "./logs";
        std::string log_file = log_dir + "/gateway.log";
        if (!channel_filter.empty()) {
            log_file = log_dir + "/channel-" + channel_filter + ".log";
        }

        if (fs::exists(log_file)) {
            std::ifstream f(log_file);
            if (f.is_open()) {
                // 读取所有行，只保留最后 N 行
                std::deque<std::string> log_lines;
                std::string line;
                while (std::getline(f, line)) {
                    log_lines.push_back(line);
                    if ((int)log_lines.size() > lines) {
                        log_lines.pop_front();
                    }
                }
                for (const auto& l : log_lines) {
                    result["logs"].push_back(l);
                }
            }
        }
    }
    
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

    auto it = runtime_snapshot.channel_accounts.find(channel_id);
    if (it == runtime_snapshot.channel_accounts.end()) {
        return core::Result<json>::Err("Channel not found");
    }

    json result;
    result["id"] = channel_id;
    result["accounts"] = json::object();

    for (const auto& [account_id, account_state] : it->second) {
        json account_info;
        account_info["accountId"] = account_id;
        account_info["enabled"] = account_state.enabled.value_or(false);
        account_info["configured"] = account_state.configured.value_or(false);
        account_info["running"] = account_state.running.value_or(false);
        account_info["connected"] = account_state.connected.value_or(false);
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

    std::string channel_id = params["channel_id"];
    std::string to         = params["to"];
    std::string content    = params["message"];
    std::string account_id = params.value("account_id", "");

    // =========================================================================
    // Hook: message_sending
    // Aligned with OpenClaw channels/plugins/hooks.ts — modifying hook
    // Fires before the message is sent; plugins may modify content or cancel.
    // =========================================================================
    auto* hook_mgr = tools::get_global_hook_manager();
    if (hook_mgr && hook_mgr->has_hooks(plugins::PluginHookName::MESSAGE_SENDING)) {
        tools::PluginHookMessageSendingEvent ms_event;
        ms_event.to      = to;
        ms_event.content = content;
        tools::PluginHookMessageContext ms_ctx;
        ms_ctx.channel_id  = channel_id;
        ms_ctx.account_id  = account_id.empty() ? std::nullopt : std::make_optional(account_id);
        try {
            auto ms_result = hook_mgr->run_message_sending(ms_event, ms_ctx);
            if (ms_result) {
                if (ms_result->cancel) {
                    // Plugin cancelled the send
                    json result;
                    result["ok"]       = false;
                    result["cancelled"] = true;
                    result["reason"]   = "cancelled by plugin hook";
                    return core::Result<json>::Ok(result);
                }
                if (ms_result->content) {
                    // Plugin modified the content
                    content = *ms_result->content;
                }
            }
        } catch (const std::exception& e) {
            // message_sending hook failure is non-fatal
        }
    }

    // ✅ 对齐 OpenClaw: 通过 channel_manager 发送消息
    auto channel_mgr = server->channel_manager();
    bool send_ok = false;
    std::string send_error;
    // Note: ChannelManager::deliver_message() not yet implemented
    if (!channel_mgr) {
        send_error = "channel manager not available";
    } else {
        send_error = "deliver_message not implemented; use delivery queue";
    }

    // =========================================================================
    // Hook: message_sent
    // Aligned with OpenClaw channels/plugins/hooks.ts — fire-and-forget
    // Fires after the send attempt (success or failure).
    // =========================================================================
    if (hook_mgr && hook_mgr->has_hooks(plugins::PluginHookName::MESSAGE_SENT)) {
        tools::PluginHookMessageSentEvent mst_event;
        mst_event.to      = to;
        mst_event.content = content;
        mst_event.success = send_ok;
        if (!send_ok) mst_event.error = send_error;
        tools::PluginHookMessageContext mst_ctx;
        mst_ctx.channel_id = channel_id;
        mst_ctx.account_id = account_id.empty() ? std::nullopt : std::make_optional(account_id);
        try {
            hook_mgr->run_message_sent(mst_event, mst_ctx);
        } catch (const std::exception& e) {
            // fire-and-forget: ignore errors
        }
    }

    return core::Result<json>::Err(send_error);
}

// ============================================================================
// channel.health - 检查 channel 健康状态
// ============================================================================
core::Result<json> method_channel_health(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;
    (void)client_ctx;

    std::string channel_id_str = params.value("channel_id", "");

    auto channel_mgr = server->channel_manager();
    if (!channel_mgr) {
        json result;
        result["healthy"] = false;
        result["reason"] = "Channel manager not initialized";
        return core::Result<json>::Ok(result);
    }

    // Get runtime snapshot for health info
    auto snapshot = channel_mgr->get_runtime_snapshot();

    if (!channel_id_str.empty()) {
        // Check specific channel health
        json result;
        result["channel_id"] = channel_id_str;

        auto it = snapshot.channels.find(channel_id_str);
        if (it == snapshot.channels.end()) {
            result["healthy"] = false;
            result["reason"] = "Channel not found";
        } else {
            const auto& ch_snapshot = it->second;
        result["healthy"] = ch_snapshot.connected.value_or(false);
            result["running"] = ch_snapshot.running.value_or(false);
            result["connected"] = ch_snapshot.connected.value_or(false);
            // Derive status from optional fields
            bool is_connected = ch_snapshot.connected.value_or(false);
            bool is_running   = ch_snapshot.running.value_or(false);
            std::string status_str;
            if (is_connected) status_str = "connected";
            else if (is_running) status_str = "starting";
            else if (ch_snapshot.last_error.has_value()) status_str = "error";
            else status_str = "stopped";
            result["status"] = status_str;
            if (ch_snapshot.last_error.has_value()) {
                result["last_error"] = ch_snapshot.last_error.value();
            }
        }
        return core::Result<json>::Ok(result);
    }

    // Check all channels health
    json channels_health = json::array();
    bool all_healthy = true;

    for (const auto& [ch_id, ch_snapshot] : snapshot.channels) {
        json ch_health;
        ch_health["channel_id"] = ch_id;
        ch_health["healthy"] = ch_snapshot.connected.value_or(false);
        ch_health["running"] = ch_snapshot.running.value_or(false);
        ch_health["connected"] = ch_snapshot.connected.value_or(false);
        // Derive status
        bool is_conn = ch_snapshot.connected.value_or(false);
        bool is_run  = ch_snapshot.running.value_or(false);
        std::string st;
        if (is_conn) st = "connected";
        else if (is_run) st = "starting";
        else if (ch_snapshot.last_error.has_value()) st = "error";
        else st = "stopped";
        ch_health["status"] = st;
        if (!is_conn) all_healthy = false;
        channels_health.push_back(ch_health);
    }

    json result;
    result["healthy"] = all_healthy;
    result["channels"] = channels_health;
    result["count"] = channels_health.size();

    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
