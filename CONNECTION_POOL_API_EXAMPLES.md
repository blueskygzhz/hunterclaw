# 🎯 ConnectionPool API 使用示例

完整展示 HunterClaw ConnectionPool 与 OpenClaw 严格对齐的 API 使用方法。

---

## 📑 目录

1. [基础使用](#1-基础使用)
2. [广播方式](#2-广播方式)
3. [订阅系统](#3-订阅系统)
4. [慢速消费者保护](#4-慢速消费者保护)
5. [权限控制](#5-权限控制)
6. [统计监控](#6-统计监控)
7. [实战场景](#7-实战场景)

---

## 1. 基础使用

### 1.1 获取单例实例

```cpp
#include "hunterclaw/gateway/connection_pool.h"

using namespace hunterclaw::gateway;

// 获取全局单例
auto& pool = ConnectionPool::instance();
```

---

### 1.2 添加客户端

```cpp
// 创建 WebSocket 连接
auto ws_conn = std::make_shared<WebSocketConnectionImpl>(...);

// 创建客户端
auto client = std::make_shared<WsClient>();
client->conn_id = "conn-abc-123";
client->socket = ws_conn;
client->role = "operator";
client->scopes = {"agent.read", "agent.write"};
client->connect_info.session_key = "sess-456";
client->connect_info.platform = "web";
client->connected_at = std::chrono::steady_clock::now();
client->last_activity = std::chrono::steady_clock::now();

// 添加到连接池
pool.add_client(client);

std::cout << "Total clients: " << pool.client_count() << std::endl;
```

---

### 1.3 移除客户端

```cpp
// 移除指定客户端
pool.remove_client("conn-abc-123");
```

---

### 1.4 查询客户端

```cpp
// 根据 conn_id 查询
auto client = pool.get_client("conn-abc-123");
if (client) {
    std::cout << "Client role: " << client->role << std::endl;
}

// 获取所有客户端
auto all_clients = pool.get_all_clients();
for (const auto& c : all_clients) {
    std::cout << "Client: " << c->conn_id << std::endl;
}
```

---

## 2. 广播方式

### 2.1 方式 1: 全局广播

发送给所有已连接客户端:

```cpp
using json = nlohmann::json;

// 广播给所有客户端
pool.broadcast("agent.text", {
    {"sessionId", "sess-456"},
    {"text", "Hello, World!"},
    {"timestamp", 1710570000000}
});
```

**输出帧** (JSON-RPC 2.0):
```json
{
  "jsonrpc": "2.0",
  "method": "agent.text",
  "params": {
    "sessionId": "sess-456",
    "text": "Hello, World!",
    "timestamp": 1710570000000
  }
}
```

---

### 2.2 方式 2: 定向广播

发送给指定客户端:

```cpp
// 只发送给指定的 2 个客户端
std::set<std::string> targets = {"conn-abc", "conn-def"};

pool.broadcast_to("notification", {
    {"title", "New Message"},
    {"body", "You have a new message"}
}, targets);
```

---

### 2.3 方式 3: 条件广播

发送给满足条件的客户端:

```cpp
// 只发送给移动端客户端
pool.broadcast_if("mobile_update", {
    {"version", "2.0.0"},
    {"url", "https://example.com/app.apk"}
}, [](const WsClient& client) {
    return client.connect_info.platform == "mobile";
});

// 只发送给有特定 scope 的客户端
pool.broadcast_if("admin.command", {
    {"action", "restart"}
}, [](const WsClient& client) {
    auto& scopes = client.scopes;
    return std::find(scopes.begin(), scopes.end(), "admin") != scopes.end();
});
```

---

### 2.4 使用 BroadcastOptions

```cpp
// 启用慢速消费者保护
pool.broadcast("agent.text", payload, {
    .drop_if_slow = true,  // 跳过慢速客户端
    .seq = 12345           // 事件序列号
});
```

---

## 3. 订阅系统

### 3.1 订阅事件 (精确匹配)

```cpp
// 订阅精确事件
uint64_t sub_id = pool.subscribe(
    "agent.text",
    [](const json& payload) {
        std::cout << "Received: " << payload["text"] << std::endl;
    }
);
```

---

### 3.2 订阅事件 (通配符匹配)

```cpp
// 订阅所有 Agent 事件 (agent.*)
uint64_t sub_id = pool.subscribe(
    "agent.*",
    [](const json& payload) {
        std::cout << "Agent event: " << payload.dump() << std::endl;
    },
    this  // owner 指针,用于批量取消订阅
);

// 支持的通配符模式:
// - "*"             → 所有事件
// - "agent.*"       → agent.text, agent.tool_start, etc.
// - "*.error"       → agent.error, tool.error, etc.
// - "agent.*.error" → agent.llm.error, agent.tool.error, etc.
```

---

### 3.3 取消订阅

```cpp
// 方式 1: 根据订阅 ID 取消
pool.unsubscribe(sub_id);

// 方式 2: 批量取消 (根据 owner)
pool.unsubscribe_by_owner(this);
```

---

### 3.4 RAII 风格订阅

```cpp
class EventListener {
public:
    EventListener() {
        sub_id_ = ConnectionPool::instance().subscribe(
            "agent.*",
            [this](const json& payload) {
                handle_event(payload);
            },
            this  // owner
        );
    }
    
    ~EventListener() {
        // 自动取消订阅
        ConnectionPool::instance().unsubscribe_by_owner(this);
    }
    
private:
    void handle_event(const json& payload) {
        std::cout << "Event: " << payload.dump() << std::endl;
    }
    
    uint64_t sub_id_;
};
```

---

## 4. 慢速消费者保护

### 4.1 启用慢速保护

```cpp
// 在广播时启用
pool.broadcast("agent.text", payload, {
    .drop_if_slow = true  // 🎯 关键参数
});
```

**效果**:
- 正常客户端: ✅ 正常接收事件
- 慢速客户端: ⚠️ 跳过,事件丢弃
- 慢速客户端累计 10 次: ❌ 自动断连

---

### 4.2 慢速检测原理

```cpp
// 内部检测逻辑
bool is_slow = is_send_buffer_full(client);

// 检测条件:
// 1. 发送队列 > 1MB
// 2. 连接已断开
```

**模拟慢速客户端**:
```cpp
client->send_queue_size = 2 * 1024 * 1024;  // 2MB (超过阈值)
```

---

### 4.3 慢速统计

```cpp
auto stats = pool.get_statistics();
std::cout << "Events dropped: " << stats.events_dropped << std::endl;
std::cout << "Slow consumers disconnected: " 
          << stats.slow_consumers_disconnected << std::endl;
```

---

## 5. 权限控制

### 5.1 配置事件权限

```cpp
// 配置哪些事件需要特定 scope (内部实现)
event_scope_guards_["admin.command"] = {"admin", "super_admin"};
event_scope_guards_["agent.debug"] = {"developer", "admin"};
```

---

### 5.2 客户端 Scope 配置

```cpp
// 客户端 1: 管理员
auto admin_client = std::make_shared<WsClient>();
admin_client->scopes = {"admin", "agent.read", "agent.write"};

// 客户端 2: 普通用户
auto user_client = std::make_shared<WsClient>();
user_client->scopes = {"agent.read"};

// 客户端 3: 超级用户 (所有权限)
auto super_client = std::make_shared<WsClient>();
super_client->scopes = {"*"};
```

---

### 5.3 权限检查示例

```cpp
// 广播管理员命令
pool.broadcast("admin.command", {
    {"action", "restart"}
});

// 结果:
// - admin_client:  ✅ 接收 (有 admin scope)
// - user_client:   ❌ 跳过 (无 admin scope)
// - super_client:  ✅ 接收 (有 * scope)
```

---

## 6. 统计监控

### 6.1 获取统计信息

```cpp
auto stats = pool.get_statistics();

std::cout << "Total clients: " << stats.total_clients << std::endl;
std::cout << "Events sent: " << stats.events_sent << std::endl;
std::cout << "Events dropped: " << stats.events_dropped << std::endl;
std::cout << "Slow consumers disconnected: " 
          << stats.slow_consumers_disconnected << std::endl;

// 按事件类型统计
for (const auto& [event, count] : stats.events_by_type) {
    std::cout << "  " << event << ": " << count << std::endl;
}
```

---

### 6.2 重置统计

```cpp
pool.reset_statistics();
```

---

## 7. 实战场景

### 7.1 场景: Agent 流式输出

```cpp
class ReactEngine {
public:
    void run(const ChatRequest& req) {
        auto& pool = ConnectionPool::instance();
        
        // 开始推送
        pool.broadcast("agent.start", {
            {"sessionId", session_id_},
            {"timestamp", std::time(nullptr)}
        });
        
        // 流式输出
        llm_client_->stream_chat(req, [&](const StreamChunk& chunk) {
            pool.broadcast("agent.text", {
                {"sessionId", session_id_},
                {"text", chunk.content}
            }, { .drop_if_slow = true });  // 🎯 防止慢速阻塞
        });
        
        // 工具调用
        pool.broadcast("tool.start", {
            {"toolName", "read_file"},
            {"args", {{"path", "/etc/hosts"}}}
        });
        
        // 工具结果
        pool.broadcast("tool.result", {
            {"toolName", "read_file"},
            {"result", file_content}
        });
        
        // 完成
        pool.broadcast("agent.finish", {
            {"sessionId", session_id_},
            {"timestamp", std::time(nullptr)}
        });
    }
    
private:
    std::string session_id_;
};
```

---

### 7.2 场景: Discord Channel 订阅

```cpp
class DiscordChannel {
public:
    DiscordChannel(const std::string& webhook_url)
        : webhook_url_(webhook_url) {
        
        auto& pool = ConnectionPool::instance();
        
        // 订阅所有 Agent 文本输出
        sub_id_ = pool.subscribe(
            "agent.text",
            [this](const json& payload) {
                send_to_discord(payload["text"]);
            },
            this
        );
    }
    
    ~DiscordChannel() {
        ConnectionPool::instance().unsubscribe_by_owner(this);
    }
    
private:
    void send_to_discord(const std::string& text) {
        // 发送到 Discord Webhook
        json discord_msg = {
            {"content", text}
        };
        
        // HTTP POST to webhook_url_
        http_client_.post(webhook_url_, discord_msg.dump());
    }
    
    std::string webhook_url_;
    uint64_t sub_id_;
};
```

---

### 7.3 场景: 实时监控面板

```cpp
class MonitoringDashboard {
public:
    void start() {
        auto& pool = ConnectionPool::instance();
        
        // 订阅所有事件
        pool.subscribe("*", [this](const json& payload) {
            update_dashboard(payload);
        }, this);
        
        // 定期更新统计
        timer_.async_wait([this](const boost::system::error_code&) {
            update_statistics();
        });
    }
    
private:
    void update_dashboard(const json& event) {
        // 更新实时事件流
        event_stream_.push(event);
        
        // 通知所有监控客户端
        ConnectionPool::instance().broadcast_if(
            "monitor.update",
            event,
            [](const WsClient& client) {
                return client.role == "monitor";
            }
        );
    }
    
    void update_statistics() {
        auto stats = ConnectionPool::instance().get_statistics();
        
        ConnectionPool::instance().broadcast_to_role(
            "monitor",
            "monitor.stats",
            {
                {"clients", stats.total_clients},
                {"events_sent", stats.events_sent},
                {"events_dropped", stats.events_dropped}
            }
        );
    }
    
    boost::asio::steady_timer timer_;
    std::queue<json> event_stream_;
};
```

---

### 7.4 场景: 会话隔离广播

```cpp
// 只广播给同一个 session 的客户端
void broadcast_to_session(const std::string& session_id,
                         const std::string& event,
                         const json& payload) {
    ConnectionPool::instance().broadcast_if(
        event,
        payload,
        [&session_id](const WsClient& client) {
            return client.connect_info.session_key == session_id;
        }
    );
}

// 使用
broadcast_to_session("sess-456", "agent.text", {
    {"text", "Private message for this session"}
});
```

---

### 7.5 场景: 多平台推送

```cpp
void push_notification(const std::string& title,
                      const std::string& body) {
    auto& pool = ConnectionPool::instance();
    
    // Web 端
    pool.broadcast_if("notification", {
        {"title", title},
        {"body", body},
        {"type", "web"}
    }, [](const WsClient& c) {
        return c.connect_info.platform == "web";
    });
    
    // 移动端
    pool.broadcast_if("notification", {
        {"title", title},
        {"body", body},
        {"type", "mobile"}
    }, [](const WsClient& c) {
        return c.connect_info.platform == "mobile";
    });
    
    // 桌面端
    pool.broadcast_if("notification", {
        {"title", title},
        {"body", body},
        {"type", "desktop"}
    }, [](const WsClient& c) {
        return c.connect_info.platform == "desktop";
    });
}
```

---

## 🎯 最佳实践

### 1. 总是启用慢速保护

```cpp
// ✅ 推荐
pool.broadcast(event, payload, { .drop_if_slow = true });

// ❌ 避免 (可能阻塞所有客户端)
pool.broadcast(event, payload);
```

---

### 2. 使用 RAII 管理订阅

```cpp
// ✅ 推荐
class MyComponent {
public:
    MyComponent() {
        sub_id_ = pool.subscribe("event", callback, this);
    }
    ~MyComponent() {
        pool.unsubscribe_by_owner(this);
    }
};
```

---

### 3. 合理使用通配符

```cpp
// ✅ 推荐: 精确订阅
pool.subscribe("agent.text", callback);

// ⚠️ 谨慎使用: 订阅所有事件 (可能产生大量回调)
pool.subscribe("*", callback);
```

---

### 4. 监控统计信息

```cpp
// 定期检查
auto stats = pool.get_statistics();
if (stats.events_dropped > 100) {
    logger->warn("Too many events dropped!");
}
```

---

### 5. 异常处理

```cpp
try {
    pool.broadcast(event, payload);
} catch (const std::exception& e) {
    logger->error("Broadcast failed: {}", e.what());
}
```

---

## 📚 参考文档

- [ConnectionPool 严格对齐报告](CONNECTION_POOL_STRICT_ALIGNMENT.md)
- [ConnectionPool OpenClaw 对齐分析](CONNECTION_POOL_OPENCLAW_ALIGNMENT.md)
- [HunterClaw Gateway 架构](GATEWAY_ARCHITECTURE.md)

---

**最后更新**: 2026-03-16  
**版本**: 1.0  
**状态**: ✅ 完整
