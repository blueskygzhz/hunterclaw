#include "hunterclaw/llm/provider.h"
#include "hunterclaw/core/logger.h"
#include "hunterclaw/core/result.h"
#include <curl/curl.h>
#include <json/json.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <functional>

namespace hunterclaw {
namespace llm {

// HTTP 响应结构体
struct HttpResponse {
    std::string body;
    long status_code = 0;
    std::map<std::string, std::string> headers;
};

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
        // Logger initialized
        (void)config;
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
            return core::Result<GenerateResponse>::Err(http_result.error);
        }
        
        return parse_response(http_result.get());
    }
    
    core::Result<GenerateResponse> generate_stream(
        const GenerateOptions& options,
        StreamCallback callback
    ) override {
        // ✅ 对齐 OpenClaw: Anthropic SSE 流式处理
        // 对应 OpenClaw: anthropic.messages.stream()
        auto request_json = build_stream_request_json(options);
        
        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/json"},
            {"x-api-key", config_.api_key},
            {"anthropic-version", "2023-06-01"},
            {"Accept", "text/event-stream"}
        };
        for (const auto& [key, value] : config_.headers) {
            headers[key] = value;
        }
        
        std::string url = config_.base_url + "/messages";
        GenerateResponse final_response;
        std::string accumulated_text;
        
        // SSE 流式回调
        auto sse_callback = [&](const std::string& chunk) {
            // 解析 SSE 行: "data: {...}"
            std::istringstream stream(chunk);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.substr(0, 6) == "data: ") {
                    std::string data = line.substr(6);
                    if (data == "[DONE]") break;
                    try {
                        auto j = nlohmann::json::parse(data);
                        std::string event_type = j.value("type", "");
                        if (event_type == "content_block_delta") {
                            auto& delta = j["delta"];
                            if (delta.value("type", "") == "text_delta") {
                                std::string text = delta.value("text", "");
                                accumulated_text += text;
                                if (callback) {
                                    StreamChunk chunk;
                                    chunk.delta = text;
                                    chunk.is_final = false;
                                    callback(chunk);
                                }
                            }
                        } else if (event_type == "message_stop") {
                            if (callback) {
                                StreamChunk final_chunk;
                                final_chunk.delta = "";
                                final_chunk.is_final = true;
                                callback(final_chunk);
                            }
                        } else if (event_type == "message_delta") {
                            if (j.contains("usage")) {
                                final_response.output_tokens = j["usage"].value("output_tokens", 0);
                            }
                        }
                    } catch (...) {}
                }
            }
        };
        
        // 使用 curl 进行 SSE 请求
        CURL* curl = curl_easy_init();
        if (!curl) return core::Result<GenerateResponse>::Err("Failed to init curl");
        
        struct SseState { std::string buffer; std::function<void(const std::string&)> cb; };
        SseState state{.cb = sse_callback};
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)request_json.size());
        
        struct curl_slist* curl_headers = nullptr;
        for (const auto& [k, v] : headers) {
            curl_headers = curl_slist_append(curl_headers, (k + ": " + v).c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
        
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
                auto* s = static_cast<SseState*>(ud);
                s->buffer.append(ptr, size * nmemb);
                // 按行处理
                size_t pos;
                while ((pos = s->buffer.find('\n')) != std::string::npos) {
                    std::string line = s->buffer.substr(0, pos + 1);
                    s->buffer.erase(0, pos + 1);
                    s->cb(line);
                }
                return size * nmemb;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(curl_headers);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            return core::Result<GenerateResponse>::Err(
                std::string("Anthropic stream error: ") + curl_easy_strerror(res));
        }
        
        final_response.content = accumulated_text;
        return core::Result<GenerateResponse>::Ok(final_response);
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
    std::string build_stream_request_json(const GenerateOptions& options) {
        // 与 build_request_json 相同，但添加 stream: true
        Json::Value root;
        root["model"] = config_.model;
        root["max_tokens"] = options.max_tokens.value_or(config_.max_tokens);
        root["stream"] = true;  // ✅ 启用 SSE 流式
        
        std::string system_message;
        Json::Value messages_json(Json::arrayValue);
        for (const auto& msg : options.messages) {
            if (msg.role == MessageRole::System) {
                system_message = msg.content;
                continue;
            }
            Json::Value msg_json;
            msg_json["role"] = (msg.role == MessageRole::User) ? "user" : "assistant";
            msg_json["content"] = msg.content;
            messages_json.append(msg_json);
        }
        if (!system_message.empty()) root["system"] = system_message;
        root["messages"] = messages_json;
        if (options.temperature.has_value()) root["temperature"] = options.temperature.value();
        
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, root);
    }
    
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
                
                // Convert nlohmann::json to Json::Value
                nlohmann::json schema_nlohmann = tool.input_schema.to_json();
                std::string schema_str = schema_nlohmann.dump();
                Json::CharReaderBuilder reader;
                Json::Value schema_jsoncpp;
                std::string errs;
                std::istringstream schema_stream(schema_str);
                Json::parseFromStream(reader, schema_stream, &schema_jsoncpp, &errs);
                
                tool_json["input_schema"] = schema_jsoncpp;
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
            return core::Result<GenerateResponse>::Err("Failed to parse JSON: " + errs);
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
                    
                    // 将 Json::Value 转换为 JsonValue  
                    std::string input_str = Json::writeString(Json::StreamWriterBuilder(), block["input"]);
                    nlohmann::json input_nlohmann = nlohmann::json::parse(input_str);
                    tool_call.input = core::JsonValue(input_nlohmann);
                    
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
        
        return core::Result<GenerateResponse>::Ok(response);
    }
    
    ProviderConfig config_;
};

core::Result<Provider::Ptr> create_anthropic_provider(const ProviderConfig& config) {
    Provider::Ptr provider = std::make_shared<AnthropicProvider>(config);
    return core::Result<Provider::Ptr>::Ok(provider);
}

} // namespace llm
} // namespace hunterclaw
