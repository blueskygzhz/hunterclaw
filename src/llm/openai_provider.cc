#include "hunterclaw/llm/provider.h"
#include <curl/curl.h>
#include <jsoncpp/json/json.h>
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>

namespace hunterclaw {
namespace llm {

// HTTP响应处理
struct CurlResponse {
    std::string data;
    long status_code = 0;
};

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* response = static_cast<CurlResponse*>(userdata);
    response->data.append(ptr, size * nmemb);
    return size * nmemb;
}

// OpenAI Provider实现
class OpenAIProvider : public Provider {
public:
    OpenAIProvider(const std::string& api_key, const std::string& base_url)
        : api_key_(api_key), base_url_(base_url) {
        
        // 初始化模型列表
        models_ = {
            {"gpt-4o", "GPT-4o", false, {"text", "image"}, {5.0, 15.0, 2.5, 5.0}, 128000, 16384},
            {"gpt-4o-mini", "GPT-4o Mini", false, {"text", "image"}, {0.15, 0.6, 0.075, 0.15}, 128000, 16384},
            {"gpt-4-turbo", "GPT-4 Turbo", false, {"text", "image"}, {10.0, 30.0, 5.0, 10.0}, 128000, 4096},
            {"gpt-3.5-turbo", "GPT-3.5 Turbo", false, {"text"}, {0.5, 1.5, 0.25, 0.5}, 16385, 4096},
            {"o1", "o1", true, {"text"}, {15.0, 60.0, 7.5, 15.0}, 200000, 100000},
            {"o1-mini", "o1 Mini", true, {"text"}, {3.0, 12.0, 1.5, 3.0}, 128000, 65536}
        };
    }
    
    std::string name() const override {
        return "openai";
    }
    
    std::vector<ModelConfig> list_models() override {
        return models_;
    }
    
    Response generate(const GenerateOptions& options) override {
        // 构建请求JSON
        Json::Value request;
        request["model"] = options.model;
        request["messages"] = build_messages_json(options.messages);
        
        if (options.temperature) {
            request["temperature"] = *options.temperature;
        }
        if (options.max_tokens) {
            request["max_tokens"] = *options.max_tokens;
        }
        if (options.top_p) {
            request["top_p"] = *options.top_p;
        }
        
        // 添加tools
        if (!options.tools.empty()) {
            Json::Value tools(Json::arrayValue);
            for (const auto& tool : options.tools) {
                Json::Value tool_obj;
                tool_obj["type"] = "function";
                
                Json::Value function;
                function["name"] = tool.name;
                function["description"] = tool.description;
                
                Json::Reader reader;
                Json::Value parameters;
                reader.parse(tool.parameters, parameters);
                function["parameters"] = parameters;
                
                tool_obj["function"] = function;
                tools.append(tool_obj);
            }
            request["tools"] = tools;
        }
        
        // 发送HTTP请求
        std::string url = base_url_ + "/chat/completions";
        CurlResponse response = post_json(url, request);
        
        if (response.status_code != 200) {
            throw std::runtime_error("OpenAI API error: " + std::to_string(response.status_code) + 
                                   " - " + response.data);
        }
        
        // 解析响应
        return parse_response(response.data, options.model);
    }
    
    Response stream_generate(const GenerateOptions& options) override {
        if (!options.stream || !options.on_chunk) {
            return generate(options);
        }
        
        // 构建请求
        Json::Value request;
        request["model"] = options.model;
        request["messages"] = build_messages_json(options.messages);
        request["stream"] = true;
        
        if (options.temperature) request["temperature"] = *options.temperature;
        if (options.max_tokens) request["max_tokens"] = *options.max_tokens;
        
        // 流式处理
        std::string url = base_url_ + "/chat/completions";
        Response result;
        result.model = options.model;
        
        std::string accumulated_content;
        std::vector<ToolCall> accumulated_tools;
        
        // 发送流式请求
        stream_post_json(url, request, [&](const std::string& line) {
            if (line.empty() || line == "data: [DONE]") return;
            
            if (line.rfind("data: ", 0) == 0) {
                std::string json_str = line.substr(6);
                
                Json::Reader reader;
                Json::Value chunk;
                if (!reader.parse(json_str, chunk)) return;
                
                auto& choices = chunk["choices"];
                if (choices.empty()) return;
                
                auto& delta = choices[0]["delta"];
                
                // 处理文本内容
                if (delta.isMember("content")) {
                    std::string text = delta["content"].asString();
                    accumulated_content += text;
                    
                    StreamChunk sc;
                    sc.type = StreamChunk::Type::Text;
                    sc.text = text;
                    options.on_chunk(sc);
                }
                
                // 处理tool calls
                if (delta.isMember("tool_calls")) {
                    auto& tool_calls = delta["tool_calls"];
                    for (const auto& tc : tool_calls) {
                        int index = tc["index"].asInt();
                        
                        // 确保数组足够大
                        while (static_cast<int>(accumulated_tools.size()) <= index) {
                            accumulated_tools.push_back({"", "", ""});
                        }
                        
                        if (tc.isMember("id")) {
                            accumulated_tools[index].id = tc["id"].asString();
                        }
                        if (tc["function"].isMember("name")) {
                            accumulated_tools[index].name = tc["function"]["name"].asString();
                        }
                        if (tc["function"].isMember("arguments")) {
                            accumulated_tools[index].arguments += tc["function"]["arguments"].asString();
                        }
                    }
                }
                
                // 处理usage (最后一个chunk)
                if (chunk.isMember("usage")) {
                    result.usage = parse_usage(chunk["usage"]);
                }
            }
        });
        
        // 发送完成信号
        StreamChunk done_chunk;
        done_chunk.type = StreamChunk::Type::Done;
        options.on_chunk(done_chunk);
        
        result.content = accumulated_content;
        result.tool_calls = accumulated_tools;
        result.finish_reason = "stop";
        
        return result;
    }

private:
    std::string api_key_;
    std::string base_url_;
    std::vector<ModelConfig> models_;
    
    Json::Value build_messages_json(const std::vector<Message>& messages) {
        Json::Value result(Json::arrayValue);
        
        for (const auto& msg : messages) {
            Json::Value msg_obj;
            
            switch (msg.role) {
                case MessageRole::System:
                    msg_obj["role"] = "system";
                    break;
                case MessageRole::User:
                    msg_obj["role"] = "user";
                    break;
                case MessageRole::Assistant:
                    msg_obj["role"] = "assistant";
                    break;
                case MessageRole::Tool:
                    msg_obj["role"] = "tool";
                    break;
            }
            
            msg_obj["content"] = msg.content;
            
            if (msg.name) {
                msg_obj["name"] = *msg.name;
            }
            if (msg.tool_call_id) {
                msg_obj["tool_call_id"] = *msg.tool_call_id;
            }
            
            result.append(msg_obj);
        }
        
        return result;
    }
    
    CurlResponse post_json(const std::string& url, const Json::Value& json) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }
        
        Json::StreamWriterBuilder writer;
        std::string json_str = Json::writeString(writer, json);
        
        CurlResponse response;
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth_header = "Authorization: Bearer " + api_key_;
        headers = curl_slist_append(headers, auth_header.c_str());
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        
        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            throw std::runtime_error("CURL error: " + std::string(curl_easy_strerror(res)));
        }
        
        return response;
    }
    
    void stream_post_json(const std::string& url, const Json::Value& json,
                         std::function<void(const std::string&)> callback) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }
        
        Json::StreamWriterBuilder writer;
        std::string json_str = Json::writeString(writer, json);
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth_header = "Authorization: Bearer " + api_key_;
        headers = curl_slist_append(headers, auth_header.c_str());
        
        std::string buffer;
        auto write_fn = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* data = static_cast<std::pair<std::string*, std::function<void(const std::string&)>*>*>(userdata);
            std::string& buffer = *data->first;
            auto& callback = *data->second;
            
            buffer.append(ptr, size * nmemb);
            
            // 按行处理
            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                
                callback(line);
            }
            
            return size * nmemb;
        };
        
        std::pair<std::string*, std::function<void(const std::string&)>*> userdata = {&buffer, &callback};
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_fn);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &userdata);
        
        CURLcode res = curl_easy_perform(curl);
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            throw std::runtime_error("CURL error: " + std::string(curl_easy_strerror(res)));
        }
    }
    
    Response parse_response(const std::string& json_str, const std::string& model) {
        Json::Reader reader;
        Json::Value root;
        
        if (!reader.parse(json_str, root)) {
            throw std::runtime_error("Failed to parse response JSON");
        }
        
        Response result;
        result.model = model;
        
        auto& choices = root["choices"];
        if (!choices.empty()) {
            auto& message = choices[0]["message"];
            
            if (message.isMember("content") && !message["content"].isNull()) {
                result.content = message["content"].asString();
            }
            
            if (message.isMember("tool_calls")) {
                auto& tool_calls = message["tool_calls"];
                for (const auto& tc : tool_calls) {
                    ToolCall tool_call;
                    tool_call.id = tc["id"].asString();
                    tool_call.name = tc["function"]["name"].asString();
                    tool_call.arguments = tc["function"]["arguments"].asString();
                    result.tool_calls.push_back(tool_call);
                }
            }
            
            result.finish_reason = choices[0]["finish_reason"].asString();
        }
        
        if (root.isMember("usage")) {
            result.usage = parse_usage(root["usage"]);
        }
        
        return result;
    }
    
    Response::Usage parse_usage(const Json::Value& usage_json) {
        Response::Usage usage;
        
        usage.input_tokens = usage_json.get("prompt_tokens", 0).asInt64();
        usage.output_tokens = usage_json.get("completion_tokens", 0).asInt64();
        
        // Prompt caching (如果支持)
        if (usage_json.isMember("prompt_tokens_details")) {
            auto& details = usage_json["prompt_tokens_details"];
            usage.cache_read_tokens = details.get("cached_tokens", 0).asInt64();
        }
        
        // 计算cost
        for (const auto& model : models_) {
            if (model.id == usage_json.get("model", "").asString()) {
                usage.cost.input = (usage.input_tokens / 1000000.0) * model.cost.input;
                usage.cost.output = (usage.output_tokens / 1000000.0) * model.cost.output;
                usage.cost.cache_read = (usage.cache_read_tokens / 1000000.0) * model.cost.cache_read;
                usage.cost.total = usage.cost.input + usage.cost.output + usage.cost.cache_read;
                break;
            }
        }
        
        return usage;
    }
};

// Factory实现
std::shared_ptr<Provider> ProviderFactory::create_openai(
    const std::string& api_key,
    const std::string& base_url
) {
    return std::make_shared<OpenAIProvider>(api_key, base_url);
}

std::shared_ptr<Provider> ProviderFactory::create_anthropic(
    const std::string& api_key,
    const std::string& base_url
) {
    // TODO: Anthropic实现
    throw std::runtime_error("Anthropic provider not implemented yet");
}

std::shared_ptr<Provider> ProviderFactory::create_ollama(
    const std::string& base_url
) {
    // TODO: Ollama实现
    throw std::runtime_error("Ollama provider not implemented yet");
}

std::shared_ptr<Provider> ProviderFactory::create_custom(
    const std::string& name,
    const std::string& base_url,
    const std::string& api_key
) {
    // 使用OpenAI兼容格式
    return std::make_shared<OpenAIProvider>(api_key, base_url);
}

// ProviderManager实现
std::shared_ptr<ProviderManager> ProviderManager::create() {
    return std::make_shared<ProviderManager>();
}

void ProviderManager::register_provider(const std::string& name, std::shared_ptr<Provider> provider) {
    providers_[name] = provider;
}

std::shared_ptr<Provider> ProviderManager::get_provider(const std::string& name) {
    auto it = providers_.find(name);
    if (it != providers_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::string> ProviderManager::list_providers() const {
    std::vector<std::string> result;
    for (const auto& [name, _] : providers_) {
        result.push_back(name);
    }
    return result;
}

void ProviderManager::load_from_config(const std::map<std::string, std::string>& config) {
    // 从配置加载providers
    // TODO: 实现配置解析
}

} // namespace llm
} // namespace hunterclaw
