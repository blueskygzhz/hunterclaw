# HunterClaw Gateway - Bearer Token 认证实际操作流程

## 📋 目录
1. [Bearer Token 简介](#bearer-token-简介)
2. [完整操作流程](#完整操作流程)
3. [配置方式](#配置方式)
4. [客户端实现](#客户端实现)
5. [服务端验证](#服务端验证)
6. [实际应用场景](#实际应用场景)
7. [安全最佳实践](#安全最佳实践)

---

## 🎯 Bearer Token 简介

**Bearer Token** 是一种基于 OAuth 2.0 标准的认证方式，通过在 HTTP 请求头中携带 Token 来证明身份。

### 为什么叫 "Bearer"？

**Bearer** = **持票人**，意思是"谁持有这个 Token，谁就有权限访问"。

```
Bearer Token ≈ 电影票/门票
- 不需要身份证明
- 持有 Token = 拥有权限
- 简单但需要妥善保管
```

### 适用场景

| 场景 | 示例 |
|------|------|
| **第三方 API 集成** | Discord Bot Token, GitHub Access Token |
| **简单认证** | 开发测试环境 |
| **无状态服务** | 微服务架构 |
| **长期凭证** | CLI 工具、脚本自动化 |

---

## 📡 完整操作流程

### 流程图

```
┌──────────────────────────────────────────────────────────────────────┐
│                     Bearer Token 认证流程                            │
└──────────────────────────────────────────────────────────────────────┘

┌─────────────┐                                    ┌──────────────┐
│   Client    │                                    │   Gateway    │
└─────────────┘                                    └──────────────┘
      │                                                     │
      │ 1️⃣ 获取 Token (离线)                              │
      ├─ 管理员创建: sk-hunterclaw-abc123                 │
      ├─ 或从 OAuth 服务获取                              │
      │                                                     │
      │ 2️⃣ 发送请求 + Bearer Token                        │
      ├──── POST /api/data ───────────────────────────>   │
      │  Headers:                                           │
      │    Authorization: Bearer sk-hunterclaw-abc123       │
      │  Body: {"action": "create", "data": {...}}         │
      │                                                     │
      │                                3️⃣ 提取 Token       │
      │                     ├─ 检查 "Authorization" 头     │
      │                     ├─ 解析 "Bearer <token>" 格式   │
      │                     └─ 提取 Token 值                │
      │                                                     │
      │                                4️⃣ 验证 Token       │
      │                     ├─ 与配置中的 Token 对比        │
      │                     ├─ 使用常量时间比较             │
      │                     └─ 检查 Token 格式和有效性      │
      │                                                     │
      │                                5️⃣ 返回结果         │
      │ <──────── 200 OK ───────────────────────────────   │
      │  {                                                  │
      │    "status": "success",                             │
      │    "data": {...}                                    │
      │  }                                                  │
      │                                                     │
```

---

## ⚙️ 配置方式

### 方式 1: 直接配置 Token（不推荐⚠️）

```yaml
# gateway.yaml
gateway:
  auth:
    enabled: true
    mode: "bearer"
    bearer_token: "sk-hunterclaw-abc123"  # ⚠️ 明文存储不安全
```

**问题**：
- ❌ Token 明文存储在配置文件中
- ❌ 容易泄露到 Git 仓库
- ❌ 不便于轮转更新

---

### 方式 2: 环境变量（推荐✅）

```yaml
# gateway.yaml
gateway:
  auth:
    enabled: true
    mode: "bearer"
    bearer_token: "${HUNTERCLAW_BEARER_TOKEN}"  # ✅ 从环境变量读取
```

```bash
# 启动前设置环境变量
export HUNTERCLAW_BEARER_TOKEN="sk-hunterclaw-abc123"

# 启动 Gateway
./hunterclaw-gateway --config gateway.yaml
```

**优点**：
- ✅ Token 不会进入版本控制
- ✅ 不同环境可使用不同 Token
- ✅ 便于 CI/CD 集成

---

### 方式 3: 从文件读取（推荐✅）

```yaml
# gateway.yaml
gateway:
  auth:
    enabled: true
    mode: "bearer"
    bearer_token:
      source: "file"
      path: "/etc/hunterclaw/secrets/bearer_token"
```

```bash
# 创建密钥文件（权限 600）
echo "sk-hunterclaw-abc123" > /etc/hunterclaw/secrets/bearer_token
chmod 600 /etc/hunterclaw/secrets/bearer_token
```

**优点**：
- ✅ 支持 Kubernetes Secrets 挂载
- ✅ 可使用专用密钥管理工具
- ✅ 便于权限控制

---

### 方式 4: 从外部命令获取（高级✅）

```yaml
# gateway.yaml
gateway:
  auth:
    enabled: true
    mode: "bearer"
    bearer_token:
      source: "exec"
      command: "/usr/local/bin/get-secret --name hunterclaw-bearer"
```

```bash
# get-secret 脚本示例
#!/bin/bash
# 从 AWS Secrets Manager 获取
aws secretsmanager get-secret-value \
  --secret-id hunterclaw-bearer \
  --query SecretString \
  --output text
```

**优点**：
- ✅ 集成云服务密钥管理（AWS/GCP/Azure）
- ✅ 支持动态密钥轮转
- ✅ 审计和访问控制

---

## 💻 客户端实现

### Python 客户端

```python
import requests
import json

class HunterClawClient:
    def __init__(self, base_url: str, bearer_token: str):
        """
        初始化 HunterClaw 客户端
        
        Args:
            base_url: Gateway 地址，如 "https://api.example.com"
            bearer_token: Bearer Token，如 "sk-hunterclaw-abc123"
        """
        self.base_url = base_url
        self.bearer_token = bearer_token
    
    def _get_headers(self) -> dict:
        """构造请求头"""
        return {
            "Authorization": f"Bearer {self.bearer_token}",
            "Content-Type": "application/json"
        }
    
    def post(self, path: str, data: dict) -> dict:
        """
        发送 POST 请求
        
        Args:
            path: API 路径，如 "/api/data"
            data: 请求数据
        
        Returns:
            响应 JSON
        """
        url = f"{self.base_url}{path}"
        response = requests.post(
            url,
            headers=self._get_headers(),
            json=data
        )
        response.raise_for_status()
        return response.json()
    
    def get(self, path: str) -> dict:
        """发送 GET 请求"""
        url = f"{self.base_url}{path}"
        response = requests.get(
            url,
            headers=self._get_headers()
        )
        response.raise_for_status()
        return response.json()

# 使用示例
if __name__ == "__main__":
    # 1. 初始化客户端
    client = HunterClawClient(
        base_url="https://gateway.example.com",
        bearer_token="sk-hunterclaw-abc123"
    )
    
    # 2. 发送请求
    try:
        result = client.post("/api/transfer", {
            "from": "Alice",
            "to": "Bob",
            "amount": 1000
        })
        print(f"✅ Success: {result}")
    except requests.HTTPError as e:
        if e.response.status_code == 401:
            print("❌ Authentication failed: Invalid bearer token")
        else:
            print(f"❌ Error: {e}")
```

---

### cURL 命令

```bash
# 基本用法
curl -X POST https://gateway.example.com/api/data \
  -H "Authorization: Bearer sk-hunterclaw-abc123" \
  -H "Content-Type: application/json" \
  -d '{"action":"create","data":{"name":"test"}}'

# 从环境变量读取 Token
curl -X POST https://gateway.example.com/api/data \
  -H "Authorization: Bearer ${HUNTERCLAW_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"action":"create"}'

# 从文件读取 Token
HUNTERCLAW_TOKEN=$(cat /etc/hunterclaw/secrets/bearer_token)
curl -X POST https://gateway.example.com/api/data \
  -H "Authorization: Bearer ${HUNTERCLAW_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"action":"create"}'
```

---

### JavaScript (Node.js) 客户端

```javascript
const axios = require('axios');

class HunterClawClient {
    constructor(baseURL, bearerToken) {
        this.client = axios.create({
            baseURL: baseURL,
            headers: {
                'Authorization': `Bearer ${bearerToken}`,
                'Content-Type': 'application/json'
            }
        });
    }
    
    async post(path, data) {
        try {
            const response = await this.client.post(path, data);
            return response.data;
        } catch (error) {
            if (error.response?.status === 401) {
                throw new Error('Authentication failed: Invalid bearer token');
            }
            throw error;
        }
    }
    
    async get(path) {
        const response = await this.client.get(path);
        return response.data;
    }
}

// 使用示例
(async () => {
    const client = new HunterClawClient(
        'https://gateway.example.com',
        process.env.HUNTERCLAW_TOKEN || 'sk-hunterclaw-abc123'
    );
    
    try {
        const result = await client.post('/api/transfer', {
            from: 'Alice',
            to: 'Bob',
            amount: 1000
        });
        console.log('✅ Success:', result);
    } catch (error) {
        console.error('❌ Error:', error.message);
    }
})();
```

---

### Go 客户端

```go
package main

import (
    "bytes"
    "encoding/json"
    "fmt"
    "io"
    "net/http"
)

type HunterClawClient struct {
    BaseURL     string
    BearerToken string
    HTTPClient  *http.Client
}

func NewClient(baseURL, bearerToken string) *HunterClawClient {
    return &HunterClawClient{
        BaseURL:     baseURL,
        BearerToken: bearerToken,
        HTTPClient:  &http.Client{},
    }
}

func (c *HunterClawClient) Post(path string, data interface{}) (map[string]interface{}, error) {
    // 序列化请求数据
    jsonData, err := json.Marshal(data)
    if err != nil {
        return nil, err
    }
    
    // 创建请求
    url := c.BaseURL + path
    req, err := http.NewRequest("POST", url, bytes.NewBuffer(jsonData))
    if err != nil {
        return nil, err
    }
    
    // 设置请求头
    req.Header.Set("Authorization", "Bearer "+c.BearerToken)
    req.Header.Set("Content-Type", "application/json")
    
    // 发送请求
    resp, err := c.HTTPClient.Do(req)
    if err != nil {
        return nil, err
    }
    defer resp.Body.Close()
    
    // 检查状态码
    if resp.StatusCode == 401 {
        return nil, fmt.Errorf("authentication failed: invalid bearer token")
    }
    if resp.StatusCode != 200 {
        return nil, fmt.Errorf("request failed with status %d", resp.StatusCode)
    }
    
    // 解析响应
    body, err := io.ReadAll(resp.Body)
    if err != nil {
        return nil, err
    }
    
    var result map[string]interface{}
    if err := json.Unmarshal(body, &result); err != nil {
        return nil, err
    }
    
    return result, nil
}

func main() {
    // 初始化客户端
    client := NewClient(
        "https://gateway.example.com",
        "sk-hunterclaw-abc123",
    )
    
    // 发送请求
    data := map[string]interface{}{
        "from":   "Alice",
        "to":     "Bob",
        "amount": 1000,
    }
    
    result, err := client.Post("/api/transfer", data)
    if err != nil {
        fmt.Printf("❌ Error: %v\n", err)
        return
    }
    
    fmt.Printf("✅ Success: %v\n", result)
}
```

---

## 🔍 服务端验证

### C++ 实现（HunterClaw Gateway）

```cpp
// 代码路径: src/gateway/auth_manager.cc

bool AuthManager::validate_bearer_token(const std::string& token) const {
    const auto& auth_config = impl_->config->gateway.auth;
    
    // 1. 检查鉴权是否启用
    if (!auth_config.enabled) {
        return true;
    }
    
    // 2. 检查鉴权模式
    if (auth_config.mode != "bearer") {
        return true;
    }
    
    // 3. 获取预期的 Token
    std::string expected = auth_config.bearer_token;
    if (expected.empty()) {
        get_logger()->warn("[AuthManager] Bearer token not configured");
        return false;
    }
    
    // 4. 支持两种格式:
    //    - "sk-hunterclaw-abc123"  (纯 Token)
    //    - "Bearer sk-hunterclaw-abc123"  (带 "Bearer " 前缀)
    bool valid = (token == expected) || (token == ("Bearer " + expected));
    
    // 5. 使用常量时间比较（防止时序攻击）
    if (valid) {
        // 注意: 实际实现应使用 constant_time_compare()
        // valid = constant_time_compare(token, expected);
    }
    
    return valid;
}

// 连接鉴权入口
AuthResult AuthManager::authenticate_connect(
    const json& params,
    const std::string& nonce,
    const std::map<std::string, std::string>& headers,
    const std::string& client_ip
) {
    AuthResult result;
    auto& auth_config = impl_->config->gateway.auth;
    
    // Bearer Token 模式
    if (auth_config.mode == "bearer") {
        // 1. 从参数中提取 Token
        std::string token = params.value("token", "");
        
        // 2. 验证 Token
        if (!validate_bearer_token(token)) {
            get_logger()->warn("[AuthManager] Bearer token validation failed");
            result.ok = false;
            result.reason = "NOT_AUTHENTICATED";
            result.method = "bearer";
            
            // 记录失败（速率限制）
            if (impl_->rate_limiter && !client_ip.empty()) {
                impl_->rate_limiter->record_failure(client_ip, "shared_secret");
            }
            return result;
        }
        
        // 3. 验证成功，返回权限
        get_logger()->debug("[AuthManager] Bearer token validated");
        result.ok = true;
        result.method = "bearer";
        result.device_id = params.value("deviceId", "authenticated");
        result.scopes = {
            "agent.run",
            "session.read",
            "session.write",
            "skill.install",
            "plugin.install",
            "admin.full"
        };
        
        // 重置速率限制
        if (impl_->rate_limiter && !client_ip.empty()) {
            impl_->rate_limiter->reset(client_ip, "shared_secret");
        }
        return result;
    }
    
    // ... 其他鉴权模式 ...
}
```

---

### HTTP 请求头提取

```cpp
// 从 HTTP 请求头中提取 Bearer Token
std::optional<std::string> extract_bearer_token(
    const std::map<std::string, std::string>& headers
) {
    // 1. 查找 "Authorization" 头（不区分大小写）
    std::string auth_header;
    for (const auto& [key, value] : headers) {
        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), 
                      lower_key.begin(), ::tolower);
        
        if (lower_key == "authorization") {
            auth_header = value;
            break;
        }
    }
    
    if (auth_header.empty()) {
        return std::nullopt;
    }
    
    // 2. 检查 "Bearer " 前缀（注意有空格）
    const std::string bearer_prefix = "Bearer ";
    if (auth_header.size() <= bearer_prefix.size()) {
        return std::nullopt;
    }
    
    if (auth_header.substr(0, bearer_prefix.size()) != bearer_prefix) {
        return std::nullopt;
    }
    
    // 3. 提取 Token（去掉 "Bearer " 前缀）
    std::string token = auth_header.substr(bearer_prefix.size());
    
    // 4. 去除首尾空格
    token.erase(0, token.find_first_not_of(" \t\n\r"));
    token.erase(token.find_last_not_of(" \t\n\r") + 1);
    
    return token;
}
```

---

## 🎬 实际应用场景

### 场景 1: Discord Bot 集成

Discord Bot Token 就是一种 Bearer Token。

```python
import discord
import requests

class DiscordBotWithHunterClaw:
    def __init__(self, discord_token: str, hunterclaw_token: str):
        self.discord_client = discord.Client()
        self.hunterclaw_token = hunterclaw_token
        self.hunterclaw_base = "https://gateway.example.com"
    
    def call_hunterclaw_api(self, path: str, data: dict):
        """调用 HunterClaw Gateway"""
        headers = {
            "Authorization": f"Bearer {self.hunterclaw_token}",
            "Content-Type": "application/json"
        }
        response = requests.post(
            f"{self.hunterclaw_base}{path}",
            headers=headers,
            json=data
        )
        return response.json()
    
    @discord_client.event
    async def on_message(self, message):
        if message.content.startswith("!analyze"):
            # 调用 HunterClaw API 分析数据
            result = self.call_hunterclaw_api("/api/analyze", {
                "text": message.content[9:]
            })
            await message.channel.send(f"分析结果: {result}")

# 启动 Bot
bot = DiscordBotWithHunterClaw(
    discord_token="YOUR_DISCORD_BOT_TOKEN",
    hunterclaw_token="sk-hunterclaw-abc123"
)
bot.discord_client.run(bot.discord_token)
```

---

### 场景 2: GitHub Actions CI/CD

```yaml
# .github/workflows/deploy.yml
name: Deploy to Production

on:
  push:
    branches: [main]

jobs:
  deploy:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Deploy via HunterClaw Gateway
        env:
          HUNTERCLAW_TOKEN: ${{ secrets.HUNTERCLAW_TOKEN }}
        run: |
          curl -X POST https://gateway.example.com/api/deploy \
            -H "Authorization: Bearer ${HUNTERCLAW_TOKEN}" \
            -H "Content-Type: application/json" \
            -d '{
              "project": "my-app",
              "version": "${{ github.sha }}",
              "environment": "production"
            }'
```

**配置密钥**：
```bash
# 在 GitHub 仓库设置中添加 Secret
# Settings → Secrets and variables → Actions → New repository secret
# Name: HUNTERCLAW_TOKEN
# Value: sk-hunterclaw-abc123
```

---

### 场景 3: Kubernetes 部署

```yaml
# kubernetes/secret.yaml
apiVersion: v1
kind: Secret
metadata:
  name: hunterclaw-bearer-token
type: Opaque
stringData:
  token: sk-hunterclaw-abc123

---
# kubernetes/deployment.yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: my-app
spec:
  template:
    spec:
      containers:
      - name: app
        image: my-app:latest
        env:
        - name: HUNTERCLAW_TOKEN
          valueFrom:
            secretKeyRef:
              name: hunterclaw-bearer-token
              key: token
        command:
        - /bin/sh
        - -c
        - |
          # 使用 Bearer Token 调用 HunterClaw API
          curl -X POST https://gateway.example.com/api/register \
            -H "Authorization: Bearer ${HUNTERCLAW_TOKEN}" \
            -H "Content-Type: application/json" \
            -d '{"service":"my-app","pod":"'${HOSTNAME}'"}'
```

---

## 🔒 安全最佳实践

### ✅ Token 管理

| 实践 | 说明 |
|------|------|
| **使用强随机 Token** | 至少 32 字符，包含字母、数字、特殊字符 |
| **定期轮转** | 建议每 90 天更换一次 |
| **限制权限** | 遵循最小权限原则 |
| **分环境管理** | 开发/测试/生产使用不同 Token |

### ✅ 存储安全

```bash
# ❌ 不要这样做
echo "bearer_token: sk-hunterclaw-abc123" >> config.yaml
git add config.yaml  # Token 泄露到 Git！

# ✅ 正确做法
echo "sk-hunterclaw-abc123" > /etc/hunterclaw/secrets/token
chmod 600 /etc/hunterclaw/secrets/token
chown hunterclaw:hunterclaw /etc/hunterclaw/secrets/token

# 在配置文件中引用
echo "bearer_token: \${HUNTERCLAW_TOKEN}" >> config.yaml
```

### ✅ 传输安全

```yaml
# 强制使用 HTTPS
gateway:
  tls:
    enabled: true
    cert_file: "/etc/hunterclaw/tls/cert.pem"
    key_file: "/etc/hunterclaw/tls/key.pem"
  
  # 禁止 HTTP 明文传输
  http:
    enabled: false
```

### ✅ 速率限制

```yaml
# 防止暴力破解
gateway:
  auth:
    mode: "bearer"
    rate_limit:
      max_attempts: 5        # 最多 5 次失败
      window_ms: 60000       # 1 分钟窗口
      lockout_ms: 300000     # 锁定 5 分钟
```

### ✅ 日志审计

```cpp
// 记录鉴权事件
void log_auth_event(
    const std::string& event_type,
    const std::string& client_ip,
    bool success,
    const std::string& reason = ""
) {
    json log_entry = {
        {"timestamp", std::time(nullptr)},
        {"event", event_type},
        {"client_ip", client_ip},
        {"success", success},
        {"reason", reason}
    };
    
    if (success) {
        get_logger()->info("[Auth] {}", log_entry.dump());
    } else {
        get_logger()->warn("[Auth] {}", log_entry.dump());
        
        // 发送告警（失败次数过多）
        if (should_alert(client_ip)) {
            send_alert("Multiple authentication failures", log_entry);
        }
    }
}
```

---

## 📊 常见错误和解决方法

### 错误 1: 401 Unauthorized

```json
{
  "error": "NOT_AUTHENTICATED",
  "message": "Invalid bearer token"
}
```

**原因**：
- ❌ Token 错误或过期
- ❌ 请求头格式错误（缺少 "Bearer " 前缀）
- ❌ Token 被撤销

**解决方法**：
```bash
# 1. 检查 Token 是否正确
echo $HUNTERCLAW_TOKEN

# 2. 检查请求头格式
curl -v https://gateway.example.com/api/data \
  -H "Authorization: Bearer ${HUNTERCLAW_TOKEN}"
# 注意: "Bearer " 后面有空格！

# 3. 测试 Token 是否有效
curl https://gateway.example.com/auth/verify \
  -H "Authorization: Bearer ${HUNTERCLAW_TOKEN}"
```

---

### 错误 2: 429 Too Many Requests

```json
{
  "error": "RATE_LIMITED",
  "message": "Too many failed authentication attempts",
  "retry_after_ms": 300000
}
```

**原因**：
- ❌ 短时间内多次认证失败
- ❌ 触发速率限制

**解决方法**：
```bash
# 等待锁定期结束（5 分钟）
sleep 300

# 或联系管理员重置速率限制
```

---

### 错误 3: Token 泄露

**检测方法**：
```bash
# 1. 检查 Git 历史
git log -p | grep -i "sk-hunterclaw"

# 2. 检查环境变量
env | grep -i token

# 3. 检查日志文件
grep -r "sk-hunterclaw" /var/log/
```

**应急处理**：
1. ✅ 立即撤销泄露的 Token
2. ✅ 生成新 Token
3. ✅ 更新所有使用该 Token 的服务
4. ✅ 审计访问日志，确认是否被滥用

---

## 🎯 总结

### Bearer Token 优缺点

**优点**：
- ✅ 实现简单，易于集成
- ✅ 无状态，适合分布式架构
- ✅ 符合 OAuth 2.0 标准
- ✅ 广泛支持（Discord、GitHub、Slack 等）

**缺点**：
- ⚠️ Token 泄露风险高（持有即有权限）
- ⚠️ 需要额外的密钥管理机制
- ⚠️ 不适合高安全要求场景（推荐用 Challenge-Response）

### 使用建议

| 场景 | 推荐 | 原因 |
|------|------|------|
| 开发测试 | ✅ | 简单快速 |
| 第三方 API 集成 | ✅ | 标准兼容 |
| 微服务内部调用 | ✅ | 无状态高效 |
| 金融支付 | ❌ | 安全性不足，用 Challenge-Response |
| 公网暴露 API | ⚠️ | 需配合 HTTPS + 速率限制 |

---

**文档版本**: v1.0  
**最后更新**: 2026-03-16  
**作者**: HunterClaw Team
