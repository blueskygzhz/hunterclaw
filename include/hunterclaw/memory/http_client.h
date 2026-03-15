#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <map>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace memory {

// ============================================================================
// HTTP Client - 用于Embedding Provider的HTTP调用
// ============================================================================

struct HttpRequest {
    std::string url;
    std::string method = "POST";  // GET, POST, etc.
    std::map<std::string, std::string> headers;
    std::string body;
    int timeout_ms = 30000;
};

struct HttpResponse {
    int status_code = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string error_message;
    
    bool is_success() const {
        return status_code >= 200 && status_code < 300;
    }
};

class HttpClient {
public:
    // 执行HTTP请求
    static core::Result<HttpResponse> request(const HttpRequest& req);
    
    // POST JSON请求 (简化接口)
    static core::Result<nlohmann::json> post_json(
        const std::string& url,
        const nlohmann::json& body,
        const std::map<std::string, std::string>& headers = {},
        int timeout_ms = 30000
    );
    
    // GET请求
    static core::Result<HttpResponse> get(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {},
        int timeout_ms = 30000
    );
    
private:
    // cURL回调函数
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata);
};

}  // namespace memory
}  // namespace hunterclaw
