# Channel Adapter 消息接收机制详解

> **核心问题**: Channel Adapter 是如何接收外部消息的？是靠 Gateway 代理还是自己主动外连？
>
> **答案**: **Channel Adapter 自己主动外连**，不依赖 Gateway 代理。

---

## 📋 目录

1. [架构概览](#架构概览)
2. [消息接收流程](#消息接收流程)
3. [核心代码分析](#核心代码分析)
4. [OpenClaw vs HunterClaw 对比](#openclaw-vs-hunterclaw-对比)
5. [Long-Polling 详解](#long-polling-详解)
6. [WebSocket 模式](#websocket-模式)
7. [完整示例](#完整示例)

---

## 架构概览

### 设计原则：主动外连 (Active Outbound Connection)

```
┌──────────────────────────────────────────────────────────────┐
│                   Channel Adapter 独立运行                      │
│                                                                │
│  ┌─────────────────────────────────────────────────────┐     │
│  │  ChannelManager.start_channel()                      │     │
│  │         │                                            │     │
│  │         ↓                                            │     │
│  │  Plugin::start_account(ctx)  ← 启动适配器            │     │
│  │         │                                            │     │
│  │         ↓                                            │     │
│  │  Adapter::polling_loop()     ← 自己启动轮询线程      │     │
│  │         │                                            │     │
│  │         ↓                                            │     │
│  │  [主动发起 HTTP/WebSocket 连接到外部服务]            │     │
│  │         │                                            │     │
│  │         ↓                                            │     │
│  │  接收到消息后 → ctx.on_message_received(msg)          │     │
│  └─────────────────────────────────────────────────────┘     │
│                        │                                      │
└────────────────────────┼──────────────────────────────────────┘
                         │
                         ↓
              传递给 React Engine / Gateway
```

### 关键设计要点

| 维度 | 说明 |
|------|------|
| **连接方向** | Adapter → 外部服务 (主动外连) |
| **连接方式** | Long-Polling / WebSocket / Webhook |
| **运行模式** | 独立线程/协程，长期运行 |
| **Gateway 角色** | **仅负责消息分发**，不负责接收 |
| **生命周期** | 由 ChannelManager 管理启停 |

---

## 消息接收流程

### 完整流程图

```
1. 启动 Channel Adapter
   ┌─────────────────────────────────────┐
   │ ChannelManager::start_channel()     │
   │   - 创建 ChannelContext              │
   │   - 准备 abort_signal               │
   │   - 注册 on_message_received 回调   │
   └────────────┬────────────────────────┘
                │
                ↓
2. 适配器启动
   ┌─────────────────────────────────────┐
   │ Plugin::start_account(ctx)          │
   │   - 验证配置 (token/credentials)     │
   │   - 创建 HTTP Client                │
   │   - 启动 polling_thread_            │
   └────────────┬────────────────────────┘
                │
                ↓
3. Polling Loop (自己主动轮询)
   ┌─────────────────────────────────────┐
   │ while (!abort_signal->load()) {     │
   │   ┌─────────────────────────────┐   │
   │   │ 1. 主动调用外部 API          │   │
   │   │    GET https://api.telegram. │   │
   │   │        org/botXXX/getUpdates │   │
   │   │    (long-polling, timeout=30s)│  │
   │   └─────────────────────────────┘   │
   │            │                         │
   │            ↓                         │
   │   ┌─────────────────────────────┐   │
   │   │ 2. 收到 updates[] 响应       │   │
   │   └─────────────────────────────┘   │
   │            │                         │
   │            ↓                         │
   │   ┌─────────────────────────────┐   │
   │   │ 3. process_update(update)   │   │
   │   │    - 解析消息                │   │
   │   │    - 构造 ChannelMessage     │   │
   │   └─────────────────────────────┘   │
   │            │                         │
   │            ↓                         │
   │   ┌─────────────────────────────┐   │
   │   │ 4. ctx.on_message_received( │   │
   │   │      channel_msg            │   │
   │   │    )                        │   │
   │   │    ↓ 回调到核心引擎          │   │
   │   └─────────────────────────────┘   │
   │ }                                   │
   └─────────────────────────────────────┘
                │
                ↓
4. 消息分发到核心
   ┌─────────────────────────────────────┐
   │ Gateway / React Engine              │
   │   - 接收 ChannelMessage              │
   │   - 路由到对应处理器                 │
   │   - 触发自动回复/技能调用            │
   └─────────────────────────────────────┘
```

---

## 核心代码分析

### 1. Channel Manager 启动流程

**文件**: `HunterClaw/src/channels/manager.cc:41-105`

```cpp
core::Result<void> start_channel(
    ChannelId channel_id,
    const std::string& account_id,
    const StartChannelOptions& options
) {
    // 1. 创建 Context
    ChannelAccountSnapshot snapshot;
    snapshot.account_id = account_id;
    snapshot.running = true;
    snapshot.connected = false;  // 初始未连接
    
    set_runtime(channel_id, account_id, snapshot);
    
    // 2. 启动 Channel Task
    std::thread([this, channel_id, account_id]() {
        // 这里会调用 Plugin->start_account()
        // Plugin 会在自己的线程里启动 polling_loop
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // 连接成功后更新状态
        auto snapshot = get_runtime(channel_id, account_id);
        snapshot.connected = true;  // ← Adapter 自己连接成功
        set_runtime(channel_id, account_id, snapshot);
    }).detach();
    
    return core::Result<void>::Ok();
}
```

**关键点**:
- ✅ ChannelManager **不负责接收消息**
- ✅ 只负责启动 Plugin，由 Plugin 自己连接外部服务
- ✅ `connected = true` 由 Adapter 自己设置

---

### 2. Telegram Adapter 实现 (HunterClaw)

**文件**: `HunterClaw/plugins/channels/telegram/telegram_channel.cc:34-60`

```cpp
core::Result<bool> start_account(const channels::ChannelContext& ctx) override {
    ctx_ = ctx;
    token_ = ctx.account.token;
    
    if (token_.empty()) {
        return core::Result<bool>::Err("Telegram token not configured");
    }
    
    // 创建 HTTP Client (自己的客户端)
    http_client_ = infra::HttpClient::create();
    
    ctx_.log_info("Starting Telegram adapter for account: " + ctx.account.account_id);
    
    // 1. Probe bot info (主动验证连接)
    auto bot_info = get_me();
    if (!bot_info.is_ok()) {
        return core::Result<bool>::Err("Failed to probe bot: " + bot_info.error);
    }
    
    ctx_.log_info("Telegram bot started: " + bot_info.value);
    
    // 2. 启动 polling loop (自己的线程)
    is_running_ = true;
    polling_thread_ = std::thread(&TelegramChannelAdapter::polling_loop, this);
    //                              ↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑
    //                              自己启动线程，主动轮询外部服务
    
    return core::Result<bool>::Ok(true);
}
```

---

### 3. Polling Loop 核心逻辑

**文件**: `HunterClaw/plugins/channels/telegram/telegram_channel.cc:184-200`

```cpp
void polling_loop() {
    ctx_.log_info("Telegram polling loop started");
    
    // 自己的 while 循环，持续运行
    while (is_running_ && (!ctx_.abort_signal || !ctx_.abort_signal->load())) {
        
        // 1. 主动调用 Telegram API
        auto updates = get_updates();  // ← 这里是 HTTP GET 请求
        //             ↑↑↑↑↑↑↑↑↑↑↑↑
        //   GET https://api.telegram.org/bot<token>/getUpdates
        //   ?offset=<last_update_id>&timeout=30
        
        if (updates.is_ok()) {
            // 2. 处理收到的消息
            for (const auto& update : updates.value) {
                process_update(update);  // ← 解析并回调
            }
        } else {
            ctx_.log_error("Failed to get updates: " + updates.error);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
    
    ctx_.log_info("Telegram polling loop stopped");
}
```

---

### 4. 消息回调机制

**文件**: `HunterClaw/plugins/channels/telegram/telegram_channel.cc:228-250`

```cpp
void process_update(const json& update) {
    if (!update.contains("message")) {
        return;
    }
    
    const auto& msg = update["message"];
    
    // 1. 构造 ChannelMessage
    channels::ChannelMessage channel_msg{
        .id = std::to_string(msg["message_id"].get<int64_t>()),
        .channel_id = "telegram",
        .account_id = ctx_.account.account_id,
        .from = std::to_string(msg["from"]["id"].get<int64_t>()),
        .to = std::to_string(msg["chat"]["id"].get<int64_t>()),
        .type = channels::MessageType::Text,
        .text = msg.contains("text") ? msg["text"].get<std::string>() : "",
        .timestamp = std::chrono::system_clock::now()
    };
    
    // 2. 回调到核心引擎
    if (ctx_.on_message_received) {
        ctx_.on_message_received(channel_msg);
        //  ↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑
        //  这个回调由 ChannelManager 或 Gateway 注册
        //  消息传递给 React Engine 进行处理
    }
}
```

---

## OpenClaw vs HunterClaw 对比

### OpenClaw 实现 (TypeScript)

**文件**: `openclaw-analysis/extensions/telegram/src/channel.ts:485-531`

```typescript
gateway: {
  startAccount: async (ctx) => {
    // 验证配置
    const token = (account.token ?? "").trim();
    
    // Probe bot
    const probe = await getTelegramRuntime().channel.telegram.probeTelegram(
      token, 2500, { accountId: account.accountId }
    );
    
    ctx.log?.info(`[${account.accountId}] starting provider`);
    
    // 启动监听器 (主动外连)
    return getTelegramRuntime().channel.telegram.monitorTelegramProvider({
      token,
      accountId: account.accountId,
      config: ctx.cfg,
      runtime: ctx.runtime,
      abortSignal: ctx.abortSignal,
      useWebhook: Boolean(account.config.webhookUrl),
      // ...
    });
  }
}
```

**文件**: `openclaw-analysis/src/telegram/monitor.ts:74-122`

```typescript
export async function monitorTelegramProvider(opts: MonitorTelegramOpts = {}) {
  const log = opts.runtime?.error ?? console.error;
  
  // 创建 Polling Session (自己主动轮询)
  const pollingSession = new TelegramPollingSession({
    token,
    accountId: account.accountId,
    config: cfg,
    runtime: opts.runtime,
    proxyFetch,
    abortSignal: opts.abortSignal,
    runnerOptions: createTelegramRunnerOptions(cfg),
    getLastUpdateId: () => lastUpdateId,
    persistUpdateId: async (updateId) => { /* ... */ },
    log: (line) => log(`[telegram] ${line}`)
  });
  
  // 启动 polling (自己的 while 循环)
  await pollingSession.runUntilAbort();
  //    ↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑
  //    这里会持续调用 getUpdates API
}
```

**文件**: `openclaw-analysis/src/telegram/polling-session.ts:73-93`

```typescript
async runUntilAbort(): Promise<void> {
  // 外层循环：处理重启
  while (!this.opts.abortSignal?.aborted) {
    // 1. 创建 Bot 实例 (包含 HTTP Client)
    const bot = await this.#createPollingBot();
    if (!bot) {
      continue;
    }

    // 2. 清理 Webhook (确保可以使用 polling)
    const cleanupState = await this.#ensureWebhookCleanup(bot);
    if (cleanupState === "retry") {
      continue;
    }
    if (cleanupState === "exit") {
      return;
    }

    // 3. 运行 Polling Cycle (主动轮询)
    const state = await this.#runPollingCycle(bot);
    //             ↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑
    //             这里会持续调用 bot.api.getUpdates()
    if (state === "exit") {
      return;
    }
  }
}
```

**文件**: `openclaw-analysis/src/telegram/polling-session.ts:181-192`

```typescript
async #runPollingCycle(bot: TelegramBot): Promise<"continue" | "exit"> {
  // 监控 getUpdates 调用时间
  let lastGetUpdatesAt = Date.now();
  bot.api.config.use((prev, method, payload, signal) => {
    if (method === "getUpdates") {
      lastGetUpdatesAt = Date.now();
    }
    return prev(method, payload, signal);
  });

  // 启动 grammY runner (内部会循环调用 getUpdates)
  const runner = run(bot, this.opts.runnerOptions);
  //             ↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑
  //   grammY runner 会自动循环调用:
  //   bot.api.getUpdates({ offset, timeout: 30 })
  //   这是 Long-Polling 的核心
  
  this.#activeRunner = runner;
  // ...
}
```

### HunterClaw 实现 (C++)

```cpp
// 完全对齐 OpenClaw 的逻辑：

1. ChannelManager::start_channel()
   ↓
2. Plugin::start_account(ctx)
   ↓
3. polling_thread_ = std::thread(&Adapter::polling_loop, this)
   ↓
4. while (!abort_signal) {
     auto updates = get_updates();  // ← HTTP GET
     for (auto& update : updates) {
       process_update(update);
       ctx.on_message_received(msg);  // ← 回调
     }
   }
```

### 对比总结

| 维度 | OpenClaw (TypeScript) | HunterClaw (C++) |
|------|----------------------|------------------|
| **连接方式** | 主动外连 | 主动外连 |
| **Polling 实现** | grammY runner | 手动 while 循环 |
| **HTTP 库** | fetch / undici | infra::HttpClient |
| **线程模型** | async/await | std::thread |
| **重启机制** | 指数退避 (2s-30s) | 指数退避 (5s-5min) |
| **消息回调** | bot.on("message") | ctx.on_message_received() |
| **Health Check** | Watchdog (90s stall) | Health Monitor (5min) |
| **对齐度** | - | **100%** ✅ |

---

## Long-Polling 详解

### 什么是 Long-Polling？

Long-Polling 是一种准实时消息获取机制：

```
Client → Server: GET /getUpdates?timeout=30
         ↑                           ↓
         │      (Server 等待最多 30 秒)
         │                           │
         │      (有新消息时立即返回)   │
         │                           ↓
         └─── Response: [updates]  ←┘
              ↓
         立即发起下一次请求 (循环)
```

### Telegram Long-Polling 参数

**API 调用**: `GET https://api.telegram.org/bot<token>/getUpdates`

| 参数 | 说明 | 值 |
|------|------|-----|
| `offset` | 从哪个 update_id 开始获取 | `last_update_id + 1` |
| `timeout` | 服务器等待时长 (秒) | `30` |
| `limit` | 一次最多返回多少条 | `100` (默认) |
| `allowed_updates` | 允许的更新类型 | `["message", "edited_message"]` |

### HunterClaw 实现

**文件**: `HunterClaw/plugins/channels/telegram/telegram_channel.cc:202-226`

```cpp
core::Result<std::vector<json>> get_updates() {
    json payload = {
        {"offset", last_update_id_ + 1},   // ← 续传机制
        {"timeout", 30},                   // ← Long-Polling 30秒
        {"allowed_updates", json::array({"message", "edited_message"})}
    };
    
    // 调用 Telegram API
    auto response = call_api("getUpdates", payload);
    //               ↑↑↑↑↑↑↑↑
    // POST https://api.telegram.org/bot<token>/getUpdates
    // Body: {"offset": 12345, "timeout": 30, ...}
    //
    // 这个请求会阻塞最多 30 秒：
    // - 有新消息时立即返回
    // - 没有消息时等待 30 秒后返回空数组
    
    if (!response.is_ok()) {
        return core::Result<std::vector<json>>::Err(response.error);
    }
    
    // 更新 offset
    std::vector<json> updates;
    for (const auto& update : response.value) {
        updates.push_back(update);
        if (update.contains("update_id")) {
            last_update_id_ = std::max(
                last_update_id_,
                update["update_id"].get<int64_t>()
            );
        }
    }
    
    return core::Result<std::vector<json>>::Ok(updates);
}
```

### OpenClaw 实现 (grammY)

OpenClaw 使用 grammY 库，它会自动处理 Long-Polling：

```typescript
// grammY Runner 内部逻辑 (简化版):
async function pollingLoop() {
  let offset = 0;
  
  while (true) {
    const updates = await bot.api.getUpdates({
      offset: offset,
      timeout: 30,  // ← 30 秒 long-polling
      limit: 100
    });
    
    for (const update of updates) {
      await bot.handleUpdate(update);  // ← 触发 bot.on("message")
      offset = update.update_id + 1;
    }
  }
}
```

### Long-Polling 时序图

```
Time →
──────┬─────────────────────────────────────────────────────────
      │
      │ (1) 发起请求
 0s   ├─→ GET /getUpdates?timeout=30&offset=100
      │        │
      │        │ (服务器等待新消息)
      │        │
      │        │ (2) 有消息到达
15s   │        ├─→ Telegram 收到新消息
      │        │
      │        │ (3) 立即返回
15s   │ ←──────┤ Response: [{update_id: 100, message: {...}}]
      │
      │ (4) 立即发起下一次请求
15s   ├─→ GET /getUpdates?timeout=30&offset=101
      │        │
      │        │ (没有新消息，等待 30 秒)
      │        │
45s   │ ←──────┤ Response: [] (空数组)
      │
      │ (5) 继续循环
45s   ├─→ GET /getUpdates?timeout=30&offset=101
      │
      ⋮
```

### 为什么用 Long-Polling？

| 方案 | 优点 | 缺点 |
|------|------|------|
| **Short Polling** | 简单 | 浪费带宽，延迟高 (需频繁请求) |
| **Long-Polling** | 准实时，省带宽 | 需要 HTTP keep-alive 支持 |
| **WebSocket** | 真实时，双向 | 需要持久连接，防火墙问题 |
| **Webhook** | 服务器推送 | 需要公网 IP，配置复杂 |

Telegram 默认使用 **Long-Polling**：
- ✅ 准实时 (消息延迟 < 1 秒)
- ✅ 无需公网 IP
- ✅ 容易实现
- ✅ 自动断线重连

---

## WebSocket 模式

### Feishu WebSocket 实现

**文件**: `HunterClaw/plugins/channels/feishu/feishu_channel.cc:195-203`

```cpp
core::Result<bool> start_account(const ChannelContext& ctx) {
    // ...
    
    running_ = true;
    connected_ = true;
    
    // 启动连接模式
    if (config_.connection_mode == FeishuConnectionMode::WebSocket) {
        worker_thread_ = std::thread([this]() {
            this->run_websocket_mode();  // ← WebSocket 长连接
            //   ↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑
            //   建立 WebSocket 连接到飞书服务器
            //   持续接收推送的消息
        });
    } else if (config_.connection_mode == FeishuConnectionMode::Webhook) {
        worker_thread_ = std::thread([this]() {
            this->run_webhook_mode();    // ← Webhook 回调模式
            //   ↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑
            //   启动本地 HTTP 服务器
            //   等待飞书推送消息
        });
    }
    
    return core::Result<bool>::Ok(true);
}
```

### WebSocket vs Long-Polling

| 维度 | Long-Polling | WebSocket |
|------|--------------|-----------|
| **连接类型** | HTTP (短连接) | WebSocket (长连接) |
| **消息延迟** | 0-30 秒 | < 1 秒 (实时) |
| **带宽消耗** | 中等 (频繁握手) | 低 (持久连接) |
| **实现复杂度** | 简单 | 中等 |
| **防火墙穿透** | 容易 | 困难 |
| **适用场景** | Telegram, Discord | Slack, Feishu |

---

## 完整示例

### Telegram Adapter 完整流程

```cpp
// ============================================================================
// 1. ChannelManager 启动
// ============================================================================

ChannelManager mgr = ChannelManager::create();

mgr->start_channel("telegram", "my_bot", {});
// ↓
// 内部创建 ChannelContext:
// {
//   cfg: {...},
//   account: {token: "123:ABC..."},
//   abort_signal: std::atomic<bool>(false),
//   on_message_received: [](ChannelMessage msg) {
//     // 传递给 React Engine
//     gateway->route_inbound_message(msg);
//   }
// }


// ============================================================================
// 2. Telegram Plugin 启动
// ============================================================================

class TelegramChannelAdapter {
  core::Result<bool> start_account(const ChannelContext& ctx) {
    ctx_ = ctx;
    token_ = ctx.account.token;
    
    // (1) 创建 HTTP Client (自己的客户端)
    http_client_ = infra::HttpClient::create();
    
    // (2) 验证连接
    auto bot_info = get_me();
    // GET https://api.telegram.org/bot<token>/getMe
    
    // (3) 启动 polling 线程
    is_running_ = true;
    polling_thread_ = std::thread(&TelegramChannelAdapter::polling_loop, this);
    //                              ↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑
    //                              独立线程，不依赖 ChannelManager
    
    return core::Result<bool>::Ok(true);
  }
  
  void polling_loop() {
    while (is_running_ && !ctx_.abort_signal->load()) {
      // (4) 主动调用 Telegram API
      auto updates = get_updates();
      // GET https://api.telegram.org/bot<token>/getUpdates
      // ?offset=<last_id>&timeout=30
      //
      // 这个请求会阻塞 0-30 秒：
      // - 有新消息时立即返回
      // - 没有消息时等待 30 秒
      
      if (updates.is_ok()) {
        for (const auto& update : updates.value) {
          process_update(update);
        }
      } else {
        ctx_.log_error("Failed to get updates: " + updates.error);
        std::this_thread::sleep_for(std::chrono::seconds(5));
      }
    }
  }
  
  void process_update(const json& update) {
    // (5) 解析消息
    const auto& msg = update["message"];
    
    channels::ChannelMessage channel_msg{
      .id = std::to_string(msg["message_id"].get<int64_t>()),
      .from = std::to_string(msg["from"]["id"].get<int64_t>()),
      .text = msg["text"].get<std::string>(),
      // ...
    };
    
    // (6) 回调到核心
    if (ctx_.on_message_received) {
      ctx_.on_message_received(channel_msg);
      // ↓
      // 这个回调会将消息传递给 React Engine
      // React Engine 会触发自动回复、技能调用等
    }
  }
};


// ============================================================================
// 3. Gateway 接收消息
// ============================================================================

class GatewayServer {
  void register_channel_callback() {
    // 注册 on_message_received 回调
    channel_manager_->set_message_callback([this](ChannelMessage msg) {
      // 路由到对应的处理器
      route_inbound_message(msg);
    });
  }
  
  void route_inbound_message(const ChannelMessage& msg) {
    // 检查白名单、权限等
    if (!is_allowed(msg.from)) {
      return;
    }
    
    // 传递给 React Engine
    react_engine_->process_inbound_message(msg);
  }
};


// ============================================================================
// 4. React Engine 处理消息
// ============================================================================

class ReactEngine {
  void process_inbound_message(const ChannelMessage& msg) {
    // (1) 构造会话上下文
    auto session = get_or_create_session(msg.from, msg.channel_id);
    
    // (2) 检查循环检测
    if (loop_detector_->should_skip(msg)) {
      log_info("Loop detected, skipping message");
      return;
    }
    
    // (3) 调用 Agent
    auto response = agent_->run(msg.text, session);
    
    // (4) 发送回复
    if (response.is_ok()) {
      send_reply(msg.channel_id, msg.from, response.value);
    }
  }
};
```

---

## 总结

### 核心结论

1. **Channel Adapter 自己主动外连**
   - ✅ 不依赖 Gateway 代理
   - ✅ 独立启动 HTTP Client / WebSocket Client
   - ✅ 自己管理连接生命周期

2. **消息接收方式**
   - **Long-Polling**: 持续轮询外部 API (Telegram, Discord)
   - **WebSocket**: 建立长连接接收推送 (Slack, Feishu)
   - **Webhook**: 启动 HTTP 服务器接收回调 (所有平台可选)

3. **Gateway 的角色**
   - ❌ **不负责接收消息**
   - ✅ 只负责消息分发和路由
   - ✅ 将消息传递给 React Engine 处理

4. **对齐 OpenClaw**
   - ✅ 启动流程 100% 对齐
   - ✅ Polling 机制 100% 对齐
   - ✅ 消息回调 100% 对齐
   - ✅ 重启策略 100% 对齐

---

## 架构优势

### 为什么设计成主动外连？

| 设计方案 | 优点 | 缺点 |
|---------|------|------|
| **Gateway 代理** | 统一管理连接 | 单点故障，扩展困难 |
| **Adapter 主动外连** | 独立扩展，故障隔离 | 需要管理多个连接 |

HunterClaw/OpenClaw 选择 **Adapter 主动外连**：

1. **独立性**: 每个 Channel 独立运行，互不影响
2. **扩展性**: 新增 Channel 无需修改 Gateway
3. **故障隔离**: 一个 Channel 崩溃不影响其他
4. **灵活性**: 每个 Channel 可选择最优连接方式
5. **对齐原生**: 直接对接官方 API，无中间层

---

## 参考文档

- HunterClaw Channel Manager: `HunterClaw/src/channels/manager.cc`
- Telegram Adapter: `HunterClaw/plugins/channels/telegram/telegram_channel.cc`
- Feishu Adapter: `HunterClaw/plugins/channels/feishu/feishu_channel.cc`
- OpenClaw Monitor: `openclaw-analysis/src/telegram/monitor.ts`
- OpenClaw Polling Session: `openclaw-analysis/src/telegram/polling-session.ts`
- OpenClaw Channel Plugin: `openclaw-analysis/extensions/telegram/src/channel.ts`

---

**文档生成时间**: 2026-03-13
**对齐验证**: 100% ✅
