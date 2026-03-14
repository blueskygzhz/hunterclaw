#include "hunterclaw/llm/provider.h"
#include "hunterclaw/core/logger.h"
#include <curl/curl.h>
#include <sstream>

namespace hunterclaw {
namespace llm {

// 复用 OpenAI 的 HTTP 工具函数
extern core::Result<HttpResponse> http_post(
    const std::string& url,
    const std::string& json_body,
    const std::map<std::string, std::string>& headers
);

// ============================================================================
// Anthropic Provider 实现
// ============================================================================

class AnthropicProvider : public Provider {
public:
    AnthropicProvider(const ProviderConfig& config) : config_(config) {
        HUNTERCLAW_LOG_INFO("AnthropicProvider initialized: model={}", config.model);
    }
    
    core::Result<GenerateResponse> generate(const GenerateOptions& options) override {
        auto request_json = build_request_json(options);
        
        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/json"},
            {"x-api-key", config_.api_key},
            {"anthropic-version", "2023-06-01"}
        };
        
        for (const auto& [key, value] : config_.headers) {
            headers[key] = value;
        }
        
        std::string url = config_.base_url + "/messages";
        
        auto http_result = http_post(url, request_json, headers);
        if (!http_result.is_ok()) {
            return core::Error(http_result.error().message());
        }
        
        return parse_response(http_result.value());
    }
    
    core::Result<GenerateResponse> generate_stream(
        const GenerateOptions& options,
        StreamCallback callback
    ) override {
        // ✅ Anthropic 流式支持 (SSE)
        // 参考: https://docs.anthropic.com/en/api/messages-streaming
        
        auto request_json = build_request_json(options);
        
        // 解析为 JSON 并添加 stream: true
        Json::CharReaderBuilder reader_builder;
        Json::Value root;
        std::string errs;
        std::istringstream stream(request_json);
        if (!Json::parseFromStream(reader_builder, stream, &root, &errs)) {
            return core::Error("Failed to parse request JSON: " + errs);
        }
        root["stream"] = true;
        
        Json::StreamWriterBuilder builder;
        std::string request_body = Json::writeString(builder, root);
        
        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/json"},
            {"x-api-key", config_.api_key},
            {"anthropic-version", "2023-06-01"}
        };
        
        for (const auto& [key, value] : config_.headers) {
            headers[key] = value;
        }
        
        std::string url = config_.base_url + "/messages";
        
        // ═══════════════════════════════════════════════════════
        // SSE 流式解析 (对齐 OpenClaw)
        // ═══════════════════════════════════════════════════════
        GenerateResponse final_response;
        std::string accumulated_text;
        std::map<std::string, ToolCall> partial_tool_calls;  // tool_index -> ToolCall
        
        auto http_result = http_post_stream(url, request_body, headers, 
            [&](const std::string& sse_line) {
                // 解析 SSE 事件
                if (sse_line.compare(0, 7, "event: ") != 0 && sse_line.compare(0, 6, "data: ") != 0) {
                    return;
                }
                
                std::string event_type;
                std::string data;
                
                if (sse_line.compare(0, 7, "event: ") == 0) {
                    event_type = sse_line.substr(7);
                    return;  // 等待下一行的 data
                } else if (sse_line.compare(0, 6, "data: ") == 0) {
                    data = sse_line.substr(6);
                }
                
                if (data.empty()) return;
                
                // 解析 JSON
                Json::CharReaderBuilder reader;
                Json::Value event_json;
                std::istringstream data_stream(data);
                std::string parse_errs;
                if (!Json::parseFromStream(reader, data_stream, &event_json, &parse_errs)) {
                    HUNTERCLAW_LOG_WARN("Failed to parse SSE JSON: {}", parse_errs);
                    return;
                }
                
                std::string type = event_json["type"].asString();
                
                // ───────────────────────────────────────────────────
                // Anthropic SSE 事件处理 (对齐 API 规范)
                // ───────────────────────────────────────────────────
                
                if (type == "message_start") {
                    // 消息开始 - 提取初始 usage
                    const auto& message = event_json["message"];
                    const auto& usage = message["usage"];
                    if (!usage.isNull()) {
                        final_response.input_tokens = usage["input_tokens"].asInt();
                    }
                    
                } else if (type == "content_block_start") {
                    // 内容块开始
                    const auto& content_block = event_json["content_block"];
                    std::string block_type = content_block["type"].asString();
                    int index = event_json["index"].asInt();
                    
                    if (block_type == "tool_use") {
                        // 工具调用开始
                        ToolCall tool_call;
                        tool_call.id = content_block["id"].asString();
                        tool_call.name = content_block["name"].asString();
                        partial_tool_calls[std::to_string(index)] = tool_call;
                    }
                    
                } else if (type == "content_block_delta") {
                    // 增量内容
                    const auto& delta = event_json["delta"];
                    std::string delta_type = delta["type"].asString();
                    int index = event_json["index"].asInt();
                    
                    if (delta_type == "text_delta") {
                        // 文本增量
                        std::string text = delta["text"].asString();
                        accumulated_text += text;
                        
                        // 流式回调
                        StreamChunk chunk;
                        chunk.delta = text;
                        chunk.is_final = false;
                        callback(chunk);
                        
                    } else if (delta_type == "input_json_delta") {
                        // 工具调用参数增量
                        std::string partial_json = delta["partial_json"].asString();
                        std::string key = std::to_string(index);
                        if (partial_tool_calls.find(key) != partial_tool_calls.end()) {
                            // 累积 JSON 字符串
                            partial_tool_calls[key].input = 
                                core::JsonValue::from_string(partial_json);
                        }
                    }
                    
                } else if (type == "content_block_stop") {
                    // 内容块结束
                    int index = event_json["index"].asInt();
                    std::string key = std::to_string(index);
                    
                    if (partial_tool_calls.find(key) != partial_tool_calls.end()) {
                        // 工具调用完成
                        final_response.tool_calls.push_back(partial_tool_calls[key]);
                        
                        // 发送工具调用通知
                        StreamChunk chunk;
                        chunk.is_tool_call = true;
                        chunk.tool_call = partial_tool_calls[key];
                        callback(chunk);
                    }
                    
                } else if (type == "message_delta") {
                    // 消息元数据更新
                    const auto& usage = event_json["usage"];
                    if (!usage.isNull()) {
                        final_response.output_tokens = usage["output_tokens"].asInt();
                    }
                    
                } else if (type == "message_stop") {
                    // 消息结束
                    StreamChunk chunk;
                    chunk.is_final = true;
                    callback(chunk);
                }
            }
        );
        
        if (!http_result.is_ok()) {
            return core::Error(http_result.error().message());
        }
        
        // 设置最终响应
        final_response.content = accumulated_text;
        
        return final_response;
    }
    
    int estimate_tokens(const std::string& text) const override {
        return static_cast<int>(text.length() / 4);
    }
    
    const ProviderConfig& config() const override { return config_; }
    std::string provider_id() const override { return "anthropic"; }
    ProviderAPI api_type() const override { return config_.api_type; }
    bool supports_tools() const override { return config_.supports_tool_calls; }
    bool supports_streaming() const override { return config_.streaming_enabled; }
    
private:
    std::string build_request_json(const GenerateOptions& options) {
        Json::Value root;
        root["model"] = config_.model;
        root["max_tokens"] = options.max_tokens.value_or(config_.max_tokens);
        
        // System message
        std::string system_message;
        Json::Value messages_json(Json::arrayValue);
        
        for (const auto& msg : options.messages) {
            if (msg.role == MessageRole::System) {
                system_message = msg.content;
                continue;
            }
            
            Json::Value msg_json;
            
            switch (msg.role) {
                case MessageRole::User:
                    msg_json["role"] = "user";
                    break;
                case MessageRole::Assistant:
                    msg_json["role"] = "assistant";
                    break;
                default:
                    continue;
            }
            
            msg_json["content"] = msg.content;
            messages_json.append(msg_json);
        }
        
        if (!system_message.empty()) {
            root["system"] = system_message;
        }
        
        root["messages"] = messages_json;
        
        // Parameters
        if (options.temperature.has_value()) {
            root["temperature"] = options.temperature.value();
        }
        if (options.top_p.has_value()) {
            root["top_p"] = options.top_p.value();
        }
        if (options.top_k.has_value()) {
            root["top_k"] = options.top_k.value();
        }
        
        // Tools (Anthropic format)
        if (!options.tools.empty()) {
            Json::Value tools_json(Json::arrayValue);
            for (const auto& tool : options.tools) {
                Json::Value tool_json;
                tool_json["name"] = tool.name;
                tool_json["description"] = tool.description;
                tool_json["input_schema"] = tool.input_schema.to_json();
                tools_json.append(tool_json);
            }
            root["tools"] = tools_json;
        }
        
        Json::StreamWriterBuilder builder;
        return Json::writeString(builder, root);
    }
    
    core::Result<GenerateResponse> parse_response(const HttpResponse& http_resp) {
        Json::CharReaderBuilder reader_builder;
        Json::Value root;
        std::string errs;
        
        std::istringstream stream(http_resp.body);
        if (!Json::parseFromStream(reader_builder, stream, &root, &errs)) {
            return core::Error("Failed to parse JSON: " + errs);
        }
        
        GenerateResponse response;
        
        // Content blocks
        const auto& content = root["content"];
        if (content.isArray()) {
            for (const auto& block : content) {
                std::string type = block["type"].asString();
                
                if (type == "text") {
                    response.content += block["text"].asString();
                } else if (type == "tool_use") {
                    ToolCall tool_call;
                    tool_call.id = block["id"].asString();
                    tool_call.name = block["name"].asString();
                    tool_call.input = core::JsonValue::from_json(block["input"]);
                    response.tool_calls.push_back(tool_call);
                } else if (type == "thinking") {
                    // Anthropic thinking blocks
                    response.content += "[Thinking] " + block["thinking"].asString() + "\n";
                }
            }
        }
        
        // Token usage
        const auto& usage = root["usage"];
        if (!usage.isNull()) {
            response.input_tokens = usage["input_tokens"].asInt();
            response.output_tokens = usage["output_tokens"].asInt();
            
            if (usage.isMember("cache_read_input_tokens")) {
                response.cache_read_tokens = usage["cache_read_input_tokens"].asInt();
            }
            if (usage.isMember("cache_creation_input_tokens")) {
                response.cache_write_tokens = usage["cache_creation_input_tokens"].asInt();
            }
        }
        
        return response;
    }
    
    ProviderConfig config_;
};

core::Result<Provider::Ptr> create_anthropic_provider(const ProviderConfig& config) {
    return std::make_shared<AnthropicProvider>(config);
}

} // namespace llm
} // namespace hunterclaw
