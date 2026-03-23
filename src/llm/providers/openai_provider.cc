#include "hunterclaw/llm/provider.h"
#include "hunterclaw/core/logger.h"
#include "hunterclaw/core/result.h"
#include <curl/curl.h>
#include <json/json.h>
#include <functional>
#include <sstream>
#include <string>
#include <chrono>
#include <thread>
namespace hunterclaw {
namespace llm {

// HTTP 响应结构体
struct HttpResponse {
    std::string body;
    long status_code = 0;
    std::map<std::string, std::string> headers;
};

// ============================================================================
// HTTP 工具函数
// ============================================================================

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* response = static_cast<std::string*>(userdata);
    response->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    std::string header(buffer, size * nitems);
    
    auto pos = header.find(':');
    if (pos != std::string::npos) {
        std::string key = header.substr(0, pos);
        std::string value = header.substr(pos + 1);
        // Trim whitespace
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        headers->insert({key, value});
    }
    
    return size * nitems;
}

core::Result<HttpResponse> http_post(
    const std::string& url,
    const std::string& json_body,
    const std::map<std::string, std::string>& headers
) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return core::Result<HttpResponse>::Err("Failed to initialize CURL");
    }
    
    HttpResponse response;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    
    // 设置请求头
    struct curl_slist* curl_headers = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        curl_headers = curl_slist_append(curl_headers, header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    
    curl_slist_free_all(curl_headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        return core::Result<HttpResponse>::Err(std::string("CURL error: ") + curl_easy_strerror(res));
    }
    
    if (response.status_code >= 400) {
        return core::Result<HttpResponse>::Err("HTTP error " + std::to_string(response.status_code) + ": " + response.body);
    }
    
    return core::Result<HttpResponse>::Ok(response);
}

// ============================================================================
// OpenAI Provider 实现
// ============================================================================

class OpenAIProvider : public Provider {
public:
    OpenAIProvider(const ProviderConfig& config) : config_(config) {
        // Logger initialized
        (void)config;
    }
    
    core::Result<GenerateResponse> generate(const GenerateOptions& options) override {
        // 构建请求 JSON
        auto request_json = build_request_json(options);
        
        // 发送请求
        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/json"},
            {"Authorization", "Bearer " + config_.api_key}
        };
        
        for (const auto& [key, value] : config_.headers) {
            headers[key] = value;
        }
        
        std::string url = config_.base_url + "/chat/completions";
        
        // 重试机制
        int retries = 0;
        while (retries < config_.max_retries) {
            auto http_result = http_post(url, request_json, headers);
            
            if (http_result.is_ok()) {
                return parse_response(http_result.get());
            }
            
            // Request failed, will retry
            (void)http_result;
            
            retries++;
            if (retries < config_.max_retries) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.retry_delay_ms * retries)
                );
            }
        }
        
        return core::Result<GenerateResponse>::Err("Max retries exceeded");
    }
    
    core::Result<GenerateResponse> generate_stream(
        const GenerateOptions& options,
        StreamCallback callback
    ) override {
        // ✅ 对齐 OpenClaw: OpenAI SSE 流式处理
        // 对应 OpenClaw: openai.chat.completions.stream()
        auto request_json = build_stream_request_json(options);
        
        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/json"},
            {"Authorization", "Bearer " + config_.api_key},
            {"Accept", "text/event-stream"}
        };
        for (const auto& [key, value] : config_.headers) {
            headers[key] = value;
        }
        
        std::string url = config_.base_url + "/chat/completions";
        GenerateResponse final_response;
        std::string accumulated_text;
        
        // SSE 流式回调
        auto sse_callback = [&](const std::string& chunk) {
            std::istringstream stream(chunk);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.substr(0, 6) == "data: ") {
                    std::string data = line.substr(6);
                    if (data == "[DONE]") {
                        if (callback) callback("", true);
                        break;
                    }
                    try {
                        auto j = Json::Value();
                        Json::CharReaderBuilder reader;
                        std::istringstream ss(data);
                        std::string errs;
                        if (Json::parseFromStream(reader, ss, &j, &errs)) {
                            if (j.isMember("choices") && j["choices"].isArray() && j["choices"].size() > 0) {
                                auto& delta = j["choices"][0]["delta"];
                                if (delta.isMember("content") && delta["content"].isString()) {
                                    std::string text = delta["content"].asString();
                                    accumulated_text += text;
                                    if (callback) callback(text, false);
                                }
                            }
                            // 使用统计
                            if (j.isMember("usage")) {
                                final_response.usage.input_tokens = j["usage"].get("prompt_tokens", 0).asInt();
                                final_response.usage.output_tokens = j["usage"].get("completion_tokens", 0).asInt();
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
                std::string("OpenAI stream error: ") + curl_easy_strerror(res));
        }
        
        final_response.content = accumulated_text;
        final_response.finish_reason = "stop";
        return core::Result<GenerateResponse>::Ok(final_response);
    }
    
    int estimate_tokens(const std::string& text) const override {
        // 简单估算: 1 token ≈ 4 字符
        return static_cast<int>(text.length() / 4);
    }
    
    const ProviderConfig& config() const override {
        return config_;
    }
    
    std::string provider_id() const override {
        return "openai";
    }
    
    ProviderAPI api_type() const override {
        return config_.api_type;
    }
    
    bool supports_tools() const override {
        return config_.supports_tool_calls;
    }
    
    bool supports_streaming() const override {
        return config_.streaming_enabled;
    }
    
private:
    std::string build_stream_request_json(const GenerateOptions& options) {
        // 与 build_request_json 相同，但添加 stream: true
        Json::Value root;
        root["model"] = config_.model;
        root["stream"] = true;  // ✅ 启用 SSE 流式
        
        Json::Value messages_json(Json::arrayValue);
        for (const auto& msg : options.messages) {
            Json::Value msg_json;
            switch (msg.role) {
                case MessageRole::System:    msg_json["role"] = "system"; break;
                case MessageRole::User:      msg_json["role"] = "user"; break;
                case MessageRole::Assistant: msg_json["role"] = "assistant"; break;
                default:                     msg_json["role"] = "user"; break;
            }
            msg_json["content"] = msg.content;
            messages_json.append(msg_json);
        }
        root["messages"] = messages_json;
        if (options.max_tokens.has_value()) root["max_tokens"] = options.max_tokens.value();
        if (options.temperature.has_value()) root["temperature"] = options.temperature.value();
        
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, root);
    }
    
    std::string build_request_json(const GenerateOptions& options) {
        Json::Value root;
        root["model"] = config_.model;
        
        // Messages
        Json::Value messages_json(Json::arrayValue);
        for (const auto& msg : options.messages) {
            Json::Value msg_json;
            
            switch (msg.role) {
                case MessageRole::System:
                    msg_json["role"] = "system";
                    break;
                case MessageRole::User:
                    msg_json["role"] = "user";
                    break;
                case MessageRole::Assistant:
                    msg_json["role"] = "assistant";
                    break;
                case MessageRole::Tool:
                    msg_json["role"] = "tool";
                    msg_json["tool_call_id"] = msg.tool_call_id;
                    msg_json["name"] = msg.name;
                    break;
            }
            
            msg_json["content"] = msg.content;
            
            // Tool calls
            if (!msg.tool_calls.empty()) {
                Json::Value tool_calls_json(Json::arrayValue);
                for (const auto& call : msg.tool_calls) {
                    Json::Value call_json;
                    call_json["id"] = call.id;
                    call_json["type"] = "function";
                    call_json["function"]["name"] = call.name;
                    // 将 JsonValue 转换为 nlohmann::json 再转换为字符串
                    call_json["function"]["arguments"] = call.input.to_json().dump();
                    tool_calls_json.append(call_json);
                }
                msg_json["tool_calls"] = tool_calls_json;
            }
            
            messages_json.append(msg_json);
        }
        root["messages"] = messages_json;
        
        // 参数
        root["max_tokens"] = options.max_tokens.value_or(config_.max_tokens);
        root["temperature"] = options.temperature.value_or(config_.temperature);
        root["top_p"] = options.top_p.value_or(config_.top_p);
        
        // Stop sequences
        if (options.stop_sequences.has_value() && !options.stop_sequences.value().empty()) {
            Json::Value stop_json(Json::arrayValue);
            for (const auto& seq : options.stop_sequences.value()) {
                stop_json.append(seq);
            }
            root["stop"] = stop_json;
        }
        
        // Tools
        if (!options.tools.empty()) {
            Json::Value tools_json(Json::arrayValue);
            for (const auto& tool : options.tools) {
                Json::Value tool_json;
                tool_json["type"] = "function";
                tool_json["function"]["name"] = tool.name;
                tool_json["function"]["description"] = tool.description;
                
                // 将 nlohmann::json 转换为 Json::Value
                // 简单方法: 先转字符串再解析
                nlohmann::json schema_nlohmann = tool.input_schema.to_json();
                std::string schema_str = schema_nlohmann.dump();
                
                Json::CharReaderBuilder reader;
                Json::Value schema_jsoncpp;
                std::string errs;
                std::istringstream schema_stream(schema_str);
                if (Json::parseFromStream(reader, schema_stream, &schema_jsoncpp, &errs)) {
                    tool_json["function"]["parameters"] = schema_jsoncpp;
                }
                
                tools_json.append(tool_json);
            }
            root["tools"] = tools_json;
            
            if (options.tool_choice.has_value()) {
                root["tool_choice"] = options.tool_choice.value();
            }
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
        
        // 提取内容
        const auto& choices = root["choices"];
        if (choices.isArray() && !choices.empty()) {
            const auto& first_choice = choices[0];
            const auto& message = first_choice["message"];
            
            if (message["content"].isString()) {
                response.content = message["content"].asString();
            }
            
            // Tool calls
            const auto& tool_calls = message["tool_calls"];
            if (tool_calls.isArray()) {
                for (const auto& call : tool_calls) {
                    ToolCall tool_call;
                    tool_call.id = call["id"].asString();
                    tool_call.name = call["function"]["name"].asString();
                    
                    std::string args_str = call["function"]["arguments"].asString();
                    // 简单解析 JSON 字符串为 JsonValue (需要根据实际的 JsonValue 实现调整)
                    Json::CharReaderBuilder args_reader;
                    Json::Value args_json;
                    std::string args_errs;
                    std::istringstream args_stream(args_str);
                    if (Json::parseFromStream(args_reader, args_stream, &args_json, &args_errs)) {
                        // 将 Json::Value 转换为 core::JsonValue (假设有转换方法)
                        // tool_call.input = convert_to_core_json(args_json);
                    }
                    
                    response.tool_calls.push_back(tool_call);
                }
            }
        }
        
        // Token 统计
        const auto& usage = root["usage"];
        if (!usage.isNull()) {
            response.input_tokens = usage["prompt_tokens"].asInt();
            response.output_tokens = usage["completion_tokens"].asInt();
        }
        
        return core::Result<GenerateResponse>::Ok(response);
    }
    
    ProviderConfig config_;
};

// ============================================================================
// Factory 函数
// ============================================================================

core::Result<Provider::Ptr> create_openai_provider(const ProviderConfig& config) {
    Provider::Ptr provider = std::make_shared<OpenAIProvider>(config);
    return core::Result<Provider::Ptr>::Ok(provider);
}

} // namespace llm
} // namespace hunterclaw
