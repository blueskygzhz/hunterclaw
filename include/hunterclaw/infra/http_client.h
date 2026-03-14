#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/core/json.h"
#include <string>
#include <map>
#include <optional>
#include <memory>

namespace hunterclaw {
namespace infra {

/**
 * @brief HTTP 响应
 * 
 * 对齐 OpenClaw fetch/axios 响应格式
 */
struct HttpResponse {
    int status_code;                          // HTTP 状态码
    std::map<std::string, std::string> headers;  // 响应头
    std::string body;                         // 响应体 (文本)
    bool ok;                                  // status_code >= 200 && < 300
    
    /**
     * @brief 解析 JSON 响应体
     */
    core::Result<core::JsonValue> json() const;
};

/**
 * @brief HTTP 请求选项
 */
struct HttpRequestOptions {
    std::map<std::string, std::string> headers;  // 自定义请求头
    std::optional<std::string> body;              // 请求体
    int timeout_ms = 5000;                        // 超时时间 (毫秒)
    bool follow_redirects = true;                 // 是否跟随重定向
    int max_redirects = 5;                        // 最大重定向次数
};

/**
 * @brief HTTP 客户端
 * 
 * 轻量级 HTTP/HTTPS 客户端,原生实现 (无 CURL 依赖)
 * 
 * **对齐 OpenClaw**:
 * - fetch() API 风格
 * - 超时控制
 * - JSON 请求/响应
 * - 错误处理
 * 
 * **实现方式**:
 * - HTTP: 原生 Unix socket
 * - HTTPS: OpenSSL (可选,通过 HUNTERCLAW_ENABLE_SSL 编译选项)
 * 
 * **特性**:
 * - 支持 HTTP/HTTPS
 * - 自定义请求头
 * - GET/POST/PUT/DELETE/PATCH
 * - 超时控制
 * - 重定向跟随
 */
class HttpClient {
public:
    using Ptr = std::shared_ptr<HttpClient>;
    
    /**
     * @brief 创建 HTTP 客户端
     */
    static Ptr create();
    
    /**
     * @brief HTTP GET 请求
     * 
     * @param url 完整 URL (支持 http:// 和 https://)
     * @param timeout_ms 超时时间 (毫秒)
     * @return HttpResponse 或错误
     */
    core::Result<HttpResponse> get(
        const std::string& url,
        int timeout_ms = 5000
    );
    
    /**
     * @brief HTTP GET 请求 (带选项)
     */
    core::Result<HttpResponse> get(
        const std::string& url,
        const HttpRequestOptions& options
    );
    
    /**
     * @brief HTTP POST 请求
     * 
     * @param url 完整 URL
     * @param body 请求体 (JSON)
     * @param timeout_ms 超时时间 (毫秒)
     * @return HttpResponse 或错误
     */
    core::Result<HttpResponse> post(
        const std::string& url,
        const core::JsonValue& body,
        int timeout_ms = 5000
    );
    
    /**
     * @brief HTTP POST 请求 (带选项)
     */
    core::Result<HttpResponse> post(
        const std::string& url,
        const HttpRequestOptions& options
    );
    
    /**
     * @brief HTTP PUT 请求
     */
    core::Result<HttpResponse> put(
        const std::string& url,
        const HttpRequestOptions& options
    );
    
    /**
     * @brief HTTP PATCH 请求
     */
    core::Result<HttpResponse> patch(
        const std::string& url,
        const HttpRequestOptions& options
    );
    
    /**
     * @brief HTTP DELETE 请求
     * 
     * @param url 完整 URL
     * @param timeout_ms 超时时间 (毫秒)
     * @return HttpResponse 或错误
     */
    core::Result<HttpResponse> del(
        const std::string& url,
        int timeout_ms = 5000
    );
    
    /**
     * @brief HTTP DELETE 请求 (带选项)
     */
    core::Result<HttpResponse> del(
        const std::string& url,
        const HttpRequestOptions& options
    );
    
    /**
     * @brief 通用 HTTP 请求
     * 
     * @param method HTTP 方法 (GET/POST/PUT/DELETE/PATCH)
     * @param url 完整 URL
     * @param options 请求选项
     * @return HttpResponse 或错误
     */
    core::Result<HttpResponse> request(
        const std::string& method,
        const std::string& url,
        const HttpRequestOptions& options
    );
    
private:
    HttpClient() = default;
    
    /**
     * @brief 内部请求方法 (兼容旧版)
     */
    core::Result<HttpResponse> request(
        const std::string& method,
        const std::string& url,
        const std::optional<std::string>& body,
        int timeout_ms
    );
    
    /**
     * @brief 解析 URL
     * @return {protocol, host, port, path} 或错误
     */
    struct ParsedUrl {
        std::string protocol;  // "http" or "https"
        std::string host;
        int port;
        std::string path;
        bool use_ssl;
    };
    core::Result<ParsedUrl> parse_url(const std::string& url);
    
    /**
     * @brief 创建 TCP 连接
     * @return socket fd 或错误
     */
    core::Result<int> connect_socket(
        const std::string& host,
        int port,
        int timeout_ms
    );
    
    /**
     * @brief 创建 SSL 连接 (如果启用 SSL 支持)
     * @return {socket_fd, ssl_context} 或错误
     */
    struct SslConnection {
        int socket_fd;
        void* ssl_ctx;    // SSL* 指针 (void* to avoid OpenSSL dependency in header)
        void* ssl;        // SSL* 指针
    };
    core::Result<SslConnection> connect_ssl(
        const std::string& host,
        int port,
        int timeout_ms
    );
    
    /**
     * @brief 发送 HTTP 请求
     */
    core::Result<void> send_request(
        int socket_fd,
        void* ssl,
        const std::string& method,
        const std::string& path,
        const std::string& host,
        int port,
        const HttpRequestOptions& options
    );
    
    /**
     * @brief 接收 HTTP 响应
     */
    core::Result<HttpResponse> recv_response(
        int socket_fd,
        void* ssl,
        int timeout_ms
    );
    
    /**
     * @brief 关闭连接
     */
    void close_connection(int socket_fd, void* ssl_ctx, void* ssl);
};

}  // namespace infra
}  // namespace hunterclaw
