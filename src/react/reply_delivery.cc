#include "hunterclaw/react/reply_delivery.h"
#include "hunterclaw/react/execution_guard.h"
#include "hunterclaw/tools/tool_hooks.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <regex>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace react {

namespace {
auto log() {
    static auto logger = spdlog::get("react");
    if (!logger) logger = spdlog::stdout_logger_mt("react");
    return logger;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    auto end = s.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

bool has_media(const ReplyPayload& p) {
    return p.media_url.has_value() || !p.media_urls.empty();
}

bool is_single_use_mode(ReplyToMode mode) {
    return mode == ReplyToMode::First;
}

// 🎯 reply_payload_sending hook：ReplyPayload ↔ JSON 轻量序列化（核心字段）。
nlohmann::json reply_payload_to_json(const ReplyPayload& p) {
    nlohmann::json j;
    j["text"] = p.text;
    j["isError"] = p.is_error;
    j["isReasoning"] = p.is_reasoning;
    j["isCompactionNotice"] = p.is_compaction_notice;
    if (p.media_url.has_value()) j["mediaUrl"] = *p.media_url;
    if (!p.media_urls.empty()) j["mediaUrls"] = p.media_urls;
    j["audioAsVoice"] = p.audio_as_voice;
    if (p.reply_to_id.has_value()) j["replyToId"] = *p.reply_to_id;
    j["replyToCurrent"] = p.reply_to_current;
    j["interactive"] = p.interactive;
    return j;
}

ReplyPayload reply_payload_from_json(const nlohmann::json& j, const ReplyPayload& base) {
    ReplyPayload p = base;  // 未提供字段沿用原值
    if (j.contains("text") && j["text"].is_string()) p.text = j["text"].get<std::string>();
    if (j.contains("isError") && j["isError"].is_boolean()) p.is_error = j["isError"].get<bool>();
    if (j.contains("mediaUrl") && j["mediaUrl"].is_string()) p.media_url = j["mediaUrl"].get<std::string>();
    if (j.contains("mediaUrls") && j["mediaUrls"].is_array())
        p.media_urls = j["mediaUrls"].get<std::vector<std::string>>();
    if (j.contains("audioAsVoice") && j["audioAsVoice"].is_boolean()) p.audio_as_voice = j["audioAsVoice"].get<bool>();
    if (j.contains("replyToId") && j["replyToId"].is_string()) p.reply_to_id = j["replyToId"].get<std::string>();
    if (j.contains("interactive") && j["interactive"].is_boolean()) p.interactive = j["interactive"].get<bool>();
    return p;
}
} // anon

// ============================================================================
// 对齐 reply-delivery.ts:13-59: normalizeReplyPayloadDirectives
// ============================================================================
NormalizedDirectiveResult normalize_reply_payload_directives(
    const ReplyPayload& payload,
    const std::optional<std::string>& current_message_id,
    const std::string& silent_token,
    bool trim_leading_whitespace,
    ReplyDirectiveParseMode parse_mode
) {
    std::string source_text = payload.text;

    // Determine if we should parse
    bool should_parse = (parse_mode == ReplyDirectiveParseMode::Always) ||
        (parse_mode == ReplyDirectiveParseMode::Auto &&
         (source_text.find("[[") != std::string::npos ||
          source_text.find("MEDIA:") != std::string::npos ||
          source_text.find(silent_token) != std::string::npos));

    DirectiveParseResult parsed;
    bool did_parse = false;
    if (should_parse) {
        parsed = StreamingDirectiveAccumulator::parse_chunk(source_text, silent_token);
        did_parse = true;
    }

    std::string text = did_parse ? (parsed.text.empty() ? "" : parsed.text) : source_text;
    if (trim_leading_whitespace && !text.empty()) {
        auto start = text.find_first_not_of(" \t\n\r");
        text = (start == std::string::npos) ? "" : text.substr(start);
    }

    // Merge media
    auto media_urls = payload.media_urls.empty() && did_parse ?
        parsed.media_urls : payload.media_urls;
    auto media_url = payload.media_url.has_value() ? payload.media_url :
        (did_parse && parsed.media_url.has_value() ? parsed.media_url :
         (!media_urls.empty() ? std::optional<std::string>(media_urls[0]) : std::nullopt));

    ReplyPayload result = payload;
    result.text = text;
    result.media_urls = media_urls;
    result.media_url = media_url;
    if (!payload.reply_to_id.has_value() && did_parse && parsed.reply_to_id.has_value()) {
        result.reply_to_id = parsed.reply_to_id;
    }
    if (!payload.reply_to_current && did_parse && parsed.reply_to_current) {
        result.reply_to_current = true;
    }
    result.audio_as_voice = payload.audio_as_voice || (did_parse && parsed.audio_as_voice);

    return {result, did_parse ? parsed.is_silent : false};
}

// ============================================================================
// 对齐 reply-payloads-base.ts:71-76: applyReplyTagsToPayload
// ============================================================================
ReplyPayload apply_reply_tags_to_payload(
    const ReplyPayload& payload,
    const std::optional<std::string>& current_message_id
) {
    ReplyPayload result = payload;
    // Parse [[reply:*]] tags from text
    if (!result.text.empty() && result.text.find("[[") != std::string::npos) {
        auto parsed = StreamingDirectiveAccumulator::parse_chunk(result.text, "NO_REPLY");
        result.text = parsed.text;
        if (!result.reply_to_id.has_value() && parsed.reply_to_id.has_value()) {
            result.reply_to_id = parsed.reply_to_id;
        }
        if (parsed.reply_to_current) result.reply_to_current = true;
    }
    // Resolve replyToCurrent → actual ID
    if (result.reply_to_current && !result.reply_to_id.has_value() && current_message_id.has_value()) {
        result.reply_to_id = *current_message_id;
    }
    return result;
}

// ============================================================================
// 对齐 reply-delivery.ts:61-140: createBlockReplyDeliveryHandler
// ============================================================================
std::function<void(const ReplyPayload&)> create_block_reply_delivery_handler(
    const BlockReplyDeliveryParams& params
) {
    return [params](const ReplyPayload& payload) {
        // Step 1: normalize streaming text
        NormalizedText norm;
        norm.text = payload.text;
        norm.skip = false;
        if (params.normalize_streaming_text) {
            norm = params.normalize_streaming_text(payload);
        }
        if (norm.skip && !has_media(payload)) return;

        // Step 2: apply reply tags
        ReplyPayload tag_input = payload;
        tag_input.text = norm.text.value_or("");
        if (!tag_input.media_url.has_value() && !tag_input.media_urls.empty()) {
            tag_input.media_url = tag_input.media_urls[0];
        }
        if (!tag_input.reply_to_id.has_value()) {
            if (payload.reply_to_current != false) {
                tag_input.reply_to_id = params.current_message_id;
            }
        }
        ReplyPayload tagged = apply_reply_tags_to_payload(
            tag_input, params.current_message_id);

        // Step 3: renderable check
        if (!is_renderable_payload(tagged) && !payload.audio_as_voice) return;

        // Step 4: directive normalization
        auto normalized = normalize_reply_payload_directives(
            tagged, params.current_message_id, "NO_REPLY", true,
            ReplyDirectiveParseMode::Auto);

        // Step 5: media normalization
        ReplyPayload media_normalized = normalized.payload;
        if (params.normalize_media_paths && has_media(media_normalized)) {
            try {
                media_normalized = params.normalize_media_paths(media_normalized);
            } catch (...) {}
        }

        // Step 6: apply replyToMode
        ReplyPayload block_payload = media_normalized;
        if (params.apply_reply_to_mode) {
            block_payload = params.apply_reply_to_mode(media_normalized);
        }

        // Step 7: skip empty (unless audioAsVoice)
        if (block_payload.text.empty() && !has_media(block_payload) &&
            !block_payload.audio_as_voice) return;
        if (normalized.is_silent && !has_media(block_payload)) return;

        // Step 7.5: Plugin hook — reply_payload_sending (modifying, before delivery)
        // 🎯 对齐 OpenClaw runReplyPayloadSending：投递前顺序修改/取消规范化 payload。
        {
            auto* hook_mgr = tools::get_global_hook_manager();
            if (hook_mgr && hook_mgr->has_hooks(plugins::PluginHookName::REPLY_PAYLOAD_SENDING)) {
                plugins::PluginHookReplyPayloadSendingEvent rps_event;
                rps_event.payload_json = reply_payload_to_json(block_payload).dump();
                rps_event.kind = "block";// block streaming delivery
                plugins::PluginHookMessageContext rps_ctx;
                try {
                    auto rps_result = hook_mgr->run_reply_payload_sending(rps_event, rps_ctx);
                    if (rps_result) {
                        if (rps_result->cancel) {
                            log()->info("[hooks] reply_payload_sending cancelled delivery{}",
                                rps_result->reason ? (" (" + *rps_result->reason + ")") : std::string{});
                            return;  // ← 取消投递
                        }
                        if (rps_result->payload_json) {
                            try {
                                auto parsed = nlohmann::json::parse(*rps_result->payload_json);
                                block_payload = reply_payload_from_json(parsed, block_payload);
                            } catch (const std::exception& e) {
                                log()->warn("reply_payload_sending: invalid payload JSON: {}", e.what());
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    log()->warn("reply_payload_sending hook failed: {}", e.what());
                }
                // 修改后重新做空检查
                if (block_payload.text.empty() && !has_media(block_payload) &&
                    !block_payload.audio_as_voice) return;
            }
        }

        // Step 8: typing signal
        if (!block_payload.text.empty() && params.signal_text_delta) {
            try { params.signal_text_delta(block_payload.text); } catch (...) {}
        }

        // Step 9: route to pipeline or direct send
        if (params.block_streaming_enabled && params.pipeline_enqueue) {
            params.pipeline_enqueue(block_payload);
        } else if (params.block_streaming_enabled && params.on_block_reply) {
            if (params.directly_sent_block_keys) {
                params.directly_sent_block_keys->insert(
                    create_block_reply_content_key(block_payload));
            }
            params.on_block_reply(block_payload);
        } else if (has_media(block_payload) && params.on_block_reply) {
            if (params.directly_sent_block_keys) {
                params.directly_sent_block_keys->insert(
                    create_block_reply_content_key(block_payload));
            }
            ReplyPayload media_only = block_payload;
            media_only.text = "";
            params.on_block_reply(media_only);
        }
    };
}

// ============================================================================
// 对齐 reply-reference.ts:17-64: createReplyReferencePlanner
// ============================================================================
ReplyReferencePlanner create_reply_reference_planner(
    ReplyToMode reply_to_mode,
    const std::optional<std::string>& existing_id,
    const std::optional<std::string>& start_id,
    bool allow_reference,
    bool initial_has_replied
) {
    auto replied = std::make_shared<bool>(initial_has_replied);

    auto use_fn = [reply_to_mode, existing_id, start_id, allow_reference, replied]()
        -> std::optional<std::string> {
        if (!allow_reference) return std::nullopt;
        if (reply_to_mode == ReplyToMode::Off) return std::nullopt;
        auto id = existing_id.has_value() ? existing_id : start_id;
        if (!id.has_value() || id->empty()) return std::nullopt;
        if (reply_to_mode == ReplyToMode::All) {
            *replied = true;
            return id;
        }
        if (is_single_use_mode(reply_to_mode) && *replied) return std::nullopt;
        *replied = true;
        return id;
    };

    auto mark_sent_fn = [replied]() { *replied = true; };
    auto has_replied_fn = [replied]() -> bool { return *replied; };

    return {use_fn, mark_sent_fn, has_replied_fn};
}

// ============================================================================
// 对齐 reply-payloads-dedupe.ts:119-161: shouldSuppressMessagingToolReplies
// ============================================================================
bool should_suppress_messaging_tool_replies(
    const std::optional<std::string>& message_provider,
    const std::vector<MessagingToolSend>& sent_targets,
    const std::optional<std::string>& originating_to,
    const std::optional<std::string>& account_id
) {
    if (!message_provider.has_value() || message_provider->empty()) return false;
    auto provider = to_lower(trim(*message_provider));
    if (provider.empty()) return false;

    if (!originating_to.has_value() || originating_to->empty()) return false;
    auto origin_target = to_lower(trim(*originating_to));
    if (origin_target.empty()) return false;

    auto origin_account = account_id.has_value() ? to_lower(trim(*account_id)) : "";

    if (sent_targets.empty()) return false;

    for (const auto& target : sent_targets) {
        // Resolve target provider
        auto target_provider = target.provider.has_value() ?
            to_lower(trim(*target.provider)) : "";
        if (target_provider.empty() || target_provider == "message") {
            target_provider = provider;
        }
        if (target_provider != provider) continue;

        // Normalize target key
        auto target_key = to_lower(trim(target.to));
        if (target_key.empty()) continue;

        // Account check
        auto target_account = target.account_id.has_value() ?
            to_lower(trim(*target.account_id)) : "";
        if (!origin_account.empty() && !target_account.empty() &&
            origin_account != target_account) continue;

        // Target match
        if (target_key == origin_target) return true;
    }
    return false;
}

// ============================================================================
// 对齐 agents/usage.ts:196-234: deriveSessionTotalTokens
// OC: SessionEntry.totalTokens 是 prompt/context 快照，只含 input tokens
// 不含 output tokens，不做 contextTokens 截断
// ============================================================================
std::optional<int64_t> derive_session_total_tokens(
    int64_t input_tokens,
    int64_t output_tokens,
    std::optional<int> context_tokens
) {
    // 对齐 OC: derivePromptTokens = input + cacheRead + cacheWrite
    // HC 没有 cacheRead/cacheWrite 分离，input_tokens 已包含 cache tokens
    // output_tokens 不计入 totalTokens（OC 明确排除 completion tokens）
    (void)output_tokens;
    (void)context_tokens;  // OC: "Keep this value unclamped; display layers are responsible"
    if (input_tokens <= 0) return std::nullopt;
    return input_tokens;
}

} // namespace react
} // namespace hunterclaw
