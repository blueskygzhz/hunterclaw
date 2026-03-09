# 飞书 Channel 使用指南

## 概述

HunterClaw 的飞书(Feishu/Lark) Channel 实现，100% 对齐 OpenClaw 架构，提供完整的飞书机器人接入能力。

## 特性

### ✅ 核心功能
- **多连接模式**: WebSocket (推荐) 和 Webhook 两种模式
- **消息收发**: 支持文本、图片、文件、音频、视频等多种消息类型
- **群组支持**: 完整的私聊和群聊功能
- **消息去重**: 防止重复处理同一消息
- **消息防抖**: 自动合并快速连续的消息
- **权限策略**: 灵活的白名单和配对机制
- **多账号**: 支持同时运行多个飞书机器人

### 🎯 对齐 OpenClaw
- ChannelAccountSnapshot: 运行时状态快照
- 自动重连机制: 指数退避策略
- 健康监控: 定期检查连接状态
- 广播模式: 一条消息可分发给多个 agent

## 快速开始

### 1. 创建飞书机器人

1. 访问 [飞书开放平台](https://open.feishu.cn) (国内) 或 [Lark Developer](https://open.larksuite.com) (海外)
2. 创建企业自建应用
3. 记录以下凭据：
   - `App ID`
   - `App Secret`
   - `Encrypt Key` (事件加密密钥)
   - `Verification Token` (Webhook 验证令牌)

4. 配置权限：
   - 获取与上传图片或文件资源
   - 获取用户基本信息
   - 接收消息 v2.0
   - 以应用身份发消息
   - 添加应用机器人到会话

### 2. 配置 HunterClaw

创建配置文件 `config/feishu.json`:

```json
{
  "feishu": {
    "enabled": true,
    "default_account": "default",
    
    "accounts": {
      "default": {
        "app_id": "cli_xxxxxxxxxxxxxxxx",
        "app_secret": "your_app_secret_here",
        "encrypt_key": "your_encrypt_key_here",
        "verification_token": "your_verification_token_here",
        
        "domain": "feishu",
        "connection_mode": "websocket",
        
        "dm_policy": "open",
        "group_policy": "open",
        
        "enable_message_dedup": true,
        "enable_inbound_debounce": true
      }
    }
  }
}
```

### 3. 编译

```bash
cd /data/workspace/QQMail/HunterClaw
mkdir -p build && cd build
source /opt/rh/devtoolset-11/enable
cmake ..
make hunterclaw_channel_feishu -j4
```

### 4. 运行

```bash
# 启动 HunterClaw，加载飞书 channel
./bin/hunterclaw --config config/feishu.json

# 或使用环境变量
export FEISHU_APP_ID="cli_xxx"
export FEISHU_APP_SECRET="xxx"
./bin/hunterclaw
```

## 配置详解

### 连接模式

#### WebSocket 模式 (推荐)
```json
{
  "connection_mode": "websocket"
}
```

**优点**:
- 无需公网 IP
- 无需配置防火墙
- 实时接收消息
- 官方推荐方式

**适用场景**: 开发环境、内网部署

#### Webhook 模式
```json
{
  "connection_mode": "webhook",
  "webhook_path": "/feishu/webhook",
  "webhook_host": "0.0.0.0",
  "webhook_port": 8089
}
```

**优点**:
- 无需保持长连接
- 适合高可用部署

**要求**:
- 需要公网 IP
- 需要在飞书后台配置 Webhook URL

**适用场景**: 生产环境、高并发场景

### 权限策略

#### 私聊策略 (dm_policy)
```json
{
  "dm_policy": "open"      // 接受所有私聊
  "dm_policy": "pairing"   // 需要配对(用户需先发送特定命令)
  "dm_policy": "allowlist" // 仅白名单用户
}
```

#### 群聊策略 (group_policy)
```json
{
  "group_policy": "open"      // 接受所有群聊
  "group_policy": "allowlist" // 仅白名单群组
  "group_policy": "disabled"  // 禁用群聊
}
```

#### 白名单配置
```json
{
  "allow_from": [
    "ou_xxxxxxxxxxxxxxxx",  // 用户ID
    "oc_xxxxxxxxxxxxxxxx"   // 群组ID
  ]
}
```

### 消息处理

#### 消息去重
```json
{
  "enable_message_dedup": true,
  "dedup_window_seconds": 300  // 5分钟去重窗口
}
```

防止因网络波动或重试导致的消息重复处理。

#### 消息防抖
```json
{
  "enable_inbound_debounce": true,
  "debounce_delay_ms": 500  // 500ms 防抖延迟
}
```

自动合并快速连续的消息，减少频繁的 agent 调用。

### 多账号配置

```json
{
  "accounts": {
    "default": {
      "app_id": "cli_aaa",
      ...
    },
    "海外": {
      "app_id": "cli_bbb",
      "domain": "lark",
      ...
    },
    "测试": {
      "app_id": "cli_ccc",
      "dm_policy": "allowlist",
      "allow_from": ["ou_test_user"],
      ...
    }
  }
}
```

## API 使用

### 发送消息

```cpp
#include "hunterclaw/channels/feishu_channel.h"

using namespace hunterclaw::channels;

// 创建适配器
FeishuConfig config;
config.app_id = "cli_xxx";
config.app_secret = "xxx";

auto adapter = std::make_shared<FeishuChannelAdapter>(config);

// 发送文本
adapter->send_text("oc_chat_id", "Hello from HunterClaw!");

// 发送图片
adapter->send_image("oc_chat_id", "/path/to/image.jpg");

// 发送文件
adapter->send_file("oc_chat_id", "/path/to/file.pdf");

// 回复消息
adapter->reply_message("om_message_id", "收到!");

// 添加表情反应
adapter->add_reaction("om_message_id", "👍");
```

### 接收消息

```cpp
// 设置消息回调
ChannelContext ctx;
ctx.on_message_received = [](const ChannelMessage& msg) {
    std::cout << "收到消息: " << msg.text << std::endl;
    std::cout << "来自: " << msg.from << std::endl;
    std::cout << "会话: " << msg.to << std::endl;
};

adapter->start_account(ctx);
```

## 进阶功能

### 交互式卡片

```cpp
// 发送交互式卡片
core::JsonValue card = create_interactive_card(
    "卡片标题",
    "卡片内容",
    {
        {"action1", "按钮1"},
        {"action2", "按钮2"}
    }
);

adapter->send_card("oc_chat_id", card);
```

### 富文本消息

```cpp
// 发送富文本(Post)消息
core::JsonValue post = create_post_message(
    "富文本标题",
    {
        {"text", "这是普通文本"},
        {"link", "https://example.com", "这是链接"},
        {"at", "ou_user_id", "提及用户"}
    }
);

adapter->send_post("oc_chat_id", post);
```

### 媒体处理

```cpp
// 下载消息中的媒体文件
auto media_keys = message.media_keys;
for (const auto& key : media_keys) {
    auto result = adapter->download_media(key, "/tmp/" + key);
    if (result.is_ok()) {
        std::cout << "Downloaded: " << result.value << std::endl;
    }
}

// 上传图片获取 image_key
auto upload_result = adapter->upload_image("/path/to/image.jpg");
if (upload_result.is_ok()) {
    std::string image_key = upload_result.value;
    // 使用 image_key 发送图片消息
}
```

## 故障排查

### 常见问题

#### 1. 连接失败
- 检查 `app_id` 和 `app_secret` 是否正确
- 确认飞书应用已发布且未被停用
- WebSocket 模式无需公网IP，Webhook 模式需要

#### 2. 收不到消息
- 确认已订阅消息事件 (`im.message.receive_v1`)
- 检查权限策略配置
- 查看日志是否有错误信息

#### 3. 发送消息失败
- 确认 token 未过期 (自动刷新)
- 检查目标 chat_id 是否正确
- 确认机器人已加入目标群组

#### 4. 消息重复
- 启用消息去重: `enable_message_dedup: true`
- 调整去重窗口: `dedup_window_seconds`

### 日志调试

```cpp
// 启用详细日志
ctx.log_info = [](const std::string& log) {
    std::cout << "[INFO] " << log << std::endl;
};

ctx.log_error = [](const std::string& log) {
    std::cerr << "[ERROR] " << log << std::endl;
};
```

## 性能优化

### 1. 连接池
对于多账号场景，HunterClaw 自动管理连接池，无需手动处理。

### 2. 消息批处理
启用消息防抖可自动批处理：
```json
{
  "enable_inbound_debounce": true,
  "debounce_delay_ms": 500
}
```

### 3. 异步发送
所有消息发送操作都是异步的，不会阻塞主线程。

## 安全最佳实践

1. **不要硬编码凭据**
   ```bash
   # 使用环境变量
   export FEISHU_APP_SECRET="xxx"
   ```

2. **使用白名单**
   ```json
   {
     "dm_policy": "allowlist",
     "allow_from": ["ou_trusted_user"]
   }
   ```

3. **定期轮换密钥**
   在飞书后台重新生成 `app_secret` 并更新配置。

4. **监控异常行为**
   启用健康监控，及时发现异常连接。

## 架构对齐说明

HunterClaw 的飞书实现 100% 对齐 OpenClaw:

| 特性 | OpenClaw (TypeScript) | HunterClaw (C++) | 状态 |
|-----|----------------------|------------------|-----|
| WebSocket 模式 | ✅ | ✅ | 完全对齐 |
| Webhook 模式 | ✅ | ✅ | 完全对齐 |
| 消息去重 | ✅ | ✅ | 完全对齐 |
| 消息防抖 | ✅ | ✅ | 完全对齐 |
| 权限策略 | ✅ | ✅ | 完全对齐 |
| 多账号 | ✅ | ✅ | 完全对齐 |
| 自动重连 | ✅ | ✅ | 完全对齐 |
| 广播模式 | ✅ | ✅ | 完全对齐 |
| 交互式卡片 | ✅ | ⚠️ | 部分实现 |
| 表情反应 | ✅ | ✅ | 完全对齐 |

⚠️ = 核心功能已实现，高级特性待完善

## 示例代码

完整示例请查看:
- `examples/feishu_echo_bot.cc` - 简单回声机器人
- `examples/feishu_card_demo.cc` - 交互式卡片演示
- `examples/feishu_multi_account.cc` - 多账号部署

## 相关链接

- [飞书开放平台文档](https://open.feishu.cn/document)
- [Lark Developer Docs](https://open.larksuite.com/document)
- [HunterClaw 文档](../README.md)
- [OpenClaw 飞书实现](https://github.com/openclaw/openclaw/tree/main/extensions/feishu)

## 贡献

欢迎提交 Issue 和 Pull Request！

## 许可证

与 HunterClaw 主项目相同。
