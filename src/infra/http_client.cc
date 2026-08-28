#include "hunterclaw/infra/http_client.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <mutex>       // std::once_flag / std::call_once（SSL 库初始化）

// OpenSSL support (optional)
#ifdef HUNTERCLAW_ENABLE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>   // X509_VERIFY_PARAM_set1_host / set_hostflags
#include <openssl/opensslv.h>

// ============================================================================
// OpenSSL 1.0.2 兼容层
//
// 本文件的 SSL 代码按 OpenSSL 1.1.0+ 编写，但构建环境可能是 1.0.2
// （RHEL/CentOS 7 系默认 1.0.2k，且仍在大量生产环境中）。
//
// ⚠ 这段兼容层【不降低安全性】—— 三个替换都是等价或更严格的：
//   ① TLS_client_method → SSLv23_client_method
//      名字有误导：SSLv23_client_method 是"协商最高可用版本"的方法，
//      并非"启用 SSLv2/v3"。真正的版本下限由 ② 控制。
//      在 1.1.0 里前者只是后者的别名（ssl.h: # define TLS_client_method
//      SSLv23_client_method 的反向兼容宏）。
//   ② SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION)
//      → SSL_CTX_set_options(ctx, NO_SSLv2|NO_SSLv3|NO_TLSv1|NO_TLSv1_1)
//      等价效果：下限同样是 TLS 1.2。1.0.2 里 min_proto_version 这个
//      setter 不存在，但 SSL_OP_NO_* 位掩码从 0.9.x 起就有。
//   ③ 主机名校验（X509_VERIFY_PARAM_set1_host）在 1.0.2 里【已存在】
//      （1.0.2 是引入它的版本），故无需替换 —— 这点很关键，
//      否则就得手写 SAN 比对，那才是真正会引入漏洞的地方。
//
// ★ 判据用 OPENSSL_VERSION_NUMBER 而非编译器探测：
//   LibreSSL 会伪装成 OPENSSL_VERSION_NUMBER 2.0.0 但缺 1.1.0 的 API，
//   故额外排除 LIBRESSL_VERSION_NUMBER。
// ============================================================================
#if (OPENSSL_VERSION_NUMBER < 0x10100000L) || defined(LIBRESSL_VERSION_NUMBER)

#ifndef HUNTERCLAW_OPENSSL_LEGACY
#define HUNTERCLAW_OPENSSL_LEGACY 1
#endif

// ① 1.0.2 无 TLS_client_method；SSLv23_client_method 才是"协商最高版本"
#ifndef TLS_client_method
#define TLS_client_method SSLv23_client_method
#endif

#endif  // OpenSSL < 1.1.0

#endif  // HUNTERCLAW_ENABLE_SSL

namespace hunterclaw {
namespace infra {

// ============================================================================
// HttpResponse Implementation
// ============================================================================

core::Result<core::JsonValue> HttpResponse::json() const {
    if (body.empty()) {
        return core::Result<core::JsonValue>::Err("Empty response body");
    }
    
    return core::JsonValue::parse(body);
}

// ============================================================================
// HttpClient Implementation
// ============================================================================

HttpClient::Ptr HttpClient::create() {
    return std::shared_ptr<HttpClient>(new HttpClient());
}

// ----------------------------------------------------------------------------
// Public API (simple)
// ----------------------------------------------------------------------------

core::Result<HttpResponse> HttpClient::get(
    const std::string& url,
    int timeout_ms
) {
    HttpRequestOptions options;
    options.timeout_ms = timeout_ms;
    return request("GET", url, options);
}

core::Result<HttpResponse> HttpClient::get(
    const std::string& url,
    const HttpRequestOptions& options
) {
    return request("GET", url, options);
}

core::Result<HttpResponse> HttpClient::post(
    const std::string& url,
    const core::JsonValue& body,
    int timeout_ms
) {
    HttpRequestOptions options;
    options.timeout_ms = timeout_ms;
    options.body = body.to_string();
    options.headers["Content-Type"] = "application/json";
    return request("POST", url, options);
}

core::Result<HttpResponse> HttpClient::post(
    const std::string& url,
    const HttpRequestOptions& options
) {
    return request("POST", url, options);
}

core::Result<HttpResponse> HttpClient::put(
    const std::string& url,
    const HttpRequestOptions& options
) {
    return request("PUT", url, options);
}

core::Result<HttpResponse> HttpClient::patch(
    const std::string& url,
    const HttpRequestOptions& options
) {
    return request("PATCH", url, options);
}

core::Result<HttpResponse> HttpClient::del(
    const std::string& url,
    int timeout_ms
) {
    HttpRequestOptions options;
    options.timeout_ms = timeout_ms;
    return request("DELETE", url, options);
}

core::Result<HttpResponse> HttpClient::del(
    const std::string& url,
    const HttpRequestOptions& options
) {
    return request("DELETE", url, options);
}

// ----------------------------------------------------------------------------
// Main Request Method
// ----------------------------------------------------------------------------

core::Result<HttpResponse> HttpClient::request(
    const std::string& method,
    const std::string& url,
    const HttpRequestOptions& options
) {
    // 1. Parse URL
    auto parsed = parse_url(url);
    if (parsed.is_err()) {
        return core::Result<HttpResponse>::Err(parsed.error);
    }
    auto& u = parsed.get();
    
    int socket_fd = -1;
    void* ssl_ctx = nullptr;
    void* ssl = nullptr;
    
    // 2. Create connection (HTTP or HTTPS)
    if (u.use_ssl) {
#ifdef HUNTERCLAW_ENABLE_SSL
        auto ssl_result = connect_ssl(u.host, u.port, options.timeout_ms, options);
        if (ssl_result.is_err()) {
            return core::Result<HttpResponse>::Err(ssl_result.error);
        }
        socket_fd = ssl_result.get().socket_fd;
        ssl_ctx = ssl_result.get().ssl_ctx;
        ssl = ssl_result.get().ssl;
#else
        return core::Result<HttpResponse>::Err("HTTPS not supported (compile with HUNTERCLAW_ENABLE_SSL)");
#endif
    } else {
        auto socket_result = connect_socket(u.host, u.port, options.timeout_ms);
        if (socket_result.is_err()) {
            return core::Result<HttpResponse>::Err(socket_result.error);
        }
        socket_fd = socket_result.get();
    }
    
    // 3. Send request
    auto send_result = send_request(socket_fd, ssl, method, u.path, u.host, u.port, options);
    if (send_result.is_err()) {
        close_connection(socket_fd, ssl_ctx, ssl);
        return core::Result<HttpResponse>::Err(send_result.error);
    }
    
    // 4. Receive response
    auto response_result = recv_response(socket_fd, ssl, options.timeout_ms);
    close_connection(socket_fd, ssl_ctx, ssl);
    
    return response_result;
}

// ----------------------------------------------------------------------------
// Internal Request Method (backward compatibility)
// ----------------------------------------------------------------------------

core::Result<HttpResponse> HttpClient::request(
    const std::string& method,
    const std::string& url,
    const std::optional<std::string>& body,
    int timeout_ms
) {
    HttpRequestOptions options;
    options.timeout_ms = timeout_ms;
    options.body = body;
    return request(method, url, options);
}

// ----------------------------------------------------------------------------
// URL Parsing
// ----------------------------------------------------------------------------

core::Result<HttpClient::ParsedUrl> HttpClient::parse_url(const std::string& url) {
    std::string normalized = url;
    ParsedUrl result;
    
    // Parse protocol
    const std::string http_prefix = "http://";
    const std::string https_prefix = "https://";
    
    if (normalized.find(http_prefix) == 0) {
        result.protocol = "http";
        result.use_ssl = false;
        normalized = normalized.substr(http_prefix.length());
    } else if (normalized.find(https_prefix) == 0) {
        result.protocol = "https";
        result.use_ssl = true;
        normalized = normalized.substr(https_prefix.length());
    } else {
        return core::Result<ParsedUrl>::Err("URL must start with http:// or https://");
    }
    
    // Split host:port and path
    size_t path_pos = normalized.find('/');
    std::string host_port = (path_pos == std::string::npos) 
        ? normalized 
        : normalized.substr(0, path_pos);
    result.path = (path_pos == std::string::npos) 
        ? "/" 
        : normalized.substr(path_pos);
    
    // Split host and port
    size_t colon_pos = host_port.find(':');
    if (colon_pos == std::string::npos) {
        result.host = host_port;
        result.port = result.use_ssl ? 443 : 80;  // Default port
    } else {
        result.host = host_port.substr(0, colon_pos);
        std::string port_str = host_port.substr(colon_pos + 1);
        try {
            result.port = std::stoi(port_str);
        } catch (...) {
            return core::Result<ParsedUrl>::Err("Invalid port number: " + port_str);
        }
    }
    
    if (result.host.empty()) {
        return core::Result<ParsedUrl>::Err("Empty host in URL");
    }
    
    return core::Result<ParsedUrl>::Ok(result);
}

// ----------------------------------------------------------------------------
// TCP Connection
// ----------------------------------------------------------------------------

core::Result<int> HttpClient::connect_socket(
    const std::string& host,
    int port,
    int timeout_ms
) {
    // Create socket
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return core::Result<int>::Err("Failed to create socket");
    }
    
    // Resolve hostname
    struct addrinfo hints{};
    struct addrinfo* result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    int ret = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (ret != 0) {
        close(socket_fd);
        return core::Result<int>::Err("Failed to resolve host: " + host);
    }
    
    // Configure address
    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr, 
           &((struct sockaddr_in*)result->ai_addr)->sin_addr,
           sizeof(server_addr.sin_addr));
    freeaddrinfo(result);
    
    // Set non-blocking mode (for timeout support)
    int flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
    
    // Connect
    ret = connect(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(socket_fd);
        return core::Result<int>::Err("Failed to connect to " + host + ":" + std::to_string(port));
    }
    
    // Wait for connection completion (using poll)
    if (errno == EINPROGRESS) {
        struct pollfd pfd;
        pfd.fd = socket_fd;
        pfd.events = POLLOUT;
        
        ret = poll(&pfd, 1, timeout_ms);
        if (ret == 0) {
            close(socket_fd);
            return core::Result<int>::Err("Connection timeout");
        } else if (ret < 0) {
            close(socket_fd);
            return core::Result<int>::Err("Poll error");
        }
        
        // Check connection status
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            close(socket_fd);
            return core::Result<int>::Err("Connection failed");
        }
    }
    
    // Restore blocking mode
    fcntl(socket_fd, F_SETFL, flags);
    
    return core::Result<int>::Ok(socket_fd);
}

// ----------------------------------------------------------------------------
// SSL Connection
// ----------------------------------------------------------------------------

#ifdef HUNTERCLAW_ENABLE_SSL

core::Result<HttpClient::SslConnection> HttpClient::connect_ssl(
    const std::string& host,
    int port,
    int timeout_ms,
    const HttpRequestOptions& options
) {
    // Initialize SSL library.
    // ⚠ 1.1.0+ 会自动初始化，这些调用是 no-op；1.0.2 则【必须】显式调用，
    //   否则 SSL_CTX_new 返回 nullptr 且 ERR 队列为空（极难排查）。
    // ⚠ static 局部变量的初始化是线程安全的（C++11），但这三个 OpenSSL
    //   函数本身在 1.0.2 下不是 —— 用 std::call_once 保证只执行一次。
    static std::once_flag ssl_init_once;
    std::call_once(ssl_init_once, []() {
        SSL_load_error_strings();
        SSL_library_init();
        OpenSSL_add_all_algorithms();
    });

    // Create TCP connection first
    auto socket_result = connect_socket(host, port, timeout_ms);
    if (socket_result.is_err()) {
        return core::Result<SslConnection>::Err(socket_result.error);
    }
    // ⚠ 必须用 .get()：value 是 std::optional<int>，直接赋给 int 编译失败。
    //   （此处曾是编译错误 —— 本分支从未被编译过，见 src/BUILD 的说明）
    int socket_fd = socket_result.get();

    // Create SSL context
    const SSL_METHOD* method = TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
        close(socket_fd);
        return core::Result<SslConnection>::Err("Failed to create SSL context");
    }

    // 🎯 对齐 OpenClaw sslVerify: 证书校验 (默认开启，防止 MITM)
    if (options.ssl_verify) {
        // 强制最低 TLS 1.2
#ifdef HUNTERCLAW_OPENSSL_LEGACY
        // OpenSSL 1.0.2 无 SSL_CTX_set_min_proto_version。
        // ★ 用 SSL_OP_NO_* 位掩码达到【完全等价】的下限：禁掉 1.2 以下全部。
        //   ⚠ SSL_OP_NO_TLSv1_1 在 1.0.1 起才有；1.0.0 及更早不支持 TLS 1.2，
        //     那种环境本就不该跑这个项目，故不再往下兼容。
        SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                                    SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
#else
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
#endif

        // 加载 CA: 自定义 bundle 或系统默认信任链
        if (!options.ca_bundle_path.empty()) {
            if (SSL_CTX_load_verify_locations(
                    ctx, options.ca_bundle_path.c_str(), nullptr) != 1) {
                SSL_CTX_free(ctx);
                close(socket_fd);
                return core::Result<SslConnection>::Err(
                    "Failed to load CA bundle: " + options.ca_bundle_path);
            }
        } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
            SSL_CTX_free(ctx);
            close(socket_fd);
            return core::Result<SslConnection>::Err(
                "Failed to load system default CA paths");
        }

        // 要求校验对端证书 (握手时验证证书链)
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_verify_depth(ctx, 10);
    } else {
        // 显式关闭校验 (仅信任的 self-signed server)
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    }

    // 🎯 对齐 OpenClaw clientCert/clientKey: mTLS 客户端证书
    if (!options.client_cert_path.empty()) {
        if (SSL_CTX_use_certificate_file(
                ctx, options.client_cert_path.c_str(), SSL_FILETYPE_PEM) != 1) {
            SSL_CTX_free(ctx);
            close(socket_fd);
            return core::Result<SslConnection>::Err(
                "Failed to load client certificate: " + options.client_cert_path);
        }
        const std::string& key_path = options.client_key_path.empty()
            ? options.client_cert_path : options.client_key_path;
        if (SSL_CTX_use_PrivateKey_file(
                ctx, key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
            SSL_CTX_free(ctx);
            close(socket_fd);
            return core::Result<SslConnection>::Err(
                "Failed to load client private key: " + key_path);
        }
        if (SSL_CTX_check_private_key(ctx) != 1) {
            SSL_CTX_free(ctx);
            close(socket_fd);
            return core::Result<SslConnection>::Err(
                "Client certificate/key mismatch");
        }
    }
    
    // Create SSL connection
    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        close(socket_fd);
        return core::Result<SslConnection>::Err("Failed to create SSL object");
    }
    
    SSL_set_fd(ssl, socket_fd);
    
    // Set SNI hostname
    SSL_set_tlsext_host_name(ssl, host.c_str());

    // 🎯 主机名校验 (防止证书有效但域名不匹配的 MITM)
    if (options.ssl_verify) {
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set_hostflags(
            param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (X509_VERIFY_PARAM_set1_host(param, host.c_str(), 0) != 1) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(socket_fd);
            return core::Result<SslConnection>::Err(
                "Failed to set hostname for verification: " + host);
        }
    }
    
    // Perform SSL handshake
    int ret = SSL_connect(ssl);
    if (ret != 1) {
        // ★ 必须带出 OpenSSL 的具体原因。
        //   裸 "SSL handshake failed" 无法区分"证书过期"/"协议版本不匹配"
        //   /"对端不是 TLS 服务"，而这三者的处置完全不同。
        const int ssl_err = SSL_get_error(ssl, ret);
        std::string detail = "SSL handshake failed (SSL_get_error=" +
                             std::to_string(ssl_err) + ")";
        // ERR 队列里可能堆了多条，取最近一条最相关的
        if (unsigned long e = ERR_get_error()) {
            char buf[256] = {0};
            ERR_error_string_n(e, buf, sizeof(buf));
            detail += ": ";
            detail += buf;
        }
        ERR_clear_error();  // ⚠ 不清空会污染后续请求的错误判断
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(socket_fd);
        return core::Result<SslConnection>::Err(detail);
    }

    // 🎯 握手后确认证书校验结果 (SSL_VERIFY_PEER 已强制，此处显式复核)
    if (options.ssl_verify) {
        long verify_result = SSL_get_verify_result(ssl);
        if (verify_result != X509_V_OK) {
            std::string reason =
                X509_verify_cert_error_string(verify_result);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(socket_fd);
            return core::Result<SslConnection>::Err(
                "Certificate verification failed: " + reason);
        }
        // 确认对端确实提供了证书
        X509* peer_cert = SSL_get_peer_certificate(ssl);
        if (!peer_cert) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(socket_fd);
            return core::Result<SslConnection>::Err(
                "Server did not present a certificate");
        }
        X509_free(peer_cert);
    }
    
    SslConnection conn;
    conn.socket_fd = socket_fd;
    conn.ssl_ctx = static_cast<void*>(ctx);
    conn.ssl = static_cast<void*>(ssl);
    
    return core::Result<SslConnection>::Ok(conn);
}

#else

core::Result<HttpClient::SslConnection> HttpClient::connect_ssl(
    const std::string&,
    int,
    int,
    const HttpRequestOptions&
) {
    return core::Result<SslConnection>::Err("SSL not supported");
}

#endif

// ----------------------------------------------------------------------------
// Send Request
// ----------------------------------------------------------------------------

core::Result<void> HttpClient::send_request(
    int socket_fd,
    void* ssl_ptr,
    const std::string& method,
    const std::string& path,
    const std::string& host,
    int port,
    const HttpRequestOptions& options
) {
    std::ostringstream request;
    
    // Request line
    request << method << " " << path << " HTTP/1.1\r\n";
    
    // Host header
    request << "Host: " << host;
    if ((port != 80 && !ssl_ptr) || (port != 443 && ssl_ptr)) {
        request << ":" << port;
    }
    request << "\r\n";
    
    // Default headers
    request << "User-Agent: HunterClaw/1.0\r\n";
    request << "Accept: */*\r\n";
    request << "Connection: close\r\n";
    
    // Custom headers
    for (const auto& [key, value] : options.headers) {
        request << key << ": " << value << "\r\n";
    }
    
    // Content-Length (if body present)
    if (options.body.has_value()) {
        // Add Content-Type if not specified
        if (options.headers.find("Content-Type") == options.headers.end()) {
            request << "Content-Type: application/json\r\n";
        }
        request << "Content-Length: " << options.body->length() << "\r\n";
    }
    
    request << "\r\n";
    
    // Request body
    if (options.body.has_value()) {
        request << *options.body;
    }
    
    std::string request_str = request.str();
    
    // Send via SSL or plain socket
    if (ssl_ptr) {
#ifdef HUNTERCLAW_ENABLE_SSL
        SSL* ssl = static_cast<SSL*>(ssl_ptr);
        int sent = SSL_write(ssl, request_str.c_str(), request_str.length());
        if (sent <= 0) {
            return core::Result<void>::Err("Failed to send SSL request");
        }
#else
        return core::Result<void>::Err("SSL not supported");
#endif
    } else {
        ssize_t sent = send(socket_fd, request_str.c_str(), request_str.length(), 0);
        if (sent < 0 || static_cast<size_t>(sent) != request_str.length()) {
            return core::Result<void>::Err("Failed to send request");
        }
    }
    
    return core::Result<void>::Ok();
}

// ----------------------------------------------------------------------------
// Receive Response
// ----------------------------------------------------------------------------

core::Result<HttpResponse> HttpClient::recv_response(
    int socket_fd,
    void* ssl_ptr,
    int timeout_ms
) {
    std::string response_str;
    char buffer[4096];
    
    // Receive response (with timeout support)
    while (true) {
        // Check for data availability (for plain sockets)
        if (!ssl_ptr) {
            struct pollfd pfd;
            pfd.fd = socket_fd;
            pfd.events = POLLIN;
            
            int ret = poll(&pfd, 1, timeout_ms);
            if (ret == 0) {
                return core::Result<HttpResponse>::Err("Response timeout");
            } else if (ret < 0) {
                return core::Result<HttpResponse>::Err("Poll error");
            }
        }
        
        // Receive data
        ssize_t received;
        if (ssl_ptr) {
#ifdef HUNTERCLAW_ENABLE_SSL
            SSL* ssl = static_cast<SSL*>(ssl_ptr);
            received = SSL_read(ssl, buffer, sizeof(buffer) - 1);
            if (received <= 0) {
                int err = SSL_get_error(ssl, received);
                if (err == SSL_ERROR_ZERO_RETURN) {
                    break;  // Connection closed cleanly
                } else {
                    break;  // Read complete or error
                }
            }
#else
            return core::Result<HttpResponse>::Err("SSL not supported");
#endif
        } else {
            received = recv(socket_fd, buffer, sizeof(buffer) - 1, 0);
            if (received < 0) {
                return core::Result<HttpResponse>::Err("Failed to receive response");
            } else if (received == 0) {
                break;  // Connection closed
            }
        }
        
        buffer[received] = '\0';
        response_str += buffer;
        
        // ✅ 对齐 OC: 完整 HTTP 响应接收 — 支持 Content-Length 和 chunked transfer encoding
        auto header_end_pos = response_str.find("\r\n\r\n");
        if (header_end_pos != std::string::npos) {
            auto headers_part = response_str.substr(0, header_end_pos);
            auto body_start = header_end_pos + 4;
            
            // Check for chunked transfer encoding
            bool is_chunked = false;
            {
                auto te_pos = headers_part.find("Transfer-Encoding:");
                if (te_pos == std::string::npos) te_pos = headers_part.find("transfer-encoding:");
                if (te_pos != std::string::npos) {
                    auto line_end = headers_part.find("\r\n", te_pos);
                    auto te_value = headers_part.substr(te_pos + 18, line_end - te_pos - 18);
                    is_chunked = (te_value.find("chunked") != std::string::npos);
                }
            }
            
            if (is_chunked) {
                // Chunked: read until we see "0\r\n\r\n" terminator
                if (response_str.find("\r\n0\r\n\r\n") != std::string::npos ||
                    response_str.find("\r\n0\r\n") != std::string::npos) {
                    break;
                }
                // Keep reading for more chunks
                continue;
            }
            
            // Check Content-Length
            size_t content_length = 0;
            bool has_content_length = false;
            {
                auto cl_pos = headers_part.find("Content-Length:");
                if (cl_pos == std::string::npos) cl_pos = headers_part.find("content-length:");
                if (cl_pos != std::string::npos) {
                    auto line_end = headers_part.find("\r\n", cl_pos);
                    auto cl_value = headers_part.substr(cl_pos + 15, line_end - cl_pos - 15);
                    try { content_length = std::stoul(cl_value); has_content_length = true; } catch (...) {}
                }
            }
            
            if (has_content_length) {
                // Have Content-Length — check if body is complete
                if (response_str.size() - body_start >= content_length) {
                    break;
                }
                // Keep reading for more body bytes
                continue;
            }
            
            // No Content-Length and not chunked — use simple heuristic
            // (connection close or small delay indicates end)
            break;
        }
    }
    
    if (response_str.empty()) {
        return core::Result<HttpResponse>::Err("Empty response");
    }
    
    // Parse response
    HttpResponse response;
    
    // Split headers and body
    size_t header_end = response_str.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return core::Result<HttpResponse>::Err("Invalid HTTP response (no header end)");
    }
    
    std::string header_str = response_str.substr(0, header_end);
    response.body = response_str.substr(header_end + 4);
    
    // Parse status line
    std::istringstream header_stream(header_str);
    std::string status_line;
    std::getline(header_stream, status_line);
    
    // HTTP/1.1 200 OK
    std::istringstream status_stream(status_line);
    std::string http_version;
    status_stream >> http_version >> response.status_code;
    
    if (status_stream.fail()) {
        return core::Result<HttpResponse>::Err("Invalid status line: " + status_line);
    }
    
    response.ok = (response.status_code >= 200 && response.status_code < 300);
    
    // Parse headers
    std::string header_line;
    while (std::getline(header_stream, header_line) && !header_line.empty()) {
        if (header_line.back() == '\r') {
            header_line.pop_back();
        }
        
        size_t colon = header_line.find(':');
        if (colon != std::string::npos) {
            std::string key = header_line.substr(0, colon);
            std::string value = header_line.substr(colon + 1);
            
            // Trim whitespace
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);
            
            response.headers[key] = value;
        }
    }
    
    return core::Result<HttpResponse>::Ok(response);
}

// ----------------------------------------------------------------------------
// SSE Streaming POST
// ----------------------------------------------------------------------------

core::Result<HttpResponse> HttpClient::post_stream(
    const std::string& url,
    const HttpRequestOptions& options,
    std::function<void(const std::string& line)> line_callback
) {
    // 1. Parse URL
    auto parsed = parse_url(url);
    if (parsed.is_err()) {
        return core::Result<HttpResponse>::Err(parsed.error);
    }
    auto& u = parsed.get();
    
    int socket_fd = -1;
    void* ssl_ctx = nullptr;
    void* ssl = nullptr;
    
    // 2. Create connection
    if (u.use_ssl) {
#ifdef HUNTERCLAW_ENABLE_SSL
        auto ssl_result = connect_ssl(u.host, u.port, options.timeout_ms, options);
        if (ssl_result.is_err()) {
            return core::Result<HttpResponse>::Err(ssl_result.error);
        }
        socket_fd = ssl_result.get().socket_fd;
        ssl_ctx = ssl_result.get().ssl_ctx;
        ssl = ssl_result.get().ssl;
#else
        return core::Result<HttpResponse>::Err("HTTPS not supported (compile with HUNTERCLAW_ENABLE_SSL)");
#endif
    } else {
        auto socket_result = connect_socket(u.host, u.port, options.timeout_ms);
        if (socket_result.is_err()) {
            return core::Result<HttpResponse>::Err(socket_result.error);
        }
        socket_fd = socket_result.get();
    }
    
    // 3. Send request
    auto send_result = send_request(socket_fd, ssl, "POST", u.path, u.host, u.port, options);
    if (send_result.is_err()) {
        close_connection(socket_fd, ssl_ctx, ssl);
        return core::Result<HttpResponse>::Err(send_result.error);
    }
    
    // 4. Receive streaming response — read line-by-line, invoke callback per line
    HttpResponse response;
    response.status_code = 0;
    response.ok = false;
    
    std::string recv_buffer;
    char buf[4096];
    bool headers_done = false;
    std::string accumulated_body;
    
    // Use a longer timeout for streaming (default 120s)
    int stream_timeout_ms = std::max(options.timeout_ms, 120000);
    
    while (true) {
        // Poll for data (timeout per chunk, not total)
        if (!ssl) {
            struct pollfd pfd;
            pfd.fd = socket_fd;
            pfd.events = POLLIN;
            
            int ret = poll(&pfd, 1, stream_timeout_ms);
            if (ret == 0) {
                // Timeout — treat as end of stream
                break;
            } else if (ret < 0) {
                close_connection(socket_fd, ssl_ctx, ssl);
                return core::Result<HttpResponse>::Err("Poll error during streaming");
            }
        }
        
        // Read data
        ssize_t received;
        if (ssl) {
#ifdef HUNTERCLAW_ENABLE_SSL
            SSL* ssl_ptr = static_cast<SSL*>(ssl);
            received = SSL_read(ssl_ptr, buf, sizeof(buf) - 1);
            if (received <= 0) {
                break;  // Connection closed or error
            }
#else
            break;
#endif
        } else {
            received = recv(socket_fd, buf, sizeof(buf) - 1, 0);
            if (received <= 0) {
                break;  // Connection closed or error
            }
        }
        
        buf[received] = '\0';
        recv_buffer.append(buf, received);
        
        // Parse headers first
        if (!headers_done) {
            size_t header_end = recv_buffer.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                headers_done = true;
                std::string header_str = recv_buffer.substr(0, header_end);
                std::string remaining = recv_buffer.substr(header_end + 4);
                recv_buffer = remaining;
                
                // Parse status line
                std::istringstream header_stream(header_str);
                std::string status_line;
                std::getline(header_stream, status_line);
                
                std::istringstream status_stream(status_line);
                std::string http_version;
                status_stream >> http_version >> response.status_code;
                response.ok = (response.status_code >= 200 && response.status_code < 300);
                
                // Parse headers
                std::string header_line;
                while (std::getline(header_stream, header_line) && !header_line.empty()) {
                    if (header_line.back() == '\r') header_line.pop_back();
                    size_t colon = header_line.find(':');
                    if (colon != std::string::npos) {
                        std::string key = header_line.substr(0, colon);
                        std::string value = header_line.substr(colon + 1);
                        value.erase(0, value.find_first_not_of(" \t"));
                        value.erase(value.find_last_not_of(" \t\r\n") + 1);
                        response.headers[key] = value;
                    }
                }
                
                // If HTTP error, collect body and return error
                if (!response.ok) {
                    // Read remaining body
                    accumulated_body = recv_buffer;
                    // Try to read more
                    while (true) {
                        if (!ssl) {
                            struct pollfd pfd2;
                            pfd2.fd = socket_fd;
                            pfd2.events = POLLIN;
                            if (poll(&pfd2, 1, 1000) <= 0) break;
                        }
                        ssize_t r;
                        if (ssl) {
#ifdef HUNTERCLAW_ENABLE_SSL
                            r = SSL_read(static_cast<SSL*>(ssl), buf, sizeof(buf) - 1);
#else
                            r = -1;
#endif
                        } else {
                            r = recv(socket_fd, buf, sizeof(buf) - 1, 0);
                        }
                        if (r <= 0) break;
                        buf[r] = '\0';
                        accumulated_body.append(buf, r);
                    }
                    close_connection(socket_fd, ssl_ctx, ssl);
                    response.body = accumulated_body;
                    return core::Result<HttpResponse>::Err(
                        "HTTP error " + std::to_string(response.status_code) + ": " + accumulated_body);
                }
            } else {
                continue;  // Headers not complete yet
            }
        }
        
        // Process body lines — invoke callback per complete line
        accumulated_body += recv_buffer;
        size_t pos;
        while ((pos = recv_buffer.find('\n')) != std::string::npos) {
            std::string line = recv_buffer.substr(0, pos + 1);
            recv_buffer.erase(0, pos + 1);
            if (line_callback) {
                line_callback(line);
            }
        }
    }
    
    // Process any remaining data in buffer
    if (!recv_buffer.empty() && line_callback) {
        line_callback(recv_buffer);
        accumulated_body += recv_buffer;
    }
    
    close_connection(socket_fd, ssl_ctx, ssl);
    
    response.body = accumulated_body;
    return core::Result<HttpResponse>::Ok(response);
}

// ----------------------------------------------------------------------------
// Interruptible SSE Streaming POST (remote_a2a)
// ----------------------------------------------------------------------------

core::Result<HttpResponse> HttpClient::post_stream_until(
    const std::string& url,
    const HttpRequestOptions& options,
    std::function<bool(const std::string& line)> line_callback,
    int idle_timeout_ms,
    int total_timeout_ms
) {
    auto parsed = parse_url(url);
    if (parsed.is_err()) {
        return core::Result<HttpResponse>::Err(parsed.error);
    }
    auto& u = parsed.get();

    int socket_fd = -1;
    void* ssl_ctx = nullptr;
    void* ssl = nullptr;

    if (u.use_ssl) {
#ifdef HUNTERCLAW_ENABLE_SSL
        auto ssl_result = connect_ssl(u.host, u.port, options.timeout_ms, options);
        if (ssl_result.is_err()) {
            return core::Result<HttpResponse>::Err(ssl_result.error);
        }
        socket_fd = ssl_result.get().socket_fd;
        ssl_ctx = ssl_result.get().ssl_ctx;
        ssl = ssl_result.get().ssl;
#else
        return core::Result<HttpResponse>::Err(
            "HTTPS not supported (compile with HUNTERCLAW_ENABLE_SSL)");
#endif
    } else {
        auto socket_result = connect_socket(u.host, u.port, options.timeout_ms);
        if (socket_result.is_err()) {
            return core::Result<HttpResponse>::Err(socket_result.error);
        }
        socket_fd = socket_result.get();
    }

    auto send_result = send_request(socket_fd, ssl, "POST", u.path, u.host, u.port, options);
    if (send_result.is_err()) {
        close_connection(socket_fd, ssl_ctx, ssl);
        return core::Result<HttpResponse>::Err(send_result.error);
    }

    HttpResponse response;
    response.status_code = 0;
    response.ok = false;

    const int idle_ms = idle_timeout_ms > 0 ? idle_timeout_ms
                                           : std::max(options.timeout_ms, 30000);
    const auto start = std::chrono::steady_clock::now();

    // ★ SSL 路径下 SSL_read 会阻塞, 单纯 poll fd 不足 (数据可能已在 SSL
    //   内部缓冲)。故对 SSL 也设 socket SO_RCVTIMEO, 让 SSL_read 自己超时。
    if (ssl) {
        struct timeval tv;
        tv.tv_sec = idle_ms / 1000;
        tv.tv_usec = (idle_ms % 1000) * 1000;
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    std::string recv_buffer;
    char buf[4096];
    bool headers_done = false;
    std::string accumulated_body;
    bool stopped_by_callback = false;

    auto exceeded_total = [&]() -> bool {
        if (total_timeout_ms <= 0) return false;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        return ms >= total_timeout_ms;
    };

    while (!stopped_by_callback) {
        if (exceeded_total()) break;

        if (!ssl) {
            struct pollfd pfd;
            pfd.fd = socket_fd;
            pfd.events = POLLIN;
            int wait_ms = idle_ms;
            if (total_timeout_ms > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count();
                const int remain = static_cast<int>(total_timeout_ms - elapsed);
                if (remain <= 0) break;
                wait_ms = std::min(wait_ms, remain);
            }
            int ret = poll(&pfd, 1, wait_ms);
            if (ret == 0) break;  // idle 超时 → 交由调用方决定是否转探测
            if (ret < 0) {
                if (errno == EINTR) continue;
                close_connection(socket_fd, ssl_ctx, ssl);
                return core::Result<HttpResponse>::Err("Poll error during streaming");
            }
        }

        ssize_t received;
        if (ssl) {
#ifdef HUNTERCLAW_ENABLE_SSL
            received = SSL_read(static_cast<SSL*>(ssl), buf, sizeof(buf) - 1);
            if (received <= 0) break;
#else
            break;
#endif
        } else {
            received = recv(socket_fd, buf, sizeof(buf) - 1, 0);
            if (received < 0 && errno == EINTR) continue;
            if (received <= 0) break;
        }

        buf[received] = '\0';
        recv_buffer.append(buf, received);

        if (!headers_done) {
            size_t header_end = recv_buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                // ⚠ 防御: 对端只发 header 不发空行时无界增长
                if (recv_buffer.size() > 64 * 1024) {
                    close_connection(socket_fd, ssl_ctx, ssl);
                    return core::Result<HttpResponse>::Err("Response headers too large");
                }
                continue;
            }
            headers_done = true;
            std::string header_str = recv_buffer.substr(0, header_end);
            recv_buffer = recv_buffer.substr(header_end + 4);

            std::istringstream header_stream(header_str);
            std::string status_line;
            std::getline(header_stream, status_line);
            std::istringstream status_stream(status_line);
            std::string http_version;
            status_stream >> http_version >> response.status_code;
            response.ok = (response.status_code >= 200 && response.status_code < 300);

            std::string header_line;
            while (std::getline(header_stream, header_line) && !header_line.empty()) {
                if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
                size_t colon = header_line.find(':');
                if (colon != std::string::npos) {
                    std::string key = header_line.substr(0, colon);
                    std::string value = header_line.substr(colon + 1);
                    value.erase(0, value.find_first_not_of(" \t"));
                    value.erase(value.find_last_not_of(" \t\r\n") + 1);
                    response.headers[key] = value;
                }
            }

            // 非 2xx: 把状态码带回去, 由调用方区分 401/403(补凭据) 与 5xx(重试)
            if (!response.ok) {
                close_connection(socket_fd, ssl_ctx, ssl);
                response.body = recv_buffer;
                return core::Result<HttpResponse>::Ok(response);
            }
        }

        accumulated_body += recv_buffer;
        size_t pos;
        while ((pos = recv_buffer.find('\n')) != std::string::npos) {
            std::string line = recv_buffer.substr(0, pos + 1);
            recv_buffer.erase(0, pos + 1);
            if (line_callback && !line_callback(line)) {
                stopped_by_callback = true;
                break;
            }
        }
    }

    close_connection(socket_fd, ssl_ctx, ssl);
    response.body = accumulated_body;
    return core::Result<HttpResponse>::Ok(response);
}

// ----------------------------------------------------------------------------
// Connection Cleanup
// ----------------------------------------------------------------------------

void HttpClient::close_connection(int socket_fd, void* ssl_ctx_ptr, void* ssl_ptr) {
    if (ssl_ptr) {
#ifdef HUNTERCLAW_ENABLE_SSL
        SSL* ssl = static_cast<SSL*>(ssl_ptr);
        SSL_shutdown(ssl);
        SSL_free(ssl);
#endif
    }
    
    if (ssl_ctx_ptr) {
#ifdef HUNTERCLAW_ENABLE_SSL
        SSL_CTX* ctx = static_cast<SSL_CTX*>(ssl_ctx_ptr);
        SSL_CTX_free(ctx);
#endif
    }
    
    if (socket_fd >= 0) {
        close(socket_fd);
    }
}

}  // namespace infra
}  // namespace hunterclaw
