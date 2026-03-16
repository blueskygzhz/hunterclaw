# HunterClaw Gateway 鉴权快速参考

## 🔐 5 种鉴权模式

### 1. None (无鉴权)
```json
{ "gateway": { "auth": { "mode": "none" } } }
```
适用: 本地开发、内网部署

### 2. Bearer Token
```json
{
  "gateway": {
    "auth": {
      "enabled": true,
      "mode": "bearer",
      "bearer_token": "sk-hunterclaw-xxx"
    }
  }
}
```
适用: 单用户、简单场景

### 3. Device (设备认证)
```json
{
  "gateway": {
    "auth": {
      "enabled": true,
      "mode": "device"
    }
  }
}
```
适用: 多设备、多用户

### 4. Challenge (挑战-响应)
```json
{
  "gateway": {
    "auth": {
      "enabled": true,
      "mode": "challenge"
    }
  }
}
```
适用: 高安全场景、零信任架构

### 5. Trusted Proxy ✨ 新增
```json
{
  "gateway": {
    "trusted_proxies": ["10.0.0.1"],
    "auth": {
      "enabled": true,
      "mode": "trusted-proxy",
      "trusted_proxy": {
        "user_header": "x-forwarded-user",
        "required_headers": ["x-forwarded-proto"],
        "allow_users": ["admin@company.com"]
      }
    }
  }
}
```
适用: 企业部署、SSO 集成

---

## ⚡ 速率限制 ✨ 新增

```json
{
  "gateway": {
    "auth": {
      "rate_limit": {
        "max_attempts": 10,
        "window_ms": 60000,
        "lockout_ms": 300000,
        "exempt_loopback": true
      }
    }
  }
}
```

**默认值**:
- 最多失败 10 次 / 1 分钟
- 锁定 5 分钟
- 本地回环豁免

---

## 🔑 环境变量凭证 ✨ 新增

```bash
# HunterClaw 环境变量 (优先)
export HUNTERCLAW_GATEWAY_TOKEN="sk-hunterclaw-xxx"
export HUNTERCLAW_GATEWAY_PASSWORD="secret-password"

# OpenClaw 兼容
export OPENCLAW_GATEWAY_TOKEN="sk-openclaw-xxx"
export OPENCLAW_GATEWAY_PASSWORD="secret-password"

# 旧版兼容
export CLAWDBOT_GATEWAY_TOKEN="sk-clawdbot-xxx"
```

**优先级**: 环境变量 > 配置文件

---

## 🎯 权限系统 (Scopes)

```cpp
namespace scopes {
    // Agent 权限
    constexpr const char* AGENT_RUN = "agent.run";
    constexpr const char* AGENT_CANCEL = "agent.cancel";
    
    // Session 权限
    constexpr const char* SESSION_READ = "session.read";
    constexpr const char* SESSION_WRITE = "session.write";
    
    // Skill/Plugin 权限
    constexpr const char* SKILL_INSTALL = "skill.install";
    constexpr const char* PLUGIN_INSTALL = "plugin.install";
    
    // 管理员权限 (通配符)
    constexpr const char* ADMIN_FULL = "admin.full";  // 拥有所有权限
}
```

**通配符支持**:
- `admin.full` → 匹配所有权限
- `agent.*` → 匹配所有 agent 开头的权限

---

## 🔧 API 使用

### C++ 代码

```cpp
#include "hunterclaw/gateway/auth_manager.h"
#include "hunterclaw/gateway/auth_rate_limiter.h"
#include "hunterclaw/gateway/credential_helper.h"

// 创建 AuthManager
auto auth_mgr = std::make_shared<AuthManager>(config);

// 鉴权 connect() 调用
json params = {{"token", "sk-hunterclaw-xxx"}};
std::map<std::string, std::string> headers = {
    {"x-forwarded-user", "admin@company.com"}
};
std::string client_ip = "192.168.1.100";

auto result = auth_mgr->authenticate_connect(
    params, 
    "", 
    headers, 
    client_ip
);

if (!result.ok) {
    if (result.rate_limited) {
        // 速率限制: 等待 result.retry_after_ms 毫秒
    }
    // 其他错误: result.reason
}
```

---

## 🌟 核心改进

### 1. 速率限制器
- ✅ 防暴力破解
- ✅ 滑动窗口算法
- ✅ IP 级别限制
- ✅ 自动锁定/解锁

### 2. Trusted Proxy
- ✅ 反向代理委托认证
- ✅ 用户白名单
- ✅ 必需头验证
- ✅ 企业级 SSO 支持

### 3. 环境变量
- ✅ 保护敏感信息
- ✅ 兼容 OpenClaw
- ✅ 优先级机制

---

## 📊 错误码

```cpp
namespace auth_errors {
    // 基础错误
    const char* NOT_AUTHENTICATED = "NOT_AUTHENTICATED";
    const char* DEVICE_TOKEN_MISMATCH = "DEVICE_TOKEN_MISMATCH";
    const char* INVALID_SIGNATURE = "INVALID_SIGNATURE";
    const char* TOKEN_EXPIRED = "TOKEN_EXPIRED";
    const char* DEVICE_REVOKED = "DEVICE_REVOKED";
    
    // Trusted Proxy 错误 ✨
    const char* TRUSTED_PROXY_UNTRUSTED_SOURCE = "TRUSTED_PROXY_UNTRUSTED_SOURCE";
    const char* TRUSTED_PROXY_USER_MISSING = "TRUSTED_PROXY_USER_MISSING";
    const char* TRUSTED_PROXY_USER_NOT_ALLOWED = "TRUSTED_PROXY_USER_NOT_ALLOWED";
    
    // 速率限制错误 ✨
    const char* RATE_LIMITED = "RATE_LIMITED";
}
```

---

## 📚 文档

完整文档: `GATEWAY_AUTH_ALIGNMENT_COMPLETE.md`

**对齐度**: **95%+** (OpenClaw 100% 对齐)
