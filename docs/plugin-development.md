# Plugin Development Guide

This guide explains how to develop plugins for HunterClaw, including **Channel Adapters** and **Skill Extensions**.

## Table of Contents

- [Plugin Architecture](#plugin-architecture)
- [Creating a Channel Plugin](#creating-a-channel-plugin)
- [Creating a Skill Plugin](#creating-a-skill-plugin)
- [Plugin API Reference](#plugin-api-reference)
- [Building and Testing](#building-and-testing)
- [Publishing Plugins](#publishing-plugins)

## Plugin Architecture

HunterClaw uses a **dynamic plugin system** based on shared libraries (`.so` on Linux, `.dylib` on macOS, `.dll` on Windows).

### Plugin Types

1. **Native Plugins** (C/C++) - High performance, full API access
2. **Script Plugins** (Python/Lua) - Easy development, sandboxed
3. **WASM Plugins** - Cross-platform, secure

### Plugin Lifecycle

```
Load → Init → Run → Reload (optional) → Shutdown → Unload
```

## Creating a Channel Plugin

### 1. Plugin Structure

```
my-channel-plugin/
├── CMakeLists.txt
├── hunterclaw.plugin.json
├── src/
│   └── my_channel.cc
└── README.md
```

### 2. Manifest File (`hunterclaw.plugin.json`)

```json
{
  "id": "my-channel",
  "name": "My Channel Adapter",
  "version": "1.0.0",
  "description": "Adapter for MyChannel messaging platform",
  "kind": "native",
  "channels": ["mychannel"],
  "dependencies": [],
  "configSchema": {
    "type": "object",
    "properties": {
      "apiKey": {
        "type": "string",
        "description": "API key for MyChannel"
      }
    },
    "required": ["apiKey"]
  }
}
```

### 3. Implement Channel Adapter

```cpp
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/plugins/plugin.h"

using namespace hunterclaw;

class MyChannelAdapter : public channels::ChannelAdapter {
public:
    channels::ChannelMeta meta() const override {
        return {
            .id = "mychannel",
            .name = "MyChannel",
            .description = "MyChannel messaging platform",
            .capabilities = {
                channels::ChannelCapability::SendText,
                channels::ChannelCapability::ReceiveText,
            }
        };
    }
    
    core::Result<bool> start_account(const channels::ChannelContext& ctx) override {
        // Initialize connection to MyChannel API
        ctx_ = ctx;
        api_key_ = ctx.account.credentials["apiKey"];
        
        // Start message polling/webhook
        is_running_ = true;
        polling_thread_ = std::thread(&MyChannelAdapter::poll_messages, this);
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> stop_account() override {
        is_running_ = false;
        if (polling_thread_.joinable()) {
            polling_thread_.join();
        }
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> send_text(const std::string& to, const std::string& text) override {
        // Call MyChannel API to send message
        // ...
        return core::Result<bool>::Ok(true);
    }
    
    // ... implement other methods
    
private:
    channels::ChannelContext ctx_;
    std::string api_key_;
    std::atomic<bool> is_running_{false};
    std::thread polling_thread_;
    
    void poll_messages() {
        while (is_running_) {
            // Poll for new messages
            // ...
            
            // Deliver to core
            channels::ChannelMessage msg{
                .id = "...",
                .channel_id = "mychannel",
                .from = "...",
                .to = "...",
                .text = "...",
            };
            
            if (ctx_.on_message_received) {
                ctx_.on_message_received(msg);
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
};

class MyChannelPlugin : public channels::ChannelPlugin {
public:
    std::string id() const override { return "mychannel"; }
    std::string name() const override { return "MyChannel Plugin"; }
    std::string version() const override { return "1.0.0"; }
    
    std::shared_ptr<channels::ChannelAdapter> create_adapter() override {
        return std::make_shared<MyChannelAdapter>();
    }
};

// Register plugin
HUNTERCLAW_REGISTER_PLUGIN(MyChannelPlugin)
```

### 4. CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(hunterclaw-channel-mychannel VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(HunterClaw REQUIRED)
find_package(CURL REQUIRED)

add_library(hunterclaw_channel_mychannel SHARED
    src/my_channel.cc
)

target_link_libraries(hunterclaw_channel_mychannel
    HunterClaw::core
    ${CURL_LIBRARIES}
)

install(TARGETS hunterclaw_channel_mychannel
    LIBRARY DESTINATION lib/hunterclaw/plugins/channels
)
```

### 5. Build and Install

```bash
mkdir build && cd build
cmake ..
make
sudo make install
```

### 6. Configure in HunterClaw

```yaml
# config.yaml
plugins:
  mychannel:
    enabled: true
    config:
      apiKey: "your-api-key-here"

channels:
  mychannel:
    default:
      enabled: true
      credentials:
        apiKey: "${MYCHANNEL_API_KEY}"
```

## Creating a Skill Plugin

Skills in HunterClaw are **compatible with OpenClaw SKILL.md format**.

### 1. Skill Structure

```
my-skill-plugin/
├── hunterclaw.plugin.json
├── skills/
│   └── my-skill/
│       ├── SKILL.md
│       └── scripts/
│           └── helper.py
└── README.md
```

### 2. Manifest

```json
{
  "id": "my-skills",
  "name": "My Skill Pack",
  "version": "1.0.0",
  "description": "Collection of useful skills",
  "skills": [
    "skills/my-skill"
  ]
}
```

### 3. SKILL.md (OpenClaw Compatible)

```markdown
---
name: my-skill
description: "Description of what this skill does"
metadata:
  openclaw:
    emoji: "🔧"
    requires:
      bins: ["jq", "curl"]
    install:
      - id: brew
        kind: brew
        formula: jq
        bins: [jq]
        os: [darwin, linux]
---

# My Skill

This skill provides XYZ functionality.

## When to Use
✅ USE when: User needs XYZ
❌ DON'T use when: User needs ABC

## Usage

### Example 1
```bash
jq '.field' data.json
```

### Example 2
```bash
curl -X GET https://api.example.com
```
```

### 4. Install Plugin

```bash
# Copy to plugins directory
cp -r my-skill-plugin ~/.hunterclaw/plugins/

# Or install from registry
hunterclaw plugin install my-skills
```

## Plugin API Reference

### Core Types

```cpp
namespace hunterclaw::core {
    // Result type (Rust-style)
    template<typename T>
    struct Result {
        bool ok;
        T value;
        std::string error;
        
        static Result<T> Ok(T val);
        static Result<T> Err(std::string err);
    };
}
```

### Plugin Context

```cpp
struct PluginContext {
    std::shared_ptr<core::Config> config;
    std::string workspace_dir;
    std::string plugin_dir;
    
    std::function<void(const std::string& log)> log_info;
    std::function<void(const std::string& log)> log_error;
    std::function<void(const std::string& event, const core::JsonValue& data)> emit_event;
};
```

### Channel Context

```cpp
struct ChannelContext {
    std::shared_ptr<core::Config> config;
    ChannelAccount account;
    
    std::function<void(const ChannelMessage& msg)> on_message_received;
    std::shared_ptr<std::atomic<bool>> abort_signal;
};
```

## Building and Testing

### Unit Tests

```cpp
#include <gtest/gtest.h>
#include "my_channel.h"

TEST(MyChannelTest, SendMessage) {
    MyChannelAdapter adapter;
    channels::ChannelContext ctx;
    
    auto result = adapter.start_account(ctx);
    ASSERT_TRUE(result.is_ok());
    
    auto send_result = adapter.send_text("user123", "Hello");
    ASSERT_TRUE(send_result.is_ok());
}
```

### Integration Tests

```bash
# Start test gateway
hunterclaw-gateway --config test-config.yaml &

# Run tests
./test_my_channel_plugin

# Stop gateway
killall hunterclaw-gateway
```

## Publishing Plugins

### 1. Package Plugin

```bash
tar czf hunterclaw-channel-mychannel-1.0.0.tar.gz \
    hunterclaw.plugin.json \
    lib/libhunterclaw_channel_mychannel.so \
    README.md
```

### 2. Publish to Registry

```bash
hunterclaw plugin publish \
    --package hunterclaw-channel-mychannel-1.0.0.tar.gz \
    --api-key YOUR_API_KEY
```

### 3. Publish to GitHub Releases

```bash
gh release create v1.0.0 \
    hunterclaw-channel-mychannel-1.0.0.tar.gz \
    --title "MyChannel Plugin v1.0.0" \
    --notes "Initial release"
```

## Best Practices

1. **Error Handling**: Always use `Result<T>` for fallible operations
2. **Logging**: Use `ctx.log_info()` and `ctx.log_error()` for debugging
3. **Thread Safety**: Protect shared state with mutexes
4. **Graceful Shutdown**: Respect `abort_signal` for cancellation
5. **Memory Management**: Use smart pointers (`shared_ptr`, `unique_ptr`)
6. **Security**: Validate all inputs, scan for dangerous patterns
7. **Testing**: Write comprehensive unit and integration tests

## Examples

See the `plugins/` directory for complete examples:
- `plugins/channels/telegram/` - Telegram adapter
- `plugins/channels/discord/` - Discord adapter
- `plugins/skills/github/` - GitHub skill

## Support

- Documentation: https://hunterclaw.io/docs
- GitHub Issues: https://github.com/hunterclaw/hunterclaw/issues
- Discord: https://discord.gg/hunterclaw
