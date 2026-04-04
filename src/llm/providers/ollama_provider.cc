#include "hunterclaw/llm/provider.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/core/logger.h"
#include "hunterclaw/core/result.h"
#include <json/json.h>
#include <nlohmann/json.hpp>
#include <sstream>

namespace hunterclaw {
namespace llm {

// ============================================================================
// Ollama Provider 实现 (使用 infra::HttpClient，无 curl 依赖)
// ============================================================================

class OllamaProvider : public Provider {
public:
    OllamaProvider(const ProviderConfig& config)
        : config_(config), http_client_(infra::HttpClient::create()) {
    }

    core::Result<GenerateResponse> generate(const GenerateOptions& options) override {
        auto request_json = build_request_json(options);

        infra::HttpRequestOptions http_opts;
        http_opts.body = request_json;
        http_opts.timeout_ms = 60000;
        http_opts.headers = {
            {"Content-Type", "application/json"}
        };

        std::string url = config_.base_url + "/api/chat";

        auto http_result = http_client_->post(url, http_opts);
        if (!http_result.is_ok()) {
            return core::Result<GenerateResponse>::Err(http_result.error);
        }

        return parse_response(http_result.get());
    }
    
    core::Result<GenerateResponse> generate_stream(
        const GenerateOptions& options,
        StreamCallback callback
    ) override {
        return core::Result<GenerateResponse>::Err("Streaming not yet implemented for Ollama");
    }
    
    int estimate_tokens(const std::string& text) const override {
        return static_cast<int>(text.length() / 4);
    }
    
    const ProviderConfig& config() const override { return config_; }
    std::string provider_id() const override { return "ollama"; }
    ProviderAPI api_type() const override { return ProviderAPI::Ollama; }
    bool supports_tools() const override { return true; }
    bool supports_streaming() const override { return true; }
    
    // Ollama 特有功能: 列出本地模型
    core::Result<std::vector<std::string>> list_models() {
        std::string url = config_.base_url + "/api/tags";

        auto http_result = http_client_->get(url, 5000);
        if (!http_result.is_ok()) {
            return core::Result<std::vector<std::string>>::Err(http_result.error);
        }

        const auto& resp = http_result.get();
        if (!resp.ok) {
            return core::Result<std::vector<std::string>>::Err(
                "HTTP error " + std::to_string(resp.status_code) + ": " + resp.body);
        }

        Json::CharReaderBuilder reader_builder;
        Json::Value root;
        std::string errs;

        std::istringstream stream(resp.body);
        if (!Json::parseFromStream(reader_builder, stream, &root, &errs)) {
            return core::Result<std::vector<std::string>>::Err("Failed to parse JSON: " + errs);
        }

        std::vector<std::string> models;
        const auto& models_json = root["models"];
        if (models_json.isArray()) {
            for (const auto& model : models_json) {
                models.push_back(model["name"].asString());
            }
        }

        return core::Result<std::vector<std::string>>::Ok(models);
    }
    
private:
    std::string build_request_json(const GenerateOptions& options) {
        Json::Value root;
        root["model"] = config_.model;
        root["stream"] = false;
        
        Json::Value messages_json(Json::arrayValue);
        for (const auto& msg : options.messages) {
            Json::Value msg_json;
            switch (msg.role) {
                case MessageRole::System:    msg_json["role"] = "system"; break;
                case MessageRole::User:      msg_json["role"] = "user"; break;
                case MessageRole::Assistant: msg_json["role"] = "assistant"; break;
                case MessageRole::Tool:      msg_json["role"] = "tool"; break;
            }
            msg_json["content"] = msg.content;
            messages_json.append(msg_json);
        }
        root["messages"] = messages_json;
        
        Json::Value opts;
        if (options.temperature.has_value()) opts["temperature"] = options.temperature.value();
        if (options.top_p.has_value()) opts["top_p"] = options.top_p.value();
        if (options.top_k.has_value()) opts["top_k"] = options.top_k.value();
        if (!opts.empty()) root["options"] = opts;
        
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
                Json::parseFromStream(reader, schema_stream, &schema_jsoncpp, &errs);
                
                tool_json["function"]["parameters"] = schema_jsoncpp;
                tools_json.append(tool_json);
            }
            root["tools"] = tools_json;
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
        
        const auto& message = root["message"];
        if (!message.isNull()) {
            response.content = message["content"].asString();
            
            const auto& tool_calls = message["tool_calls"];
            if (tool_calls.isArray()) {
                for (const auto& call : tool_calls) {
                    ToolCall tool_call;
                    tool_call.name = call["function"]["name"].asString();
                    
                    std::string args_str = call["function"]["arguments"].asString();
                    nlohmann::json args_nlohmann = nlohmann::json::parse(args_str);
                    tool_call.input = core::JsonValue(args_nlohmann);
                    
                    response.tool_calls.push_back(tool_call);
                }
            }
        }
        
        return core::Result<GenerateResponse>::Ok(response);
    }
    
    ProviderConfig config_;
    infra::HttpClient::Ptr http_client_;
};

core::Result<Provider::Ptr> create_ollama_provider(const ProviderConfig& config) {
    Provider::Ptr provider = std::make_shared<OllamaProvider>(config);
    return core::Result<Provider::Ptr>::Ok(provider);
}

} // namespace llm
} // namespace hunterclaw
