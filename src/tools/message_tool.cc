// message_tool.cc
// Strictly aligned with OpenClaw: src/agents/tools/message-tool.ts
//
// Tool: message
// Description: Send, delete, and manage messages via channel plugins.

#include "hunterclaw/tools/message_tool.h"
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <set>
#include <vector>
#include <algorithm>
#include <cmath>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

// ============================================================================
// 🎯 对齐 OpenClaw: CHANNEL_MESSAGE_ACTION_NAMES (all supported actions)
// ============================================================================
static const std::vector<std::string> ALL_MESSAGE_ACTIONS = {
    // Core send actions
    "send", "sendWithEffect", "sendAttachment", "reply", "thread-reply", "broadcast",
    // Message management
    "delete", "edit", "pin", "unpin", "fetch",
    // Reactions
    "react", "react-remove",
    // Polls
    "poll", "poll-vote", "poll-close",
    // Threads
    "thread-create", "thread-list", "thread-archive", "thread-unarchive",
    // Events
    "event-create", "event-list", "event-delete", "event-edit",
    // Moderation
    "ban", "kick", "timeout", "unban", "warn",
    // Roles
    "role-add", "role-remove", "role-list",
    // Channels
    "channel-create", "channel-delete", "channel-edit", "channel-list",
    // Presence
    "presence",
    // Stickers
    "sticker",
    // Search
    "search",
    // Typing
    "typing",
    // Read receipts
    "mark-read",
    // Forwarding
    "forward",
    // Pinned messages
    "pinned-list",
    // Members
    "members-list",
    // Invites
    "invite-create", "invite-list", "invite-delete",
    // Webhooks
    "webhook-send",
};

// 🎯 对齐 OpenClaw: EXPLICIT_TARGET_ACTIONS
static const std::set<std::string> EXPLICIT_TARGET_ACTIONS = {
    "send", "sendWithEffect", "sendAttachment", "reply", "thread-reply", "broadcast"
};

// ============================================================================
// Helpers
// ============================================================================

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string read_str(const json& params, const std::string& key) {
    auto it = params.find(key);
    if (it == params.end() || !it->is_string()) return "";
    return trim(it->get<std::string>());
}

// 🎯 对齐 OpenClaw: stripReasoningTagsFromText
static std::string strip_reasoning_tags(const std::string& text) {
    std::string result = text;
    // Remove <think>...</think> tags
    size_t pos = 0;
    while ((pos = result.find("<think>", pos)) != std::string::npos) {
        size_t end = result.find("</think>", pos);
        if (end == std::string::npos) break;
        result.erase(pos, end - pos + 8);
    }
    return result;
}

// ============================================================================
// MessageTool
// ============================================================================

class MessageTool : public Tool {
public:
    explicit MessageTool(MessageToolOptions opts)
        : opts_(std::move(opts)) {}

    // 🎯 对齐 OpenClaw: name: "message"
    std::string name() const override { return "message"; }

    // 🎯 对齐 OpenClaw: description (dynamic based on channel)
    std::string description() const override {
        std::string base = "Send, delete, and manage messages via channel plugins.";
        if (!opts_.current_channel_provider.empty()) {
            base += " Current channel (" + opts_.current_channel_provider + ").";
        }
        base += " Supports actions: send, delete, react, poll, pin, threads, and more.";
        return base;
    }

    // 🎯 对齐 OpenClaw: MessageToolSchema (flattened, all params)
    Schema parameters() const override {
        Schema schema;
        schema.type = "object";

        auto make_str = [](const std::string& desc) {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            p["description"] = desc;
            return p;
        };
        auto make_num = [](const std::string& desc) {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "number";
            p["description"] = desc;
            return p;
        };
        auto make_bool = [](const std::string& desc) {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "boolean";
            p["description"] = desc;
            return p;
        };

        // action (required)
        {
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "string";
            p["description"] = "Message action to perform";
            core::JsonValue e = core::JsonValue::array();
            for (const auto& a : ALL_MESSAGE_ACTIONS) e.push_back(a);
            p["enum"] = e;
            schema.properties["action"] = p;
        }

        // 🎯 对齐 OpenClaw: buildRoutingSchema
        schema.properties["channel"]    = make_str("Channel id or name");
        schema.properties["target"]     = make_str("Target channel/user id or name");
        schema.properties["accountId"]  = make_str("Account ID override");
        schema.properties["dryRun"]     = make_bool("Dry run (don't actually send)");

        // 🎯 对齐 OpenClaw: buildSendSchema
        schema.properties["message"]     = make_str("Message text to send");
        schema.properties["text"]        = make_str("Alias for message");
        schema.properties["content"]     = make_str("Alias for message");
        schema.properties["effectId"]    = make_str("Message effect name/id for sendWithEffect");
        schema.properties["effect"]      = make_str("Alias for effectId");
        schema.properties["media"]       = make_str("Media URL or local path");
        schema.properties["filename"]    = make_str("Filename for attachment");
        schema.properties["buffer"]      = make_str("Base64 payload for attachments");
        schema.properties["contentType"] = make_str("Content type for attachment");
        schema.properties["mimeType"]    = make_str("MIME type alias");
        schema.properties["caption"]     = make_str("Caption for media");
        schema.properties["path"]        = make_str("File path for attachment");
        schema.properties["filePath"]    = make_str("Alias for path");
        schema.properties["replyTo"]     = make_str("Message ID to reply to");
        schema.properties["threadId"]    = make_str("Thread ID");
        schema.properties["asVoice"]     = make_bool("Send audio as voice message");
        schema.properties["silent"]      = make_bool("Send silently");
        schema.properties["quoteText"]   = make_str("Quote text for Telegram reply_parameters");
        schema.properties["bestEffort"]  = make_bool("Best effort delivery");
        schema.properties["gifPlayback"] = make_bool("GIF playback mode");

        // 🎯 对齐 OpenClaw: buildReactionSchema
        schema.properties["messageId"]       = make_str("Target message id for reaction");
        schema.properties["message_id"]      = make_str("snake_case alias of messageId");
        schema.properties["emoji"]           = make_str("Emoji for reaction");
        schema.properties["remove"]          = make_bool("Remove reaction");
        schema.properties["targetAuthor"]    = make_str("Target author for reaction");
        schema.properties["targetAuthorUuid"]= make_str("Target author UUID");
        schema.properties["groupId"]         = make_str("Group ID");

        // 🎯 对齐 OpenClaw: buildFetchSchema
        schema.properties["limit"]           = make_num("Max messages to fetch");
        schema.properties["before"]          = make_str("Fetch messages before this ID");
        schema.properties["after"]           = make_str("Fetch messages after this ID");
        schema.properties["around"]          = make_str("Fetch messages around this ID");
        schema.properties["fromMe"]          = make_bool("Fetch only my messages");
        schema.properties["includeArchived"] = make_bool("Include archived messages");

        // 🎯 对齐 OpenClaw: buildPollSchema
        schema.properties["pollId"]          = make_str("Poll ID");
        schema.properties["pollOptionId"]    = make_str("Poll answer id to vote for");
        schema.properties["pollOptionIndex"] = make_num("1-based poll option number");
        schema.properties["question"]        = make_str("Poll question");
        schema.properties["options"]         = []{
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "array";
            p["description"] = "Poll options";
            core::JsonValue items = core::JsonValue::object();
            items["type"] = "string";
            p["items"] = items;
            return p;
        }();
        schema.properties["multiSelect"]     = make_bool("Allow multiple selections");
        schema.properties["anonymous"]       = make_bool("Anonymous poll");
        schema.properties["closesAt"]        = make_str("Poll close time (ISO 8601)");

        // 🎯 对齐 OpenClaw: buildChannelTargetSchema
        schema.properties["channelId"]       = make_str("Channel id filter");
        schema.properties["guildId"]         = make_str("Guild/server ID");
        schema.properties["userId"]          = make_str("User ID");
        schema.properties["authorId"]        = make_str("Author ID");
        schema.properties["roleId"]          = make_str("Role ID");
        schema.properties["participant"]     = make_str("Participant ID");

        // 🎯 对齐 OpenClaw: buildThreadSchema
        schema.properties["threadName"]      = make_str("Thread name");
        schema.properties["autoArchiveMin"]  = make_num("Auto-archive minutes");
        schema.properties["appliedTags"]     = []{
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "array";
            p["description"] = "Applied tags for thread";
            core::JsonValue items = core::JsonValue::object();
            items["type"] = "string";
            p["items"] = items;
            return p;
        }();

        // 🎯 对齐 OpenClaw: buildEventSchema
        schema.properties["query"]           = make_str("Search query");
        schema.properties["eventName"]       = make_str("Event name");
        schema.properties["eventType"]       = make_str("Event type");
        schema.properties["startTime"]       = make_str("Event start time (ISO 8601)");
        schema.properties["endTime"]         = make_str("Event end time (ISO 8601)");
        schema.properties["desc"]            = make_str("Event description");
        schema.properties["location"]        = make_str("Event location");
        schema.properties["durationMin"]     = make_num("Event duration in minutes");
        schema.properties["until"]           = make_str("Recurrence end time");

        // 🎯 对齐 OpenClaw: buildModerationSchema
        schema.properties["reason"]          = make_str("Moderation reason");
        schema.properties["deleteDays"]      = make_num("Days of messages to delete on ban");

        // 🎯 对齐 OpenClaw: buildGatewaySchema
        schema.properties["gatewayUrl"]      = make_str("Gateway URL override");
        schema.properties["gatewayToken"]    = make_str("Gateway token override");
        schema.properties["timeoutMs"]       = make_num("Request timeout in milliseconds");

        // 🎯 对齐 OpenClaw: buildChannelManagementSchema
        schema.properties["name"]            = make_str("Channel/role name");
        schema.properties["type"]            = make_num("Channel type");
        schema.properties["parentId"]        = make_str("Parent category ID");
        schema.properties["topic"]           = make_str("Channel topic");
        schema.properties["position"]        = make_num("Channel position");
        schema.properties["nsfw"]            = make_bool("NSFW channel");
        schema.properties["rateLimitPerUser"]= make_num("Rate limit per user (seconds)");
        schema.properties["categoryId"]      = make_str("Category ID");
        schema.properties["clearParent"]     = make_bool("Clear parent/category");

        // 🎯 对齐 OpenClaw: buildPresenceSchema
        schema.properties["activityType"]    = make_str("Activity type: playing, streaming, listening, watching, competing, custom");
        schema.properties["activityName"]    = make_str("Activity name");
        schema.properties["activityUrl"]     = make_str("Streaming URL");
        schema.properties["activityState"]   = make_str("State text");
        schema.properties["status"]          = make_str("Bot status: online, dnd, idle, invisible");

        // 🎯 对齐 OpenClaw: buildStickerSchema
        schema.properties["emojiName"]       = make_str("Emoji name for sticker");
        schema.properties["stickerId"]       = []{
            core::JsonValue p = core::JsonValue::object();
            p["type"] = "array";
            p["description"] = "Sticker IDs";
            core::JsonValue items = core::JsonValue::object();
            items["type"] = "string";
            p["items"] = items;
            return p;
        }();
        schema.properties["stickerName"]     = make_str("Sticker name");
        schema.properties["stickerDesc"]     = make_str("Sticker description");
        schema.properties["stickerTags"]     = make_str("Sticker tags");

        schema.required = {"action"};
        return schema;
    }

    // 🎯 对齐 OpenClaw: execute
    ToolResult execute(const core::JsonValue& params_val) override {
        try {
            json params = params_val.to_json();

            // 🎯 对齐 OpenClaw: strip reasoning tags from text fields
            for (const auto& field : {"text", "content", "message", "caption"}) {
                if (params.contains(field) && params[field].is_string()) {
                    params[field] = strip_reasoning_tags(params[field].get<std::string>());
                }
            }

            std::string action = read_str(params, "action");
            if (action.empty())
                return make_error("action is required");

            // 🎯 对齐 OpenClaw: requireExplicitTarget check
            if (opts_.require_explicit_target && EXPLICIT_TARGET_ACTIONS.count(action)) {
                bool has_target =
                    (!read_str(params, "target").empty()) ||
                    (!read_str(params, "channelId").empty()) ||
                    (params.contains("targets") && params["targets"].is_array() &&
                     !params["targets"].empty());
                if (!has_target) {
                    return make_error(
                        "Explicit message target required for this run. "
                        "Provide target/targets (and channel when needed).");
                }
            }

            // 🎯 对齐 OpenClaw: resolve accountId
            std::string account_id = read_str(params, "accountId");
            if (account_id.empty()) account_id = opts_.agent_account_id;

            // 🎯 对齐 OpenClaw: resolve channel context
            std::string channel = read_str(params, "channel");
            if (channel.empty()) channel = opts_.current_channel_provider;

            std::string target = read_str(params, "target");
            if (target.empty()) target = opts_.current_channel_id;

            // 🎯 对齐 OpenClaw: runMessageAction dispatch
            return dispatch_action(action, params, channel, target, account_id);

        } catch (const std::exception& e) {
            return make_error(std::string(e.what()));
        }
    }

private:
    MessageToolOptions opts_;

    ToolResult dispatch_action(
        const std::string& action,
        const json& params,
        const std::string& channel,
        const std::string& target,
        const std::string& account_id
    ) {
        // 🎯 对齐 OpenClaw: runMessageAction → channel plugin dispatch
        // Build the message action request
        json request = {
            {"action",    action},
            {"channel",   channel},
            {"target",    target},
            {"accountId", account_id},
            {"params",    params},
        };

        // Add session context
        if (!opts_.agent_session_key.empty())
            request["sessionKey"] = opts_.agent_session_key;
        if (!opts_.current_thread_ts.empty())
            request["threadTs"] = opts_.current_thread_ts;
        if (!opts_.current_message_id.empty())
            request["currentMessageId"] = opts_.current_message_id;
        if (!opts_.reply_to_mode.empty())
            request["replyToMode"] = opts_.reply_to_mode;
        if (!opts_.requester_sender_id.empty())
            request["requesterSenderId"] = opts_.requester_sender_id;

        // TODO: call the actual channel plugin via GatewayClient
        // For now, return a structured response indicating the action was dispatched
        json result = {
            {"ok",      true},
            {"action",  action},
            {"channel", channel},
            {"target",  target},
            {"stub",    true},
            {"message", "Message action dispatched (inject GatewayClient for real delivery)"},
        };

        // For send actions, include the message text in the result
        std::string msg_text;
        for (const auto& field : {"message", "text", "content"}) {
            if (params.contains(field) && params[field].is_string()) {
                msg_text = params[field].get<std::string>();
                break;
            }
        }
        if (!msg_text.empty()) result["text"] = msg_text;

        return make_json_result(result);
    }

    ToolResult make_error(const std::string& msg) const {
        ToolResult result;
        result.success = false;
        result.error = msg;
        ToolResultContent content;
        content.type = ToolResultContent::Type::Text;
        content.text = "Error: " + msg;
        result.content.push_back(content);
        return result;
    }

    ToolResult make_json_result(const json& data) const {
        ToolResult result;
        result.success = true;
        ToolResultContent content;
        content.type = ToolResultContent::Type::Json;
        content.text = data.dump();
        result.content.push_back(content);
        return result;
    }
};

// ============================================================================
// Factory
// ============================================================================

std::shared_ptr<Tool> create_message_tool(const MessageToolOptions& opts) {
    return std::make_shared<MessageTool>(opts);
}

} // namespace tools
} // namespace hunterclaw
