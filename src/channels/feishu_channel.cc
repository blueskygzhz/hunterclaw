#include "hunterclaw/channels/feishu_channel.h"
#include "hunterclaw/core/utils.h"
#include <chrono>
#include <sstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <curl/curl.h>
#include <cstring>
#include <algorithm>

namespace hunterclaw {
namespace channels {

// ============================================================================
// 简易 JSON 工具 (生产环境建议使用 nlohmann/json)
// ============================================================================

namespace {
    std::string json_escape(const std::string& str) {
        std::string result;
        result.reserve(str.length() * 1.2);
        for (char c : str) {
            switch (c) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:   result += c; break;
            }
        }
        return result;
    }
    
    std::string extract_json_string(const std::string& json, const std::string& field) {
        std::string search = "\"" + field + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";
        
        pos = json.find("\"", pos + search.length());
        if (pos == std::string::npos) return "";
        
        size_t end = pos + 1;
        while (end < json.length() && json[end] != '"') {
            if (json[end] == '\\') end++;
            end++;
        }
        
        return json.substr(pos + 1, end - pos - 1);
    }
    
    int extract_json_int(const std::string& json, const std::string& field) {
        std::string search = "\"" + field + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return 0;
        
        pos += search.length();
        while (pos < json.length() && std::isspace(json[pos])) pos++;
        
        int value = 0;
        while (pos < json.length() && std::isdigit(json[pos])) {
            value = value * 10 + (json[pos] - '0');
            pos++;
        }
        return value;
    }
    
    int64_t get_timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
}

// ============================================================================
// HTTP 客户端 (基于 libcurl)
// ============================================================================

struct HttpResponse {
    int status_code;
    std::string body;
    std::string error;
};

namespace {
    size_t curl_write_callback(void* ptr, size_t size, size_t nmemb, std::string* data) {
        data->append((char*)ptr, size * nmemb);
        return size * nmemb;
    }
}

HttpResponse http_request(
    const std::string& url,
    const std::string& method,
    const std::map<std::string, std::string>& headers,
    const std::string& body
) {
    HttpResponse response;
    response.status_code = 0;
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "Failed to init curl";
        return response;
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    
    struct curl_slist* header_list = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.length());
    }
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        response.error = curl_easy_strerror(res);
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        response.status_code = static_cast<int>(http_code);
    }
    
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    
    return response;
}

// ============================================================================
// FeishuChannelAdapter 实现
// ============================================================================

FeishuChannelAdapter::FeishuChannelAdapter(const FeishuConfig& config)
    : config_(config)
    , running_(false)
    , connected_(false)
    , token_expire_time_(0)
{
}

FeishuChannelAdapter::~FeishuChannelAdapter() {
    if (running_) {
        stop_account();
    }
}

// ----------------------------------------------------------------------------
// 基础接口
// ----------------------------------------------------------------------------

ChannelMeta FeishuChannelAdapter::meta() const {
    ChannelMeta m;
    m.id = "feishu";
    m.name = "Feishu";
    m.description = "Feishu (飞书) / Lark channel adapter";
    m.capabilities = {
        ChannelCapability::SendText,
        ChannelCapability::SendImage,
        ChannelCapability::SendFile,
        ChannelCapability::ReceiveText,
        ChannelCapability::ReceiveImage,
        ChannelCapability::ReceiveFile,
        ChannelCapability::GroupChat,
        ChannelCapability::PrivateChat
    };
    return m;
}

std::string FeishuChannelAdapter::resolve_domain() const {
    switch (config_.domain) {
        case FeishuDomain::Feishu:
            return "https://open.feishu.cn";
        case FeishuDomain::Lark:
            return "https://open.larksuite.com";
        case FeishuDomain::Custom:
            return config_.custom_domain;
        default:
            return "https://open.feishu.cn";
    }
}

std::string FeishuChannelAdapter::get_api_base_url() const {
    return resolve_domain() + "/open-apis";
}

// ----------------------------------------------------------------------------
// Token 管理
// ----------------------------------------------------------------------------

core::Result<core::JsonValue> FeishuChannelAdapter::get_tenant_access_token() {
    std::lock_guard<std::mutex> lock(token_mutex_);
    
    int64_t now = get_timestamp_ms();
    
    // Token 还有效（提前5分钟刷新）
    if (!tenant_access_token_.empty() && now < token_expire_time_ - 300000) {
        core::JsonValue result;
        result = tenant_access_token_;  // 使用赋值运算符
        return core::Result<core::JsonValue>::Ok(result);
    }
    
    // 获取新 Token
    std::string url = get_api_base_url() + "/auth/v3/tenant_access_token/internal";
    
    std::map<std::string, std::string> headers = {
        {"Content-Type", "application/json; charset=utf-8"}
    };
    
    std::string body = "{"
        "\"app_id\":\"" + json_escape(config_.app_id) + "\","
        "\"app_secret\":\"" + json_escape(config_.app_secret) + "\""
    "}";
    
    auto resp = http_request(url, "POST", headers, body);
    
    if (!resp.error.empty()) {
        return core::Result<core::JsonValue>::Err("Token request failed: " + resp.error);
    }
    
    if (resp.status_code != 200) {
        return core::Result<core::JsonValue>::Err(
            "Token request failed with status " + std::to_string(resp.status_code)
        );
    }
    
    int code = extract_json_int(resp.body, "code");
    if (code != 0) {
        std::string msg = extract_json_string(resp.body, "msg");
        return core::Result<core::JsonValue>::Err("Token API error: " + msg);
    }
    
    tenant_access_token_ = extract_json_string(resp.body, "tenant_access_token");
    int expire_seconds = extract_json_int(resp.body, "expire");
    token_expire_time_ = now + expire_seconds * 1000;
    
    if (tenant_access_token_.empty()) {
        return core::Result<core::JsonValue>::Err("Empty token received");
    }
    
    core::JsonValue result;
    result = tenant_access_token_;  // 使用赋值运算符
    return core::Result<core::JsonValue>::Ok(result);
}

// ----------------------------------------------------------------------------
// 账户管理
// ----------------------------------------------------------------------------

core::Result<bool> FeishuChannelAdapter::start_account(const ChannelContext& ctx) {
    if (running_) {
        return core::Result<bool>::Err("Already running");
    }
    
    context_ = ctx;
    
    // 测试连接
    auto token_result = get_tenant_access_token();
    if (token_result.is_err()) {
        return core::Result<bool>::Err("Failed to get token: " + token_result.error);
    }
    
    running_ = true;
    connected_ = true;
    
    // 启动连接模式
    if (config_.connection_mode == FeishuConnectionMode::WebSocket) {
        worker_thread_ = std::thread([this]() {
            this->run_websocket_mode();
        });
    } else if (config_.connection_mode == FeishuConnectionMode::Webhook) {
        worker_thread_ = std::thread([this]() {
            this->run_webhook_mode();
        });
    }
    
    if (ctx.log_info) {
        ctx.log_info("Feishu channel started: " + config_.app_id);
    }
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> FeishuChannelAdapter::stop_account() {
    if (!running_) {
        return core::Result<bool>::Ok(true);
    }
    
    running_ = false;
    connected_ = false;
    
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    
    if (context_.log_info) {
        context_.log_info("Feishu channel stopped");
    }
    
    return core::Result<bool>::Ok(true);
}

bool FeishuChannelAdapter::is_connected() const {
    return connected_;
}

core::Result<std::string> FeishuChannelAdapter::get_status() const {
    if (connected_) {
        return core::Result<std::string>::Ok("connected");
    } else if (running_) {
        return core::Result<std::string>::Ok("connecting");
    } else {
        return core::Result<std::string>::Ok("disconnected");
    }
}

// ----------------------------------------------------------------------------
// 消息发送
// ----------------------------------------------------------------------------

core::Result<bool> FeishuChannelAdapter::send_text(
    const std::string& to,
    const std::string& text
) {
    auto token_result = get_tenant_access_token();
    if (token_result.is_err()) {
        return core::Result<bool>::Err("Failed to get token: " + token_result.error);
    }
    
    std::string token = token_result.value.as_string();
    std::string url = get_api_base_url() + "/im/v1/messages?receive_id_type=chat_id";
    
    std::map<std::string, std::string> headers = {
        {"Content-Type", "application/json; charset=utf-8"},
        {"Authorization", "Bearer " + token}
    };
    
    std::string body = "{"
        "\"receive_id\":\"" + json_escape(to) + "\","
        "\"msg_type\":\"text\","
        "\"content\":\"{\\\"text\\\":\\\"" + json_escape(text) + "\\\"}\""
    "}";
    
    auto resp = http_request(url, "POST", headers, body);
    
    if (resp.status_code != 200) {
        return core::Result<bool>::Err(
            "Send failed with status " + std::to_string(resp.status_code)
        );
    }
    
    int code = extract_json_int(resp.body, "code");
    if (code != 0) {
        std::string msg = extract_json_string(resp.body, "msg");
        return core::Result<bool>::Err("Send failed: " + msg);
    }
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> FeishuChannelAdapter::send_image(
    const std::string& to,
    const std::string& image_key
) {
    auto token_result = get_tenant_access_token();
    if (token_result.is_err()) {
        return core::Result<bool>::Err("Failed to get token");
    }
    
    std::string token = token_result.value.as_string();
    std::string url = get_api_base_url() + "/im/v1/messages?receive_id_type=chat_id";
    
    std::map<std::string, std::string> headers = {
        {"Content-Type", "application/json; charset=utf-8"},
        {"Authorization", "Bearer " + token}
    };
    
    std::string body = "{"
        "\"receive_id\":\"" + json_escape(to) + "\","
        "\"msg_type\":\"image\","
        "\"content\":\"{\\\"image_key\\\":\\\"" + json_escape(image_key) + "\\\"}\""
    "}";
    
    auto resp = http_request(url, "POST", headers, body);
    
    int code = extract_json_int(resp.body, "code");
    if (code != 0) {
        return core::Result<bool>::Err("Send image failed");
    }
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> FeishuChannelAdapter::send_file(
    const std::string& to,
    const std::string& file_key
) {
    auto token_result = get_tenant_access_token();
    if (token_result.is_err()) {
        return core::Result<bool>::Err("Failed to get token");
    }
    
    std::string token = token_result.value.as_string();
    std::string url = get_api_base_url() + "/im/v1/messages?receive_id_type=chat_id";
    
    std::map<std::string, std::string> headers = {
        {"Content-Type", "application/json; charset=utf-8"},
        {"Authorization", "Bearer " + token}
    };
    
    std::string body = "{"
        "\"receive_id\":\"" + json_escape(to) + "\","
        "\"msg_type\":\"file\","
        "\"content\":\"{\\\"file_key\\\":\\\"" + json_escape(file_key) + "\\\"}\""
    "}";
    
    auto resp = http_request(url, "POST", headers, body);
    
    int code = extract_json_int(resp.body, "code");
    if (code != 0) {
        return core::Result<bool>::Err("Send file failed");
    }
    
    return core::Result<bool>::Ok(true);
}

// 其他扩展接口在头文件中有定义，但这里不需要实现（使用默认行为）

// ----------------------------------------------------------------------------
// 事件处理（内部方法）
// ----------------------------------------------------------------------------

void FeishuChannelAdapter::handle_message_received(const core::JsonValue& /* event_data */) {
    // TODO: 实现完整的消息处理逻辑
    // 需要解析 event_data，构建 ChannelMessage 并调用回调
}

// ----------------------------------------------------------------------------
// 权限检查
// ----------------------------------------------------------------------------

bool FeishuChannelAdapter::check_permission(
    const std::string& chat_id,
    const std::string& sender_id
) {
    // DM 权限检查
    if (chat_id.find("ou_") == 0) {  // 私聊
        if (config_.dm_policy == FeishuDMPolicy::Allowlist) {
            return is_in_allowlist(sender_id);
        }
        return true;  // Pairing/Open 模式
    }
    
    // 群组权限检查
    if (chat_id.find("oc_") == 0) {  // 群聊
        if (config_.group_policy == FeishuGroupPolicy::Disabled) {
            return false;
        }
        if (config_.group_policy == FeishuGroupPolicy::Allowlist) {
            return is_in_allowlist(chat_id);
        }
        return true;
    }
    
    return false;
}

bool FeishuChannelAdapter::is_in_allowlist(const std::string& id) {
    return std::find(config_.allow_from.begin(), config_.allow_from.end(), id) 
        != config_.allow_from.end();
}

bool FeishuChannelAdapter::is_duplicate_message(const std::string& message_id) {
    std::lock_guard<std::mutex> lock(dedup_mutex_);
    
    if (seen_message_ids_.find(message_id) != seen_message_ids_.end()) {
        return true;
    }
    
    seen_message_ids_.insert(message_id);
    
    // 限制缓存大小
    if (seen_message_ids_.size() > 10000) {
        // 简化实现：清空一半
        auto it = seen_message_ids_.begin();
        std::advance(it, 5000);
        seen_message_ids_.erase(seen_message_ids_.begin(), it);
    }
    
    return false;
}

// ----------------------------------------------------------------------------
// 连接模式实现（框架）
// ----------------------------------------------------------------------------

void FeishuChannelAdapter::run_websocket_mode() {
    // TODO: 实现 WebSocket 长连接
    // 需要 libwebsockets 或 websocketpp
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void FeishuChannelAdapter::run_webhook_mode() {
    // TODO: 实现 Webhook HTTP 服务器
    // 需要 cpp-httplib 或 Boost.Beast
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ============================================================================
// FeishuChannelPlugin 实现
// ============================================================================

FeishuChannelPlugin::FeishuChannelPlugin(const FeishuConfig& config)
    : config_(config)
{
}

std::string FeishuChannelPlugin::id() const {
    return "feishu";
}

std::string FeishuChannelPlugin::name() const {
    return "Feishu Channel Plugin";
}

std::string FeishuChannelPlugin::version() const {
    return "1.0.0";
}

std::shared_ptr<ChannelAdapter> FeishuChannelPlugin::create_adapter() {
    return std::make_shared<FeishuChannelAdapter>(config_);
}

// ============================================================================
// 导出插件
// ============================================================================

extern "C" {
    ChannelPlugin* create_feishu_channel_plugin(const char* /* config_json */) {
        FeishuConfig config;
        config.app_id = "cli_xxx";
        config.app_secret = "xxx";
        return new FeishuChannelPlugin(config);
    }
    
    void destroy_feishu_channel_plugin(ChannelPlugin* plugin) {
        delete plugin;
    }
}

} // namespace channels
} // namespace hunterclaw
