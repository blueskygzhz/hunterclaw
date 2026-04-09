// 🎯 严格对齐 OpenClaw: WebSocket 客户端
// 纯 C++ 实现，使用 POSIX socket + RFC 6455 协议
// 不依赖 Boost、websocketpp 或任何外部 WebSocket 库

#include "hunterclaw/core/websocket.h"
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <cstring>
#include <random>
#include <thread>
#include <atomic>
#include <sstream>
#include <algorithm>
#include <functional>

namespace hunterclaw {
namespace core {

// ============================================================================
// RFC 6455 WebSocket Implementation (no Boost, no external WS library)
// ============================================================================

namespace {

// Base64 encode (for Sec-WebSocket-Key)
std::string base64_encode(const unsigned char* data, size_t len) {
    BIO* bio = BIO_new(BIO_f_base64());
    BIO* bmem = BIO_new(BIO_s_mem());
    bio = BIO_push(bio, bmem);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, data, static_cast<int>(len));
    BIO_flush(bio);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(bio, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(bio);
    return result;
}

// Generate random 16-byte WebSocket key
std::string generate_ws_key() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    unsigned char key[16];
    for (int i = 0; i < 16; ++i) key[i] = static_cast<unsigned char>(dis(gen));
    return base64_encode(key, 16);
}

// Compute Sec-WebSocket-Accept from key (RFC 6455 §4.2.2)
std::string compute_accept_key(const std::string& key) {
    static const char* GUID = "258EAFA5-E914-47DA-95CA-5AB5DC525DB5";
    std::string concat = key + GUID;
    unsigned char sha1[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(concat.data()), concat.size(), sha1);
    return base64_encode(sha1, SHA_DIGEST_LENGTH);
}

// Parse URL into host, port, path
struct ParsedUrl {
    std::string host;
    int port = 80;
    std::string path = "/";
    bool is_wss = false;
};

ParsedUrl parse_ws_url(const std::string& url) {
    ParsedUrl result;
    std::string u = url;
    if (u.substr(0, 6) == "wss://") {
        result.is_wss = true;
        result.port = 443;
        u = u.substr(6);
    } else if (u.substr(0, 5) == "ws://") {
        u = u.substr(5);
    }
    auto path_pos = u.find('/');
    std::string host_port = (path_pos != std::string::npos) ? u.substr(0, path_pos) : u;
    result.path = (path_pos != std::string::npos) ? u.substr(path_pos) : "/";

    auto colon = host_port.rfind(':');
    if (colon != std::string::npos && host_port.find(']') < colon) {
        result.host = host_port.substr(0, colon);
        result.port = std::stoi(host_port.substr(colon + 1));
    } else if (colon != std::string::npos && host_port.find('[') == std::string::npos) {
        result.host = host_port.substr(0, colon);
        result.port = std::stoi(host_port.substr(colon + 1));
    } else {
        result.host = host_port;
    }
    return result;
}

// WebSocket frame opcodes
enum WsOpcode : uint8_t {
    WS_TEXT = 0x1,
    WS_BINARY = 0x2,
    WS_CLOSE = 0x8,
    WS_PING = 0x9,
    WS_PONG = 0xA,
};

// Build masked WebSocket frame (client → server must mask)
std::string build_ws_frame(const std::string& payload, WsOpcode opcode = WS_TEXT) {
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);  // FIN + opcode

    size_t len = payload.size();
    if (len < 126) {
        frame += static_cast<char>(0x80 | len);  // MASK bit + length
    } else if (len < 65536) {
        frame += static_cast<char>(0x80 | 126);
        frame += static_cast<char>((len >> 8) & 0xFF);
        frame += static_cast<char>(len & 0xFF);
    } else {
        frame += static_cast<char>(0x80 | 127);
        for (int i = 7; i >= 0; --i) {
            frame += static_cast<char>((len >> (8 * i)) & 0xFF);
        }
    }

    // Generate 4-byte mask key
    std::random_device rd;
    uint8_t mask[4];
    for (int i = 0; i < 4; ++i) mask[i] = static_cast<uint8_t>(rd() & 0xFF);
    frame.append(reinterpret_cast<char*>(mask), 4);

    // Mask payload
    for (size_t i = 0; i < payload.size(); ++i) {
        frame += static_cast<char>(payload[i] ^ mask[i % 4]);
    }
    return frame;
}

// Parse WebSocket frame from buffer, returns payload and bytes consumed
struct WsFrame {
    WsOpcode opcode;
    std::string payload;
    size_t consumed;
    bool valid;
};

WsFrame parse_ws_frame(const char* data, size_t len) {
    WsFrame frame{WS_TEXT, "", 0, false};
    if (len < 2) return frame;

    uint8_t b0 = static_cast<uint8_t>(data[0]);
    uint8_t b1 = static_cast<uint8_t>(data[1]);
    frame.opcode = static_cast<WsOpcode>(b0 & 0x0F);
    bool masked = (b1 & 0x80) != 0;
    size_t payload_len = b1 & 0x7F;
    size_t offset = 2;

    if (payload_len == 126) {
        if (len < 4) return frame;
        payload_len = (static_cast<size_t>(static_cast<uint8_t>(data[2])) << 8) |
                       static_cast<size_t>(static_cast<uint8_t>(data[3]));
        offset = 4;
    } else if (payload_len == 127) {
        if (len < 10) return frame;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | static_cast<uint8_t>(data[2 + i]);
        }
        offset = 10;
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
        if (len < offset + 4) return frame;
        std::memcpy(mask_key, data + offset, 4);
        offset += 4;
    }

    if (len < offset + payload_len) return frame;

    frame.payload.resize(payload_len);
    for (size_t i = 0; i < payload_len; ++i) {
        frame.payload[i] = data[offset + i] ^ (masked ? mask_key[i % 4] : 0);
    }
    frame.consumed = offset + payload_len;
    frame.valid = true;
    return frame;
}

} // namespace

// ============================================================================
// WebSocket::Impl — POSIX socket based implementation
// ============================================================================

class WebSocket::Impl {
public:
    explicit Impl(const std::string& url)
        : url_(url), fd_(-1), connected_(false), running_(false) {}

    ~Impl() {
        close();
    }

    std::string url_;
    int fd_;
    std::atomic<bool> connected_;
    std::atomic<bool> running_;
    std::thread recv_thread_;
    std::function<void(const std::string&)> message_callback_;
    std::function<void(const std::string&)> error_callback_;
    std::string recv_buffer_;

    void close() {
        running_ = false;
        connected_ = false;
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
        if (recv_thread_.joinable()) {
            recv_thread_.join();
        }
    }
};

WebSocket::WebSocket(const std::string& url)
    : impl_(std::make_unique<Impl>(url)) {
    auto log = spdlog::get("websocket");
    if (log) log->info("[WebSocket] Creating connection for URL: {}", url);
}

WebSocket::~WebSocket() {
    if (impl_ && impl_->connected_) {
        close();
    }
}

Result<bool> WebSocket::connect() {
    auto log = spdlog::get("websocket");
    auto parsed = parse_ws_url(impl_->url_);

    if (parsed.is_wss) {
        return Result<bool>::Err("WSS (TLS) not supported in POSIX-only mode; use ws:// or add TLS wrapper");
    }

    // DNS resolve
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(parsed.port);

    int err = getaddrinfo(parsed.host.c_str(), port_str.c_str(), &hints, &res);
    if (err != 0) {
        return Result<bool>::Err(std::string("DNS resolve failed: ") + gai_strerror(err));
    }

    // Create socket and connect
    impl_->fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (impl_->fd_ < 0) {
        freeaddrinfo(res);
        return Result<bool>::Err("socket() failed");
    }

    if (::connect(impl_->fd_, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        ::close(impl_->fd_);
        impl_->fd_ = -1;
        return Result<bool>::Err("TCP connect failed to " + parsed.host + ":" + port_str);
    }
    freeaddrinfo(res);

    // WebSocket upgrade handshake (RFC 6455 §4.1)
    std::string ws_key = generate_ws_key();
    std::string expected_accept = compute_accept_key(ws_key);

    std::ostringstream req;
    req << "GET " << parsed.path << " HTTP/1.1\r\n";
    req << "Host: " << parsed.host << ":" << parsed.port << "\r\n";
    req << "Upgrade: websocket\r\n";
    req << "Connection: Upgrade\r\n";
    req << "Sec-WebSocket-Key: " << ws_key << "\r\n";
    req << "Sec-WebSocket-Version: 13\r\n";
    req << "\r\n";

    std::string request = req.str();
    if (::send(impl_->fd_, request.data(), request.size(), 0) < 0) {
        ::close(impl_->fd_);
        impl_->fd_ = -1;
        return Result<bool>::Err("Failed to send WS handshake");
    }

    // Read HTTP response
    char buf[4096];
    std::string response;
    while (true) {
        ssize_t n = ::recv(impl_->fd_, buf, sizeof(buf), 0);
        if (n <= 0) {
            ::close(impl_->fd_);
            impl_->fd_ = -1;
            return Result<bool>::Err("WS handshake response read failed");
        }
        response.append(buf, n);
        if (response.find("\r\n\r\n") != std::string::npos) break;
    }

    // Validate 101 response
    if (response.find("HTTP/1.1 101") == std::string::npos) {
        ::close(impl_->fd_);
        impl_->fd_ = -1;
        return Result<bool>::Err("WS handshake failed: expected 101, got: " +
                                  response.substr(0, std::min<size_t>(80, response.size())));
    }

    // Validate Sec-WebSocket-Accept
    auto accept_pos = response.find("Sec-WebSocket-Accept: ");
    if (accept_pos != std::string::npos) {
        auto end_pos = response.find("\r\n", accept_pos);
        std::string accept_val = response.substr(accept_pos + 22, end_pos - accept_pos - 22);
        if (accept_val != expected_accept) {
            if (log) log->warn("[WebSocket] Accept key mismatch (non-fatal)");
        }
    }

    impl_->connected_ = true;
    impl_->running_ = true;

    // Save any data after headers for later frame parsing
    auto header_end = response.find("\r\n\r\n") + 4;
    if (header_end < response.size()) {
        impl_->recv_buffer_ = response.substr(header_end);
    }

    // Start receive thread
    impl_->recv_thread_ = std::thread([this]() {
        char buf[65536];
        while (impl_->running_ && impl_->fd_ >= 0) {
            ssize_t n = ::recv(impl_->fd_, buf, sizeof(buf), 0);
            if (n <= 0) {
                impl_->connected_ = false;
                impl_->running_ = false;
                if (impl_->error_callback_) {
                    impl_->error_callback_("Connection closed");
                }
                break;
            }
            impl_->recv_buffer_.append(buf, n);

            // Parse frames from buffer
            while (impl_->recv_buffer_.size() >= 2) {
                auto frame = parse_ws_frame(impl_->recv_buffer_.data(), impl_->recv_buffer_.size());
                if (!frame.valid) break;

                impl_->recv_buffer_.erase(0, frame.consumed);

                switch (frame.opcode) {
                    case WS_TEXT:
                    case WS_BINARY:
                        if (impl_->message_callback_) {
                            impl_->message_callback_(frame.payload);
                        }
                        break;
                    case WS_PING: {
                        // Auto-respond with PONG
                        auto pong = build_ws_frame(frame.payload, WS_PONG);
                        ::send(impl_->fd_, pong.data(), pong.size(), MSG_NOSIGNAL);
                        break;
                    }
                    case WS_CLOSE:
                        impl_->connected_ = false;
                        impl_->running_ = false;
                        break;
                    default:
                        break;
                }
            }
        }
    });

    if (log) log->info("[WebSocket] Connected to {}", impl_->url_);
    return Result<bool>::Ok(true);
}

void WebSocket::close() {
    if (!impl_) return;

    if (impl_->connected_ && impl_->fd_ >= 0) {
        // Send close frame
        auto close_frame = build_ws_frame("", WS_CLOSE);
        ::send(impl_->fd_, close_frame.data(), close_frame.size(), MSG_NOSIGNAL);
    }
    impl_->close();
}

Result<bool> WebSocket::send(const std::string& message) {
    if (!impl_->connected_ || impl_->fd_ < 0) {
        return Result<bool>::Err("WebSocket not connected");
    }

    auto frame = build_ws_frame(message, WS_TEXT);
    ssize_t sent = ::send(impl_->fd_, frame.data(), frame.size(), MSG_NOSIGNAL);
    if (sent < 0) {
        impl_->connected_ = false;
        return Result<bool>::Err("WebSocket send failed");
    }
    return Result<bool>::Ok(true);
}

Result<std::string> WebSocket::receive() {
    // Synchronous receive not recommended; use set_message_callback instead
    if (!impl_->connected_) {
        return Result<std::string>::Err("WebSocket not connected");
    }

    // Block-read one frame
    char buf[65536];
    ssize_t n = ::recv(impl_->fd_, buf, sizeof(buf), 0);
    if (n <= 0) {
        impl_->connected_ = false;
        return Result<std::string>::Err("Connection closed during receive");
    }

    std::string data(buf, n);
    auto frame = parse_ws_frame(data.data(), data.size());
    if (!frame.valid) {
        return Result<std::string>::Err("Invalid WebSocket frame");
    }
    return Result<std::string>::Ok(frame.payload);
}

bool WebSocket::is_connected() const {
    return impl_->connected_;
}

void WebSocket::set_message_callback(std::function<void(const std::string&)> callback) {
    impl_->message_callback_ = std::move(callback);
}

void WebSocket::set_error_callback(std::function<void(const std::string&)> callback) {
    impl_->error_callback_ = std::move(callback);
}

}  // namespace core
}  // namespace hunterclaw
