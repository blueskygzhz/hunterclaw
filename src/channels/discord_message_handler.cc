// Aligned with openclaw: src/discord/monitor/message-handler.ts
#include "hunterclaw/channels/discord_message_handler.h"
#include "hunterclaw/routing/resolver.h"
#include <chrono>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw {
namespace channels {

namespace {
std::shared_ptr<spdlog::logger> get_fallback_logger() {
    auto log = spdlog::get("discord_message_handler");
    if (!log) log = spdlog::stdout_logger_mt("discord_message_handler");
    return log;
}
}  // namespace

// ============================================================================
// create_discord_message_handler
// Aligned with OpenClaw: createDiscordMessageHandler (message-handler.ts)
// ============================================================================
DiscordMessageHandlerWithLifecycle create_discord_message_handler(
    DiscordMessageHandlerParams params)
{
    // Aligned with openclaw: createDiscordInboundWorker({ runtime, setStatus, abortSignal, runTimeoutMs })
    DiscordInboundWorkerParams worker_params;
    worker_params.logger          = params.logger;
    worker_params.set_status      = params.set_status;
    worker_params.abort_signal    = params.abort_signal;
    worker_params.run_timeout_ms  = params.worker_run_timeout_ms;
    worker_params.cfg             = params.cfg;
    worker_params.account_id      = params.account_id;
    worker_params.dispatch_inbound = params.dispatch_inbound;

    auto inbound_worker = std::make_shared<DiscordInboundWorker>(std::move(worker_params));

    // Capture params needed by the handler closure
    const std::string account_id   = params.account_id;
    const auto bot_user_id         = params.bot_user_id;
    const auto logger              = params.logger;
    const auto cfg                 = params.cfg;
    const bool dm_enabled          = params.dm_enabled;
    const bool group_dm_enabled    = params.group_dm_enabled;
    const std::string dm_policy    = params.dm_policy;
    const std::string group_policy = params.group_policy;
    const auto allow_from          = params.allow_from;
    const auto group_dm_channels   = params.group_dm_channels;
    const bool allow_name_matching = params.allow_name_matching;
    const auto guild_entries       = params.guild_entries;

    // Aligned with openclaw:
    //   const handler = async (data, client, options) => {
    //     if (options?.abortSignal?.aborted) return;
    //     // Filter bot-own messages early
    //     const msgAuthorId = data.message?.author?.id ?? data.author?.id;
    //     if (botUserId && msgAuthorId === botUserId) return;
    //     await debouncer.enqueue({ data, client, abortSignal });
    //   };
    //   handler.deactivate = inboundWorker.deactivate;
    DiscordMessageHandler handler = [=](const nlohmann::json& data,
                                        const std::string& /*client_account_id*/) mutable
    {
        try {
            // Aligned with openclaw: filter bot-own messages early
            // const msgAuthorId = data.message?.author?.id ?? data.author?.id
            std::string msg_author_id;
            if (data.contains("message") && data["message"].is_object()) {
                const auto& msg = data["message"];
                if (msg.contains("author") && msg["author"].is_object()
                    && msg["author"].contains("id") && msg["author"]["id"].is_string()) {
                    msg_author_id = msg["author"]["id"].get<std::string>();
                }
            }
            if (msg_author_id.empty() && data.contains("author") && data["author"].is_object()
                && data["author"].contains("id") && data["author"]["id"].is_string()) {
                msg_author_id = data["author"]["id"].get<std::string>();
            }
            // Aligned with openclaw: if (botUserId && msgAuthorId === botUserId) return
            if (bot_user_id && !bot_user_id->empty() && msg_author_id == *bot_user_id) {
                return;
            }

            // Aligned with openclaw: buildDiscordInboundJob(ctx) → inboundWorker.enqueue(job)
            // HunterClaw: build a minimal DiscordInboundJob from raw event data.
            // Full preflight (preflightDiscordMessage) will be implemented when
            // the message-handler preflight module is ported.
            DiscordInboundJob job;

            // Aligned with openclaw: queueKey = resolveDiscordInboundJobQueueKey(payload)
            // Use channel_id as queue key for per-channel serialization
            std::string channel_id;
            if (data.contains("channel_id") && data["channel_id"].is_string()) {
                channel_id = data["channel_id"].get<std::string>();
            }

            DiscordInboundJobPayload& payload = job.payload;
            payload.account_id         = account_id;
            payload.message_channel_id = channel_id;
            payload.channel            = "discord";

            // Aligned with openclaw: isGuildMessage = Boolean(data.guild_id)
            const bool is_guild = data.contains("guild_id") && !data["guild_id"].is_null()
                && data["guild_id"].is_string() && !data["guild_id"].get<std::string>().empty();
            payload.is_guild_message  = is_guild;
            payload.is_direct_message = !is_guild;

            if (is_guild && data["guild_id"].is_string()) {
                payload.guild_id = data["guild_id"].get<std::string>();
            }
            payload.bot_user_id = bot_user_id;

            // Aligned with openclaw: payload.data = sanitized event data
            payload.data = data;

            // Aligned with openclaw: payload.message = data.message (sanitized)
            if (data.contains("message") && data["message"].is_object()) {
                payload.message = data["message"];
            } else {
                // MESSAGE_CREATE: the event IS the message
                payload.message = data;
            }

            // 🎯 严格对齐 OpenClaw: discord/monitor/route-resolution.ts
            // 用 resolve_discord_bound_conversation_route 构建正确的 session_key
            {
                // 提取 author.id (DM 时作为 direct_user_id)
                std::string direct_user_id;
                if (payload.is_direct_message && data.contains("author") && data["author"].is_object()) {
                    if (data["author"].contains("id") && data["author"]["id"].is_string()) {
                        direct_user_id = data["author"]["id"].get<std::string>();
                    }
                }

                // 提取 parent_id（线程继承父会话）
                std::optional<std::string> parent_id;
                if (data.contains("message_reference") && data["message_reference"].is_object()) {
                    const auto& ref = data["message_reference"];
                    if (ref.contains("channel_id") && ref["channel_id"].is_string()) {
                        parent_id = ref["channel_id"].get<std::string>();
                    }
                }

                // 提取 member.roles[]
                std::vector<std::string> member_role_ids;
                if (data.contains("member") && data["member"].is_object()) {
                    const auto& member = data["member"];
                    if (member.contains("roles") && member["roles"].is_array()) {
                        for (const auto& role : member["roles"]) {
                            if (role.is_string()) {
                                member_role_ids.push_back(role.get<std::string>());
                            }
                        }
                    }
                }

                // 构建路由 — 使用 routing 系统的渠道封装函数
                routing::RouteResolver resolver;
                resolver.set_default_agent_id("main");

                auto route = routing::resolve_discord_bound_conversation_route(
                    resolver,
                    std::optional<std::string>(account_id),
                    payload.guild_id,
                    member_role_ids,
                    payload.is_direct_message,
                    false,  // is_group_dm — 需要从 channel.type 判断
                    direct_user_id,
                    channel_id,
                    parent_id);

                payload.session_key = route.session_key;
            }

            job.queue_key = resolve_discord_inbound_job_queue_key(payload);

            // Aligned with openclaw: runtime fields injected at materialization
            job.runtime.logger         = logger;
            job.runtime.cfg            = cfg;
            job.runtime.account_id     = account_id;

            inbound_worker->enqueue(std::move(job));
        } catch (const std::exception& e) {
            const auto& log = logger ? logger : get_fallback_logger();
            log->error("[discord] message handler failed: {}", e.what());
        } catch (...) {
            const auto& log = logger ? logger : get_fallback_logger();
            log->error("[discord] message handler failed (unknown exception)");
        }
    };

    DiscordMessageHandlerWithLifecycle result;
    result.handler    = std::move(handler);
    // Aligned with openclaw: handler.deactivate = inboundWorker.deactivate
    result.deactivate = [worker = inbound_worker]() { worker->deactivate(); };

    return result;
}

}  // namespace channels
}  // namespace hunterclaw
