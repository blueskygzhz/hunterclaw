#include "hunterclaw/llm/provider.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/core/logger.h"
#include "hunterclaw/core/result.h"
#include <json/json.h>
#include <functional>
#include <sstream>
#include <string>
#include <chrono>
#include <thread>

namespace hunterclaw {
namespace llm {

// ============================================================================
// OpenAI Provider 实现 (使用 infra::HttpClient，无 curl 依赖)
// ============================================================================

class OpenAIProvider : public Provider {
public:
    OpenAIProvider(const ProviderConfig& config)
        : config_(config), http_client_(infra::HttpClient::create()) {
    }
    
    core::Result<GenerateResponse> generate(const GenerateOptions& options) override {
        auto request_json = build_request_json(options);
        
        infra::HttpRequestOptions http_opts;
        http_opts.body = request_json;
        http_opts.timeout_ms = 60000;  // 60s for LLM calls
        http_opts.headers = {
            {"Content-Type", "application/json"},
            {"Authorization", "Bearer " + config_.api_key}
        };
        for (const auto& [key, value] : config_.headers) {
            http_opts.headers[key] = value;
        }
        
        std::string url = config_.base_url + "/chat/completions";
        
        // 重试机制
        int retries = 0;
        while (retries < config_.max_retries) {
            auto http_result = http_client_->post(url, http_opts);
            
            if (http_result.is_ok()) {
                return parse_response(http_result.get());
            }
            
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
        auto request_json = build_stream_request_json(options);
        
        infra::HttpRequestOptions http_opts;
        http_opts.body = request_json;
        http_opts.timeout_ms = 120000;  // 120s for streaming
        http_opts.headers = {
            {"Content-Type", "application/json"},
            {"Authorization", "Bearer " + config_.api_key},
            {"Accept", "text/event-stream"}
        };
        for (const auto& [key, value] : config_.headers) {
            http_opts.headers[key] = value;
        }
        
        std::string url = config_.base_url + "/chat/completions";
        GenerateResponse final_response;
        std::string accumulated_text;
        
        // SSE 行回调
        auto line_callback = [&](const std::string& line) {
            // 去除尾部换行
            std::string trimmed = line;
            while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
                trimmed.pop_back();
            }
            
            if (trimmed.substr(0, 6) == "data: ") {
                std::string data = trimmed.substr(6);
                if (data == "[DONE]") {
                    if (callback) {
                        StreamChunk final_chunk;
                        final_chunk.delta = "";
                        final_chunk.is_final = true;
                        callback(final_chunk);
                    }
                    return;
                }
                try {
                    Json::Value j;
                    Json::CharReaderBuilder reader;
                    std::istringstream ss(data);
                    std::string errs;
                    if (Json::parseFromStream(reader, ss, &j, &errs)) {
                        if (j.isMember("choices") && j["choices"].isArray() && j["choices"].size() > 0) {
                            auto& delta = j["choices"][0]["delta"];
                            if (delta.isMember("content") && delta["content"].isString()) {
                                std::string text = delta["content"].asString();
                                accumulated_text += text;
                                if (callback) {
                                    StreamChunk chunk;
                                    chunk.delta = text;
                                    chunk.is_final = false;
                                    callback(chunk);
                                }
                            }
                        }
                        if (j.isMember("usage")) {
                            final_response.input_tokens = j["usage"].get("prompt_tokens", 0).asInt();
                            final_response.output_tokens = j["usage"].get("completion_tokens", 0).asInt();
                        }
                    }
                } catch (...) {}
            }
        };
        
        auto result = http_client_->post_stream(url, http_opts, line_callback);
        if (!result.is_ok()) {
            return core::Result<GenerateResponse>::Err(
                std::string("OpenAI stream error: ") + result.error);
        }
        
        final_response.content = accumulated_text;
        return core::Result<GenerateResponse>::Ok(final_response);
    }
    
    int estimate_tokens(const std::string& text) const override {
        return static_cast<int>(text.length() / 4);
    }
    
    const ProviderConfig& config() const override { return config_; }
    std::string provider_id() const override { return "openai"; }
    ProviderAPI api_type() const override { return config_.api_type; }
    bool supports_tools() const override { return config_.supports_tool_calls; }
    bool supports_streaming() const override { return config_.streaming_enabled; }
    
private:
    std::string build_stream_request_json(const GenerateOptions& options) {
        Json::Value root;
        root["model"] = config_.model;
        root["stream"] = true;
        
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
        
        Json::Value messages_json(Json::arrayValue);
        for (const auto& msg : options.messages) {
            Json::Value msg_json;
            switch (msg.role) {
                case MessageRole::System:    msg_json["role"] = "system"; break;
                case MessageRole::User:      msg_json["role"] = "user"; break;
                case MessageRole::Assistant: msg_json["role"] = "assistant"; break;
                case MessageRole::Tool:
                    msg_json["role"] = "tool";
                    msg_json["tool_call_id"] = msg.tool_call_id;
                    msg_json["name"] = msg.name;
                    break;
            }
            msg_json["content"] = msg.content;
            
            if (!msg.tool_calls.empty()) {
                Json::Value tool_calls_json(Json::arrayValue);
                for (const auto& call : msg.tool_calls) {
                    Json::Value call_json;
                    call_json["id"] = call.id;
                    call_json["type"] = "function";
                    call_json["function"]["name"] = call.name;
                    call_json["function"]["arguments"] = call.input.to_json().dump();
                    tool_calls_json.append(call_json);
                }
                msg_json["tool_calls"] = tool_calls_json;
            }
            
            messages_json.append(msg_json);
        }
        root["messages"] = messages_json;
        
        root["max_tokens"] = options.max_tokens.value_or(config_.max_tokens);
        root["temperature"] = options.temperature.value_or(config_.temperature);
        root["top_p"] = options.top_p.value_or(config_.top_p);
        
        if (options.stop_sequences.has_value() && !options.stop_sequences.value().empty()) {
            Json::Value stop_json(Json::arrayValue);
            for (const auto& seq : options.stop_sequences.value()) {
                stop_json.append(seq);
            }
            root["stop"] = stop_json;
        }
        
        if (!options.tools.empty()) {
            Json::Value tools_json(Json::arrayValue);
            for (const auto& tool : options.tools) {
                Json::Value tool_json;
                tool_json["type"] = "function";
                tool_json["function"]["name"] = tool.name;
                tool_json["function"]["description"] = tool.description;
                
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
    
    core::Result<GenerateResponse> parse_response(const infra::HttpResponse& http_resp) {
        if (!http_resp.ok) {
            return core::Result<GenerateResponse>::Err(
                "HTTP error " + std::to_string(http_resp.status_code) + ": " + http_resp.body);
        }
        
        Json::CharReaderBuilder reader_builder;
        Json::Value root;
        std::string errs;
        
        std::istringstream stream(http_resp.body);
        if (!Json::parseFromStream(reader_builder, stream, &root, &errs)) {
            return core::Result<GenerateResponse>::Err("Failed to parse JSON: " + errs);
        }
        
        GenerateResponse response;
        
        const auto& choices = root["choices"];
        if (choices.isArray() && !choices.empty()) {
            const auto& first_choice = choices[0];
            const auto& message = first_choice["message"];
            
            if (message["content"].isString()) {
                response.content = message["content"].asString();
            }
            
            const auto& tool_calls = message["tool_calls"];
            if (tool_calls.isArray()) {
                for (const auto& call : tool_calls) {
                    ToolCall tool_call;
                    tool_call.id = call["id"].asString();
                    tool_call.name = call["function"]["name"].asString();
                    
                    std::string args_str = call["function"]["arguments"].asString();
                    Json::CharReaderBuilder args_reader;
                    Json::Value args_json;
                    std::string args_errs;
                    std::istringstream args_stream(args_str);
                    if (Json::parseFromStream(args_reader, args_stream, &args_json, &args_errs)) {
                        // tool_call.input = convert_to_core_json(args_json);
                    }
                    
                    response.tool_calls.push_back(tool_call);
                }
            }
        }
        
        const auto& usage = root["usage"];
        if (!usage.isNull()) {
            response.input_tokens = usage["prompt_tokens"].asInt();
            response.output_tokens = usage["completion_tokens"].asInt();
        }
        
        return core::Result<GenerateResponse>::Ok(response);
    }
    
    ProviderConfig config_;
    infra::HttpClient::Ptr http_client_;
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
