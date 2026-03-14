#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <memory>
#include <functional>

namespace hunterclaw {
namespace core {

/**
 * WebSocket client interface
 * 
 * Basic WebSocket client for real-time communication.
 * This is a minimal interface - implementations should use a proper WebSocket library.
 */
class WebSocket {
public:
    /**
     * Construct WebSocket with URL
     * @param url WebSocket URL (ws:// or wss://)
     */
    explicit WebSocket(const std::string& url);
    
    ~WebSocket();
    
    // Delete copy
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;
    
    /**
     * Connect to the WebSocket server
     * @return Result indicating success or error
     */
    Result<bool> connect();
    
    /**
     * Close the WebSocket connection
     */
    void close();
    
    /**
     * Send a text message
     * @param message Message to send
     * @return Result indicating success or error
     */
    Result<bool> send(const std::string& message);
    
    /**
     * Receive a message (blocking)
     * @return Result containing the received message or error
     */
    Result<std::string> receive();
    
    /**
     * Check if the connection is open
     * @return true if connected, false otherwise
     */
    bool is_connected() const;
    
    /**
     * Set message callback for async receive
     * @param callback Function to call when message is received
     */
    void set_message_callback(std::function<void(const std::string&)> callback);
    
    /**
     * Set error callback
     * @param callback Function to call on error
     */
    void set_error_callback(std::function<void(const std::string&)> callback);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace core
}  // namespace hunterclaw
