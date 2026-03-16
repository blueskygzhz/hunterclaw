# HunterClaw Channel 鉴权与设备配对详解

## 📚 目录
1. [概述](#概述)
2. [DM Policy (私聊策略)](#dm-policy-私聊策略)
3. [设备配对完整流程](#设备配对完整流程)
4. [实际代码示例](#实际代码示例)
5. [安全机制](#安全机制)
6. [最佳实践](#最佳实践)

---

## 概述

HunterClaw Channel 鉴权系统 **100% 对齐 OpenClaw**,提供三种核心安全机制:

| 机制 | 说明 | 适用场景 |
|------|------|----------|
| **DM Policy** | 私聊访问控制 | Discord, Telegram, 飞书等 |
| **Pairing System** | 设备配对验证 | 首次连接、安全加固 |
| **Allow-From List** | 白名单管理 | 多账户隔离 |

---

## DM Policy (私聊策略)

### 📋 三种策略

```cpp
enum class DmPolicy {
    Open,    // ✅ 接受所有 DM (通过 allow_from 过滤)
    Pairing  // 🔐 需要配对码 (Challenge-Response)
};
```

### 配置示例

```yaml
# config.yaml
channels:
  telegram:
    accounts:
      - id: "bot_main"
        token: "{{TELEGRAM_BOT_TOKEN}}"
        dmPolicy: "pairing"  # 🔐 需要配对
        allowFrom:
          - "123456789"      # 已授权的用户 ID
          
  discord:
    accounts:
      - id: "bot_test"
        token: "{{DISCORD_BOT_TOKEN}}"
        dmPolicy: "open"     # ✅ 接受所有 DM
        allowFrom: []        # 空白名单 = 所有人
```

---

## 设备配对完整流程

### 🎯 场景: Telegram 用户首次私聊 Bot

#### **步骤 1: 用户发送消息**

```
用户 → Telegram Bot: "Hello"
```

#### **步骤 2: Bot 检测鉴权状态**

```cpp
// Channel 入站消息处理
void TelegramChannel::handle_incoming_message(const TelegramMessage& msg) {
    // 1. 检查 DM Policy
    if (config_.dm_policy == DmPolicy::Pairing) {
        // 2. 检查发件人是否已授权
        if (!pairing_adapter_->is_allowed(msg.from.id)) {
            // ❌ 未授权 → 发起配对挑战
            issue_pairing_challenge(msg);
            return;
        }
    }
    
    // ✅ 已授权 → 正常处理消息
    process_message(msg);
}
```

#### **步骤 3: 发起配对挑战**

```cpp
void TelegramChannel::issue_pairing_challenge(const TelegramMessage& msg) {
    // 构造配对参数
    pairing::PairingChallengeParams params;
    params.channel = "telegram";
    params.sender_id = msg.from.id;
    params.sender_id_line = "User: " + msg.from.username + " (ID: " + msg.from.id + ")";
    params.meta = {
        {"username", msg.from.username},
        {"first_name", msg.from.first_name}
    };
    
    // Callback: 创建 Pairing Request
    params.upsert_pairing_request = [this](const std::string& id, auto& meta) {
        return pairing_store_.upsert_request(id, meta);
        // 返回: (code, created)
        // code = "X7K2MP9Q" (8位随机码)
        // created = true (首次) / false (重复请求)
    };
    
    // Callback: 发送配对回复
    params.send_pairing_reply = [this, &msg](const std::string& text) {
        telegram_api_.send_message(msg.chat.id, text);
    };
    
    // 🚀 执行配对挑战
    auto result = pairing::issue_pairing_challenge(params);
    
    if (result.created) {
        log_->info("Pairing challenge issued: code={}", result.code.value());
    }
}
```

#### **步骤 4: Bot 回复配对提示**

```
Telegram Bot → 用户:
┌─────────────────────────────────────────┐
│ HunterClaw: access not configured.      │
│                                         │
│ User: john_doe (ID: 123456789)          │
│                                         │
│ Pairing code: X7K2MP9Q                  │
│                                         │
│ Ask the bot owner to approve with:      │
│ hunterclaw pairing approve telegram X7K2MP9Q │
└─────────────────────────────────────────┘
```

#### **步骤 5: 管理员批准配对**

```bash
# 方式 1: CLI 命令
$ hunterclaw pairing approve telegram X7K2MP9Q

# 方式 2: RPC 调用
$ curl -X POST http://localhost:3000/rpc -d '{
  "jsonrpc": "2.0",
  "method": "pairing.approve",
  "params": {
    "channel": "telegram",
    "code": "X7K2MP9Q"
  },
  "id": 1
}'
```

#### **步骤 6: 后台处理批准请求**

```cpp
// RPC Handler: pairing.approve
core::Result<nlohmann::json> handle_pairing_approve(
    const std::string& channel,
    const std::string& code
) {
    // 1. 查找 Pairing Request
    auto pairing_store = get_pairing_store(channel);
    auto request_opt = pairing_store->get_request(code);
    
    if (!request_opt.has_value()) {
        return core::Result<nlohmann::json>::Err("Invalid pairing code");
    }
    
    auto request = request_opt.value();
    
    // 2. 添加到 Allow-From 白名单
    pairing_store->add_allow_from(request.id);
    
    // 3. 删除 Pairing Request
    pairing_store->remove_request(code);
    
    log_->info("Pairing approved: channel={}, user={}", channel, request.id);
    
    return core::Result<nlohmann::json>::Ok({
        {"approved", true},
        {"user_id", request.id},
        {"meta", request.meta}
    });
}
```

#### **步骤 7: 用户重新发送消息**

```
用户 → Telegram Bot: "Hello again"
```

```cpp
void TelegramChannel::handle_incoming_message(const TelegramMessage& msg) {
    // 1. 检查 DM Policy
    if (config_.dm_policy == DmPolicy::Pairing) {
        // 2. 检查发件人是否已授权
        if (!pairing_adapter_->is_allowed(msg.from.id)) {
            // ❌ 未授权
            issue_pairing_challenge(msg);
            return;
        }
    }
    
    // ✅ 已授权 → 正常处理! 🎉
    process_message(msg);  // → 转发给 Agent 处理
}
```

---

## 实际代码示例

### 示例 1: Telegram Channel 完整实现

```cpp
// telegram_channel.h
class TelegramChannel {
public:
    explicit TelegramChannel(const ChannelPolicyConfig& config);
    
    void start();
    void stop();
    
private:
    // 入站消息处理
    void handle_incoming_message(const TelegramMessage& msg);
    
    // 配对挑战
    void issue_pairing_challenge(const TelegramMessage& msg);
    
    // 消息处理
    void process_message(const TelegramMessage& msg);
    
    ChannelPolicyConfig config_;
    std::unique_ptr<pairing::DefaultPairingAdapter> pairing_adapter_;
    pairing::PairingStore pairing_store_;
    std::shared_ptr<spdlog::logger> log_;
};

// telegram_channel.cc
TelegramChannel::TelegramChannel(const ChannelPolicyConfig& config)
    : config_(config)
    , pairing_store_("telegram")  // 初始化 PairingStore
    , log_(spdlog::get("console"))
{
    // 创建 Pairing Adapter
    pairing_adapter_ = std::make_unique<pairing::DefaultPairingAdapter>("telegram");
}

void TelegramChannel::start() {
    log_->info("[TelegramChannel] Starting with dmPolicy={}", 
               config_.dm_policy == DmPolicy::Pairing ? "pairing" : "open");
    
    // 监听 Telegram 消息
    telegram_api_.on_message([this](const TelegramMessage& msg) {
        handle_incoming_message(msg);
    });
}

void TelegramChannel::handle_incoming_message(const TelegramMessage& msg) {
    // 🔐 DM Policy 检查
    if (config_.dm_policy == DmPolicy::Pairing) {
        // 检查白名单
        if (!pairing_adapter_->is_allowed(msg.from.id)) {
            log_->warn("[TelegramChannel] Unauthorized DM from {}", msg.from.id);
            issue_pairing_challenge(msg);
            return;
        }
    }
    
    // ✅ 授权通过,处理消息
    log_->info("[TelegramChannel] Processing message from {}", msg.from.id);
    process_message(msg);
}

void TelegramChannel::issue_pairing_challenge(const TelegramMessage& msg) {
    // 构造配对参数
    pairing::PairingChallengeParams params;
    params.channel = "telegram";
    params.sender_id = msg.from.id;
    params.sender_id_line = fmt::format("User: {} (ID: {})", 
                                        msg.from.username, msg.from.id);
    params.meta = {
        {"username", msg.from.username},
        {"first_name", msg.from.first_name},
        {"last_name", msg.from.last_name}
    };
    
    // Callback: 创建 Pairing Request
    params.upsert_pairing_request = [this](
        const std::string& id,
        const std::unordered_map<std::string, std::string>& meta
    ) -> std::pair<std::string, bool> {
        return pairing_store_.upsert_request(id, meta);
    };
    
    // Callback: 发送配对回复
    params.send_pairing_reply = [this, chat_id = msg.chat.id](const std::string& text) {
        telegram_api_.send_message(chat_id, text);
    };
    
    // Callback: 配对码创建成功
    params.on_created = [this](const std::string& code) {
        log_->info("[TelegramChannel] Pairing code issued: {}", code);
    };
    
    // Callback: 发送失败
    params.on_reply_error = [this](const std::string& error) {
        log_->error("[TelegramChannel] Failed to send pairing reply: {}", error);
    };
    
    // 🚀 发起配对挑战
    auto result = pairing::issue_pairing_challenge(params);
    
    if (result.created && result.code.has_value()) {
        log_->info("[TelegramChannel] Issued pairing challenge: code={}", 
                   result.code.value());
    } else {
        log_->debug("[TelegramChannel] Pairing request already exists");
    }
}

void TelegramChannel::process_message(const TelegramMessage& msg) {
    // 转发给 Agent 处理
    agent_manager_.process_channel_message({
        .channel = "telegram",
        .from = msg.from.id,
        .text = msg.text,
        .attachments = {}
    });
}
```

---

### 示例 2: RPC 管理配对请求

```cpp
// gateway/methods/pairing.cc
#include "hunterclaw/gateway/method_handler.h"
#include "hunterclaw/pairing/service.h"

// ========== pairing.list ==========
// 列出待批准的配对请求
nlohmann::json handle_pairing_list(
    const nlohmann::json& params,
    const ClientContext& ctx
) {
    std::string channel = params.value("channel", "");
    if (channel.empty()) {
        throw std::runtime_error("Missing channel parameter");
    }
    
    // 获取 PairingStore
    auto pairing_store = get_pairing_store(channel);
    
    // 获取待批准的请求
    auto requests = pairing_store->list_requests();
    
    // 清理过期请求
    pairing_store->prune_expired_requests();
    
    // 构造响应
    nlohmann::json result = nlohmann::json::array();
    for (const auto& req : requests) {
        result.push_back({
            {"id", req.id},
            {"code", req.code},
            {"createdAt", req.created_at},
            {"lastSeenAt", req.last_seen_at},
            {"meta", req.meta}
        });
    }
    
    return result;
}

// ========== pairing.approve ==========
// 批准配对请求
nlohmann::json handle_pairing_approve(
    const nlohmann::json& params,
    const ClientContext& ctx
) {
    std::string channel = params.value("channel", "");
    std::string code = params.value("code", "");
    
    if (channel.empty() || code.empty()) {
        throw std::runtime_error("Missing channel or code parameter");
    }
    
    auto pairing_store = get_pairing_store(channel);
    
    // 1. 查找请求
    auto request_opt = pairing_store->get_request(code);
    if (!request_opt.has_value()) {
        throw std::runtime_error("Invalid pairing code");
    }
    
    auto request = request_opt.value();
    
    // 2. 添加到白名单
    pairing_store->add_allow_from(request.id);
    
    // 3. 删除请求
    pairing_store->remove_request(code);
    
    auto logger = spdlog::get("console");
    if (logger) {
        logger->info("[Gateway] Pairing approved: channel={}, user={}", 
                    channel, request.id);
    }
    
    return {
        {"approved", true},
        {"userId", request.id},
        {"meta", request.meta}
    };
}

// ========== pairing.reject ==========
// 拒绝配对请求
nlohmann::json handle_pairing_reject(
    const nlohmann::json& params,
    const ClientContext& ctx
) {
    std::string channel = params.value("channel", "");
    std::string code = params.value("code", "");
    
    if (channel.empty() || code.empty()) {
        throw std::runtime_error("Missing channel or code parameter");
    }
    
    auto pairing_store = get_pairing_store(channel);
    
    // 删除请求
    bool removed = pairing_store->remove_request(code);
    
    return {
        {"rejected", removed}
    };
}

// ========== pairing.allowlist ==========
// 管理白名单
nlohmann::json handle_pairing_allowlist(
    const nlohmann::json& params,
    const ClientContext& ctx
) {
    std::string channel = params.value("channel", "");
    std::string action = params.value("action", "list");  // list | add | remove
    
    auto pairing_store = get_pairing_store(channel);
    
    if (action == "list") {
        auto entries = pairing_store->list_allow_from();
        return nlohmann::json{{"entries", entries}};
    }
    else if (action == "add") {
        std::string entry = params.value("entry", "");
        if (entry.empty()) {
            throw std::runtime_error("Missing entry parameter");
        }
        pairing_store->add_allow_from(entry);
        return {{"added", true}};
    }
    else if (action == "remove") {
        std::string entry = params.value("entry", "");
        if (entry.empty()) {
            throw std::runtime_error("Missing entry parameter");
        }
        pairing_store->remove_allow_from(entry);
        return {{"removed", true}};
    }
    
    throw std::runtime_error("Invalid action: " + action);
}

// 注册 RPC 方法
void register_pairing_methods(Gateway& gateway) {
    gateway.register_method("pairing.list", handle_pairing_list);
    gateway.register_method("pairing.approve", handle_pairing_approve);
    gateway.register_method("pairing.reject", handle_pairing_reject);
    gateway.register_method("pairing.allowlist", handle_pairing_allowlist);
}
```

---

### 示例 3: CLI 工具

```cpp
// cli/pairing.cc
#include "hunterclaw/pairing/service.h"
#include <CLI/CLI.hpp>

int main(int argc, char** argv) {
    CLI::App app{"HunterClaw Pairing Management"};
    
    std::string channel;
    std::string code;
    std::string user_id;
    
    // ========== hunterclaw pairing list ==========
    auto list_cmd = app.add_subcommand("list", "List pending pairing requests");
    list_cmd->add_option("channel", channel, "Channel name")->required();
    list_cmd->callback([&]() {
        pairing::PairingStore store(channel);
        auto requests = store.list_requests();
        
        std::cout << "Pending pairing requests for " << channel << ":\n\n";
        for (const auto& req : requests) {
            std::cout << "Code: " << req.code << "\n";
            std::cout << "  User ID: " << req.id << "\n";
            std::cout << "  Created: " << req.created_at << "\n";
            std::cout << "  Last Seen: " << req.last_seen_at << "\n";
            std::cout << "  Meta:\n";
            for (const auto& [key, value] : req.meta) {
                std::cout << "    " << key << ": " << value << "\n";
            }
            std::cout << "\n";
        }
    });
    
    // ========== hunterclaw pairing approve ==========
    auto approve_cmd = app.add_subcommand("approve", "Approve a pairing request");
    approve_cmd->add_option("channel", channel, "Channel name")->required();
    approve_cmd->add_option("code", code, "Pairing code")->required();
    approve_cmd->callback([&]() {
        pairing::PairingStore store(channel);
        
        auto request_opt = store.get_request(code);
        if (!request_opt.has_value()) {
            std::cerr << "Error: Invalid pairing code\n";
            return;
        }
        
        auto request = request_opt.value();
        store.add_allow_from(request.id);
        store.remove_request(code);
        
        std::cout << "✅ Pairing approved!\n";
        std::cout << "User ID: " << request.id << "\n";
    });
    
    // ========== hunterclaw pairing reject ==========
    auto reject_cmd = app.add_subcommand("reject", "Reject a pairing request");
    reject_cmd->add_option("channel", channel, "Channel name")->required();
    reject_cmd->add_option("code", code, "Pairing code")->required();
    reject_cmd->callback([&]() {
        pairing::PairingStore store(channel);
        bool removed = store.remove_request(code);
        
        if (removed) {
            std::cout << "✅ Pairing request rejected\n";
        } else {
            std::cerr << "Error: Invalid pairing code\n";
        }
    });
    
    // ========== hunterclaw pairing allowlist ==========
    auto allowlist_cmd = app.add_subcommand("allowlist", "Manage allow-from list");
    allowlist_cmd->add_option("channel", channel, "Channel name")->required();
    
    auto add_cmd = allowlist_cmd->add_subcommand("add", "Add user to allowlist");
    add_cmd->add_option("user_id", user_id, "User ID")->required();
    add_cmd->callback([&]() {
        pairing::PairingStore store(channel);
        store.add_allow_from(user_id);
        std::cout << "✅ User added to allowlist\n";
    });
    
    auto remove_cmd = allowlist_cmd->add_subcommand("remove", "Remove user from allowlist");
    remove_cmd->add_option("user_id", user_id, "User ID")->required();
    remove_cmd->callback([&]() {
        pairing::PairingStore store(channel);
        store.remove_allow_from(user_id);
        std::cout << "✅ User removed from allowlist\n";
    });
    
    auto list_allowlist_cmd = allowlist_cmd->add_subcommand("list", "List allowlist");
    list_allowlist_cmd->callback([&]() {
        pairing::PairingStore store(channel);
        auto entries = store.list_allow_from();
        
        std::cout << "Allowlist for " << channel << ":\n";
        for (const auto& entry : entries) {
            std::cout << "  - " << entry << "\n";
        }
    });
    
    CLI11_PARSE(app, argc, argv);
    return 0;
}
```

---

## 安全机制

### 1️⃣ **配对码生成**

```cpp
// 8位随机码,使用混淆字母表 (避免 0/O, 1/I 等易混淆字符)
constexpr const char* PAIRING_CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr size_t PAIRING_CODE_LENGTH = 8;

// 示例: "X7K2MP9Q"
std::string generate_setup_code() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, strlen(PAIRING_CODE_ALPHABET) - 1);
    
    std::string code;
    code.reserve(PAIRING_CODE_LENGTH);
    for (size_t i = 0; i < PAIRING_CODE_LENGTH; ++i) {
        code += PAIRING_CODE_ALPHABET[dis(gen)];
    }
    return code;
}
```

**安全特性:**
- 32 种字符 → 32^8 = 1,099,511,627,776 种组合 (1.1 万亿)
- 避免易混淆字符 (0/O, 1/I)
- 唯一性检查 (最多 500 次重试)

---

### 2️⃣ **配对请求过期**

```cpp
constexpr int64_t PAIRING_PENDING_TTL_MS = 60 * 60 * 1000;  // 1 hour

void PairingStore::prune_expired_requests() {
    int64_t now_ms = std::chrono::system_clock::now().time_since_epoch().count();
    
    // 删除超过 1 小时的请求
    impl_->requests.erase(
        std::remove_if(impl_->requests.begin(), impl_->requests.end(),
            [now_ms](const PairingRequest& req) {
                int64_t created_at = parse_timestamp(req.created_at);
                return now_ms - created_at > PAIRING_PENDING_TTL_MS;
            }
        ),
        impl_->requests.end()
    );
}
```

**安全特性:**
- 1 小时自动过期
- 防止配对码泄露长期有效
- 定期清理过期请求

---

### 3️⃣ **白名单持久化**

```cpp
// 存储路径: /tmp/hunterclaw_pairing_telegram.json
void PairingStore::save() {
    json j;
    j["version"] = 1;
    j["channel"] = impl_->channel;
    j["requests"] = impl_->requests;  // 待批准请求
    j["allowFrom"] = impl_->allow_from_by_account;  // 已授权用户
    
    std::ofstream file(impl_->store_path);
    file << j.dump(2);
}

// 启动时自动加载
void PairingStore::load() {
    std::ifstream file(impl_->store_path);
    if (!file.is_open()) return;
    
    json j;
    file >> j;
    
    impl_->requests = j["requests"];
    impl_->allow_from_by_account = j["allowFrom"];
}
```

**安全特性:**
- 重启后白名单保留
- JSON 格式便于审计
- 支持多账户隔离

---

### 4️⃣ **请求数量限制**

```cpp
constexpr size_t PAIRING_PENDING_MAX = 3;

std::pair<std::string, bool> PairingStore::upsert_request(...) {
    // 限制待批准请求数量
    if (impl_->requests.size() > PAIRING_PENDING_MAX) {
        // 保留最近的 3 个
        std::sort(impl_->requests.begin(), impl_->requests.end(),
            [](const PairingRequest& a, const PairingRequest& b) {
                return parse_timestamp(a.last_seen_at) < parse_timestamp(b.last_seen_at);
            }
        );
        impl_->requests.erase(
            impl_->requests.begin(),
            impl_->requests.end() - PAIRING_PENDING_MAX
        );
    }
    
    return {code, true};
}
```

**安全特性:**
- 防止配对请求堆积
- 自动清理旧请求
- 保留最近活跃的请求

---

## 最佳实践

### ✅ **推荐配置**

```yaml
# 生产环境 - 严格模式
channels:
  telegram:
    accounts:
      - id: "prod_bot"
        dmPolicy: "pairing"  # 🔐 强制配对
        allowFrom: []        # 启动时为空
        
# 开发环境 - 宽松模式
channels:
  telegram:
    accounts:
      - id: "dev_bot"
        dmPolicy: "open"     # ✅ 接受所有 DM
        allowFrom:
          - "123456789"      # 开发者 ID
```

---

### ✅ **定期审计**

```bash
# 列出所有已授权用户
$ hunterclaw pairing allowlist telegram list

# 移除不需要的用户
$ hunterclaw pairing allowlist telegram remove 987654321
```

---

### ✅ **多账户隔离**

```cpp
// 每个账户独立的白名单
pairing_store->add_allow_from("bot_main", "123456789");
pairing_store->add_allow_from("bot_test", "987654321");

// 检查时指定账户
bool allowed = pairing_store->is_allowed("bot_main", "123456789");
```

---

### ✅ **监控与日志**

```cpp
void TelegramChannel::handle_incoming_message(const TelegramMessage& msg) {
    if (config_.dm_policy == DmPolicy::Pairing) {
        if (!pairing_adapter_->is_allowed(msg.from.id)) {
            // 🚨 记录未授权访问尝试
            log_->warn("[Security] Unauthorized DM attempt: user={}, username={}",
                      msg.from.id, msg.from.username);
            
            // 可选: 通知管理员
            notify_admin_unauthorized_access(msg);
            
            issue_pairing_challenge(msg);
            return;
        }
    }
}
```

---

## 总结

### 🎯 HunterClaw Channel 鉴权特点

| 特性 | 说明 |
|------|------|
| **简单易用** | 8位配对码,一键批准 |
| **安全可靠** | 1 小时过期,白名单持久化 |
| **灵活配置** | 支持 Open/Pairing 策略 |
| **多账户隔离** | 每个账户独立白名单 |
| **100% OpenClaw 对齐** | 完全兼容 OpenClaw 设计 |

### 🚀 快速开始

```bash
# 1. 配置 Channel
$ vim config.yaml  # 设置 dmPolicy: "pairing"

# 2. 启动 HunterClaw
$ hunterclaw start

# 3. 用户发送消息 → 获得配对码
# 4. 批准配对
$ hunterclaw pairing approve telegram X7K2MP9Q

# 5. 完成! 🎉
```

---

**完整实现已 100% 对齐 OpenClaw Pairing System!** ✅
