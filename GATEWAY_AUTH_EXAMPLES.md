# 🔐 HunterClaw Gateway 鉴权认证流程详解

本文档通过实际示例详细说明 HunterClaw Gateway 的 **5 种鉴权模式**，覆盖配置、请求处理、验证逻辑的完整流程。

---

## 📚 目录

1. [鉴权模式概览](#鉴权模式概览)
2. [模式 1: API Key 鉴权](#模式-1-api-key-鉴权)
3. [模式 2: Bearer Token 鉴权](#模式-2-bearer-token-鉴权)
4. [模式 3: Basic Auth 鉴权](#模式-3-basic-auth-鉴权)
5. [模式 4: Challenge-Response 签名鉴权](#模式-4-challenge-response-签名鉴权)
6. [模式 5: Trusted Proxy 鉴权](#模式-5-trusted-proxy-鉴权)
7. [SecretRef 密钥管理](#secretref-密钥管理)
8. [权限控制与速率限制](#权限控制与速率限制)
9. [完整配置示例](#完整配置示例)

---

## 🎯 鉴权模式概览

| 模式 | 安全级别 | 复杂度 | 适用场景 |
|------|----------|--------|----------|
| **API Key** | ⭐⭐⭐ | 低 | 简单 API 调用、开发测试 |
| **Bearer Token** | ⭐⭐⭐⭐ | 低 | OAuth2.0 集成、第三方服务 |
| **Basic Auth** | ⭐⭐⭐ | 低 | 传统 HTTP 认证、内部系统 |
| **Challenge-Response** | ⭐⭐⭐⭐⭐ | 高 | 高安全要求、防重放攻击 |
| **Trusted Proxy** | ⭐⭐⭐⭐ | 中 | 企业内部部署、微服务间调用 |

---

## 模式 1: API Key 鉴权

### 📝 场景说明

API Key 是最简单的鉴权方式，客户端在请求头中携带预配置的密钥，适合简单的 API 调用场景。

### ⚙️ 配置示例

```yaml
# config.yaml
gateway:
  auth:
    api_keys:
      # 方式 1: 明文密钥 (仅开发环境)
      - key: "sk-hunterclaw-dev-12345678"
        permissions:
          - "bot:message:*"    # 允许所有消息操作
          - "user:profile:read" # 允许读取用户信息
      
      # 方式 2: 环境变量模板
      - key: "${HUNTERCLAW_API_KEY}"
        permissions:
          - "bot:*"             # 允许所有 bot 操作
      
      # 方式 3: SecretRef 从文件读取
      - key: |
          {
            "source": "file",
            "provider": "default",
            "id": "/etc/hunterclaw/secrets/production.key"
          }
        permissions:
          - "*"                 # 超级管理员权限
```

### 🔄 请求流程

#### **Step 1: 客户端发起请求**

```bash
curl -X POST https://gateway.example.com/api/bot/send-message \
  -H "X-API-Key: sk-hunterclaw-dev-12345678" \
  -H "Content-Type: application/json" \
  -d '{
    "channel": "discord",
    "message": "Hello from HunterClaw!"
  }'
```

#### **Step 2: Gateway 接收请求**

```cpp
// 代码路径: src/gateway/auth_middleware.cc

std::optional<AuthContext> AuthMiddleware::authenticate_request(
    const HttpRequest& request
) {
    // 1. 提取 API Key
    auto api_key_header = request.get_header("X-API-Key");
    if (!api_key_header.has_value()) {
        // 尝试从 Query String 提取 (兼容性)
        api_key_header = request.get_query_param("api_key");
    }
    
    if (!api_key_header.has_value()) {
        return std::nullopt; // 缺少 API Key
    }
    
    std::string api_key = api_key_header.value();
    
    // 2. 查找匹配的密钥配置
    for (const auto& key_config : config_.api_keys) {
        // 解析密钥值 (支持 SecretRef)
        auto resolved_key = resolve_configured_secret(
            key_config.key, 
            secret_config_
        );
        
        if (!resolved_key.is_ok()) {
            // 密钥解析失败，跳过
            continue;
        }
        
        // 3. 比较密钥 (使用常量时间比较防止时序攻击)
        if (constant_time_compare(api_key, resolved_key.value())) {
            // ✅ 验证成功！构造认证上下文
            return AuthContext{
                .auth_type = AuthType::ApiKey,
                .user_id = "api_key_user",
                .permissions = key_config.permissions,
                .metadata = {{"key_id", key_config.id}}
            };
        }
    }
    
    // ❌ 没有匹配的密钥
    return std::nullopt;
}
```

#### **Step 3: 权限检查**

```cpp
// 代码路径: src/gateway/permission_checker.cc

bool PermissionChecker::check_permission(
    const AuthContext& ctx,
    const std::string& required_permission
) {
    // 检查是否有超级管理员权限 "*"
    if (std::find(ctx.permissions.begin(), ctx.permissions.end(), "*") 
        != ctx.permissions.end()) {
        return true; // 允许所有操作
    }
    
    // 检查精确匹配
    if (std::find(ctx.permissions.begin(), ctx.permissions.end(), required_permission) 
        != ctx.permissions.end()) {
        return true;
    }
    
    // 检查通配符匹配: "bot:message:*" 匹配 "bot:message:send"
    for (const auto& perm : ctx.permissions) {
        if (wildcard_match(perm, required_permission)) {
            return true;
        }
    }
    
    return false; // ❌ 权限不足
}
```

#### **Step 4: 执行业务逻辑**

```cpp
// 代码路径: src/gateway/handlers/bot_handler.cc

HttpResponse BotHandler::send_message(
    const HttpRequest& request,
    const AuthContext& auth
) {
    // 权限检查
    if (!permission_checker_.check_permission(auth, "bot:message:send")) {
        return HttpResponse::forbidden("Permission denied");
    }
    
    // 执行业务逻辑
    auto message = parse_message(request.body);
    auto result = bot_service_->send_message(message);
    
    return HttpResponse::json({
        {"success", true},
        {"message_id", result.message_id}
    });
}
```

### ✅ 成功响应

```json
{
  "success": true,
  "message_id": "msg_abc123xyz"
}
```

### ❌ 失败场景

| 错误 | HTTP 状态码 | 响应示例 |
|------|------------|----------|
| 缺少 API Key | 401 | `{"error": "Missing API Key"}` |
| API Key 无效 | 401 | `{"error": "Invalid API Key"}` |
| 权限不足 | 403 | `{"error": "Permission denied"}` |

---

## 模式 2: Bearer Token 鉴权

### 📝 场景说明

Bearer Token 遵循 OAuth 2.0 标准，适合与第三方服务集成（如 Discord Bot Token、GitHub Access Token）。

### ⚙️ 配置示例

```yaml
gateway:
  auth:
    bearer_tokens:
      # Discord Bot Token
      - token: "${DISCORD_BOT_TOKEN}"
        permissions:
          - "discord:*"
        metadata:
          provider: "discord"
          bot_id: "123456789"
      
      # GitHub Personal Access Token
      - token: |
          {
            "source": "exec",
            "provider": "github",
            "id": "/usr/local/bin/get-github-token"
          }
        permissions:
          - "github:repo:*"
```

### 🔄 请求流程

#### **Step 1: 客户端发起请求**

```bash
curl -X GET https://gateway.example.com/api/github/repos \
  -H "Authorization: Bearer ghp_1234567890abcdef"
```

#### **Step 2: Gateway 解析 Bearer Token**

```cpp
// 代码路径: src/gateway/auth_middleware.cc

std::optional<AuthContext> AuthMiddleware::authenticate_bearer_token(
    const HttpRequest& request
) {
    // 1. 提取 Authorization Header
    auto auth_header = request.get_header("Authorization");
    if (!auth_header.has_value()) {
        return std::nullopt;
    }
    
    // 2. 解析 "Bearer <token>" 格式
    std::string header_value = auth_header.value();
    if (header_value.substr(0, 7) != "Bearer ") {
        return std::nullopt; // 格式错误
    }
    
    std::string token = header_value.substr(7); // 去掉 "Bearer " 前缀
    
    // 3. 查找匹配的 Token 配置
    for (const auto& token_config : config_.bearer_tokens) {
        auto resolved_token = resolve_configured_secret(
            token_config.token,
            secret_config_
        );
        
        if (!resolved_token.is_ok()) {
            continue;
        }
        
        // 4. 验证 Token
        if (constant_time_compare(token, resolved_token.value())) {
            return AuthContext{
                .auth_type = AuthType::BearerToken,
                .user_id = token_config.metadata.value("bot_id", "bearer_user"),
                .permissions = token_config.permissions,
                .metadata = token_config.metadata
            };
        }
    }
    
    return std::nullopt;
}
```

### ✅ 成功响应

```json
{
  "repos": [
    {"name": "hunterclaw", "stars": 100},
    {"name": "openclaw", "stars": 50}
  ]
}
```

---

## 模式 3: Basic Auth 鉴权

### 📝 场景说明

Basic Auth 使用 Base64 编码的 `username:password`，适合传统 HTTP 认证场景。

### ⚙️ 配置示例

```yaml
gateway:
  auth:
    basic_auth:
      - username: "admin"
        password: "${ADMIN_PASSWORD}"
        permissions:
          - "*"
      
      - username: "readonly"
        password: |
          {
            "source": "file",
            "provider": "default",
            "id": "/etc/hunterclaw/secrets/readonly.pass"
          }
        permissions:
          - "bot:*:read"
```

### 🔄 请求流程

#### **Step 1: 客户端发起请求**

```bash
# Base64("admin:secret123") = "YWRtaW46c2VjcmV0MTIz"
curl -X DELETE https://gateway.example.com/api/bot/sessions/123 \
  -H "Authorization: Basic YWRtaW46c2VjcmV0MTIz"
```

#### **Step 2: Gateway 解析 Basic Auth**

```cpp
// 代码路径: src/gateway/auth_middleware.cc

std::optional<AuthContext> AuthMiddleware::authenticate_basic_auth(
    const HttpRequest& request
) {
    // 1. 提取 Authorization Header
    auto auth_header = request.get_header("Authorization");
    if (!auth_header.has_value()) {
        return std::nullopt;
    }
    
    // 2. 解析 "Basic <base64>" 格式
    std::string header_value = auth_header.value();
    if (header_value.substr(0, 6) != "Basic ") {
        return std::nullopt;
    }
    
    std::string encoded = header_value.substr(6);
    
    // 3. Base64 解码
    std::string decoded = base64_decode(encoded);
    
    // 4. 解析 "username:password"
    size_t colon_pos = decoded.find(':');
    if (colon_pos == std::string::npos) {
        return std::nullopt; // 格式错误
    }
    
    std::string username = decoded.substr(0, colon_pos);
    std::string password = decoded.substr(colon_pos + 1);
    
    // 5. 查找匹配的用户配置
    for (const auto& user_config : config_.basic_auth) {
        if (user_config.username != username) {
            continue;
        }
        
        // 6. 验证密码
        auto resolved_password = resolve_configured_secret(
            user_config.password,
            secret_config_
        );
        
        if (!resolved_password.is_ok()) {
            continue;
        }
        
        if (constant_time_compare(password, resolved_password.value())) {
            return AuthContext{
                .auth_type = AuthType::BasicAuth,
                .user_id = username,
                .permissions = user_config.permissions,
                .metadata = {{"auth_method", "basic"}}
            };
        }
    }
    
    return std::nullopt;
}
```

### ✅ 成功响应

```json
{
  "success": true,
  "message": "Session deleted"
}
```

---

## 模式 4: Challenge-Response 签名鉴权

### 📝 场景说明

**HunterClaw 独有的高安全模式**，使用 HMAC-SHA256 签名防止重放攻击，适合金融、医疗等高安全要求场景。

### ⚙️ 配置示例

```yaml
gateway:
  auth:
    challenge_response:
      - client_id: "client_prod_001"
        secret: "${CLIENT_SECRET}"
        permissions:
          - "payment:*"
        nonce_ttl: 300  # Nonce 有效期 5 分钟
```

### 🔄 请求流程

#### **Step 1: 客户端生成签名**

```python
# client.py
import hmac
import hashlib
import time
import uuid
import requests

def sign_request(client_id, secret, method, path, body):
    # 1. 生成 Nonce (防重放)
    nonce = str(uuid.uuid4())
    timestamp = str(int(time.time()))
    
    # 2. 构造签名字符串
    sign_str = f"{method}\n{path}\n{timestamp}\n{nonce}\n{body}"
    
    # 3. 计算 HMAC-SHA256 签名
    signature = hmac.new(
        secret.encode('utf-8'),
        sign_str.encode('utf-8'),
        hashlib.sha256
    ).hexdigest()
    
    return {
        "X-Client-ID": client_id,
        "X-Timestamp": timestamp,
        "X-Nonce": nonce,
        "X-Signature": signature
    }

# 发起请求
headers = sign_request(
    client_id="client_prod_001",
    secret="my_secret_key_12345",
    method="POST",
    path="/api/payment/charge",
    body='{"amount": 100, "currency": "USD"}'
)

response = requests.post(
    "https://gateway.example.com/api/payment/charge",
    headers=headers,
    json={"amount": 100, "currency": "USD"}
)
```

#### **Step 2: Gateway 验证签名**

```cpp
// 代码路径: src/gateway/auth_middleware.cc

std::optional<AuthContext> AuthMiddleware::authenticate_challenge_response(
    const HttpRequest& request
) {
    // 1. 提取签名相关的 Headers
    auto client_id_header = request.get_header("X-Client-ID");
    auto timestamp_header = request.get_header("X-Timestamp");
    auto nonce_header = request.get_header("X-Nonce");
    auto signature_header = request.get_header("X-Signature");
    
    if (!client_id_header || !timestamp_header || !nonce_header || !signature_header) {
        return std::nullopt; // 缺少必要的 Headers
    }
    
    std::string client_id = client_id_header.value();
    std::string timestamp = timestamp_header.value();
    std::string nonce = nonce_header.value();
    std::string signature = signature_header.value();
    
    // 2. 查找客户端配置
    auto client_config = find_challenge_response_config(client_id);
    if (!client_config.has_value()) {
        return std::nullopt; // 未知的客户端 ID
    }
    
    // 3. 检查 Nonce (防重放攻击)
    if (!nonce_manager_.check_nonce(nonce, client_config->nonce_ttl)) {
        return std::nullopt; // Nonce 已使用或过期
    }
    
    // 4. 检查时间戳 (防止时钟偏移攻击)
    int64_t request_time = std::stoll(timestamp);
    int64_t current_time = std::time(nullptr);
    if (std::abs(current_time - request_time) > 300) { // 5分钟时间窗口
        return std::nullopt; // 时间戳过期
    }
    
    // 5. 重构签名字符串
    std::string sign_str = request.method + "\n"
                         + request.path + "\n"
                         + timestamp + "\n"
                         + nonce + "\n"
                         + request.body;
    
    // 6. 计算期望的签名
    auto resolved_secret = resolve_configured_secret(
        client_config->secret,
        secret_config_
    );
    
    if (!resolved_secret.is_ok()) {
        return std::nullopt; // 密钥解析失败
    }
    
    std::string expected_signature = hmac_sha256(
        resolved_secret.value(),
        sign_str
    );
    
    // 7. 验证签名 (常量时间比较)
    if (!constant_time_compare(signature, expected_signature)) {
        return std::nullopt; // ❌ 签名不匹配
    }
    
    // 8. 标记 Nonce 为已使用
    nonce_manager_.mark_nonce_used(nonce);
    
    // ✅ 验证成功！
    return AuthContext{
        .auth_type = AuthType::ChallengeResponse,
        .user_id = client_id,
        .permissions = client_config->permissions,
        .metadata = {
            {"timestamp", timestamp},
            {"nonce", nonce}
        }
    };
}
```

#### **Step 3: Nonce 管理器（防重放）**

```cpp
// 代码路径: src/gateway/nonce_manager.cc

class NonceManager {
public:
    // 检查 Nonce 是否有效
    bool check_nonce(const std::string& nonce, int ttl_seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 检查是否已存在（重放攻击）
        if (used_nonces_.find(nonce) != used_nonces_.end()) {
            return false; // ❌ Nonce 已被使用
        }
        
        return true; // ✅ Nonce 有效
    }
    
    // 标记 Nonce 为已使用
    void mark_nonce_used(const std::string& nonce) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        used_nonces_[nonce] = std::time(nullptr);
    }
    
    // 清理过期的 Nonce (定期执行)
    void cleanup_expired_nonces(int ttl_seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        int64_t now = std::time(nullptr);
        for (auto it = used_nonces_.begin(); it != used_nonces_.end();) {
            if (now - it->second > ttl_seconds) {
                it = used_nonces_.erase(it); // 删除过期项
            } else {
                ++it;
            }
        }
    }

private:
    std::unordered_map<std::string, int64_t> used_nonces_; // nonce -> 使用时间
    std::mutex mutex_;
};
```

### ✅ 成功响应

```json
{
  "success": true,
  "transaction_id": "txn_abc123",
  "amount": 100,
  "currency": "USD"
}
```

### ❌ 失败场景

| 错误 | HTTP 状态码 | 原因 |
|------|------------|------|
| Nonce 重复 | 401 | 重放攻击检测 |
| 时间戳过期 | 401 | 请求超时（超过 5 分钟） |
| 签名不匹配 | 401 | 密钥错误或数据被篡改 |

---

## 模式 5: Trusted Proxy 鉴权

### 📝 场景说明

在企业内部部署中，Gateway 前端通常有反向代理（Nginx、Envoy），Trusted Proxy 模式允许代理传递已验证的用户信息。

### ⚙️ 配置示例

```yaml
gateway:
  auth:
    trusted_proxy:
      enabled: true
      proxy_ips:
        - "10.0.0.0/8"      # 内网 IP 段
        - "172.16.0.0/12"   # Docker 网络
        - "192.168.1.100"   # 特定代理 IP
      
      headers:
        user_id: "X-Authenticated-User"
        permissions: "X-User-Permissions"  # 逗号分隔
        metadata: "X-User-Metadata"        # JSON 格式
```

### 🔄 请求流程

#### **Step 1: 前端代理（Nginx）验证用户**

```nginx
# nginx.conf
location /api/ {
    # 用户通过 OAuth2.0 登录后，Nginx 设置用户信息
    proxy_set_header X-Authenticated-User "user@example.com";
    proxy_set_header X-User-Permissions "bot:*,user:profile:read";
    proxy_set_header X-User-Metadata '{"department": "engineering", "role": "admin"}';
    
    # 转发到 HunterClaw Gateway
    proxy_pass http://hunterclaw:8080;
}
```

#### **Step 2: Gateway 提取代理传递的信息**

```cpp
// 代码路径: src/gateway/auth_middleware.cc

std::optional<AuthContext> AuthMiddleware::authenticate_trusted_proxy(
    const HttpRequest& request
) {
    // 1. 检查请求来源是否是可信代理
    std::string client_ip = request.get_client_ip();
    
    if (!is_trusted_proxy_ip(client_ip, config_.trusted_proxy.proxy_ips)) {
        return std::nullopt; // ❌ 不是可信代理的请求
    }
    
    // 2. 提取代理传递的用户信息
    auto user_id_header = request.get_header(
        config_.trusted_proxy.headers.user_id
    );
    
    if (!user_id_header.has_value()) {
        return std::nullopt; // 缺少用户 ID
    }
    
    std::string user_id = user_id_header.value();
    
    // 3. 提取权限列表（逗号分隔）
    std::vector<std::string> permissions;
    auto permissions_header = request.get_header(
        config_.trusted_proxy.headers.permissions
    );
    
    if (permissions_header.has_value()) {
        permissions = split_string(permissions_header.value(), ',');
    }
    
    // 4. 提取元数据（JSON 格式）
    std::map<std::string, std::string> metadata;
    auto metadata_header = request.get_header(
        config_.trusted_proxy.headers.metadata
    );
    
    if (metadata_header.has_value()) {
        try {
            json j = json::parse(metadata_header.value());
            for (auto& [key, value] : j.items()) {
                metadata[key] = value.get<std::string>();
            }
        } catch (const std::exception&) {
            // 忽略解析错误
        }
    }
    
    // ✅ 验证成功！
    return AuthContext{
        .auth_type = AuthType::TrustedProxy,
        .user_id = user_id,
        .permissions = permissions,
        .metadata = metadata
    };
}
```

#### **Step 3: IP 地址验证**

```cpp
// 代码路径: src/gateway/ip_utils.cc

bool is_trusted_proxy_ip(
    const std::string& client_ip,
    const std::vector<std::string>& trusted_ranges
) {
    for (const auto& range : trusted_ranges) {
        // 检查 CIDR 范围（如 "10.0.0.0/8"）
        if (range.find('/') != std::string::npos) {
            if (ip_in_cidr(client_ip, range)) {
                return true;
            }
        }
        // 检查精确匹配
        else if (client_ip == range) {
            return true;
        }
    }
    
    return false;
}
```

### ✅ 成功响应

```json
{
  "message": "Hello, user@example.com!",
  "department": "engineering",
  "role": "admin"
}
```

### ⚠️ 安全注意事项

1. **仅允许可信代理 IP** - 防止客户端伪造 Headers
2. **使用加密通信** - 代理到 Gateway 之间使用 TLS
3. **定期审计** - 监控来自非预期 IP 的请求

---

## 🔑 SecretRef 密钥管理

SecretRef 提供三种方式安全管理密钥，避免明文存储在配置文件中。

### 方式 1: 环境变量模板

```yaml
gateway:
  auth:
    api_keys:
      - key: "${HUNTERCLAW_API_KEY}"  # ✅ 推荐：环境变量
        permissions: ["bot:*"]
```

**解析逻辑**:

```cpp
// 代码路径: include/hunterclaw/gateway/secret_ref.h

core::Result<std::string> resolve_configured_secret(const std::string& value) {
    // 1. 匹配 ${VAR_NAME} 模板
    auto env_var = parse_env_secret_template(value);
    if (env_var.has_value()) {
        // 2. 从环境变量读取
        const char* env_value = std::getenv(env_var.value().c_str());
        if (env_value) {
            return Result::ok(std::string(env_value));
        }
        return Result::error("Environment variable not found");
    }
    
    // 3. 不是模板，返回明文
    return Result::ok(value);
}
```

### 方式 2: 文件读取

```yaml
gateway:
  auth:
    bearer_tokens:
      - token: |
          {
            "source": "file",
            "provider": "default",
            "id": "/etc/hunterclaw/secrets/discord.token"
          }
        permissions: ["discord:*"]
```

**文件内容示例**:

```
# /etc/hunterclaw/secrets/discord.token
MTE1ODk3MjQ1MDg4NzQ0NDQ4MA.GxYzRt.abc123xyz789
```

**解析逻辑**:

```cpp
// 代码路径: include/hunterclaw/gateway/secret_ref.h

core::Result<std::string> read_secret_from_file(const std::string& file_path) {
    // 1. 打开文件
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        return Result::error("Failed to open file");
    }
    
    // 2. 检查文件大小（最大 16KB）
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    if (size > 16 * 1024) {
        return Result::error("File too large");
    }
    
    // 3. 读取内容
    file.seekg(0, std::ios::beg);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    
    // 4. Trim 空白字符
    content = trim(content);
    
    return Result::ok(content);
}
```

### 方式 3: 命令执行

```yaml
gateway:
  auth:
    api_keys:
      - key: |
          {
            "source": "exec",
            "provider": "aws",
            "id": "/usr/local/bin/get-aws-secret --name hunterclaw-api-key"
          }
        permissions: ["*"]
```

**命令脚本示例**:

```bash
#!/bin/bash
# /usr/local/bin/get-aws-secret
aws secretsmanager get-secret-value \
  --secret-id "$2" \
  --query SecretString \
  --output text
```

**解析逻辑**:

```cpp
// 代码路径: include/hunterclaw/gateway/secret_ref.h

core::Result<std::string> read_secret_from_exec(const std::string& command) {
    // 1. 安全检查：命令必须在可信目录中
    if (!is_command_in_trusted_dir(command)) {
        return Result::error("Command not trusted");
    }
    
    // 2. 执行命令
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return Result::error("Failed to execute command");
    }
    
    // 3. 读取输出（最大 16KB）
    std::string output;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
        if (output.size() > 16 * 1024) {
            pclose(pipe);
            return Result::error("Output too large");
        }
    }
    
    // 4. 检查退出码
    int exit_code = pclose(pipe);
    if (exit_code != 0) {
        return Result::error("Command failed");
    }
    
    // 5. Trim 空白字符
    output = trim(output);
    
    return Result::ok(output);
}
```

---

## ⚡ 权限控制与速率限制

### 权限通配符匹配

```cpp
// 代码路径: src/gateway/permission_checker.cc

bool wildcard_match(const std::string& pattern, const std::string& text) {
    // 支持的模式:
    // - "*"              匹配所有
    // - "bot:*"          匹配 "bot:message:send", "bot:user:read"
    // - "bot:message:*"  匹配 "bot:message:send", "bot:message:edit"
    
    if (pattern == "*") return true;
    
    size_t star_pos = pattern.find('*');
    if (star_pos == std::string::npos) {
        return pattern == text; // 精确匹配
    }
    
    std::string prefix = pattern.substr(0, star_pos);
    return text.substr(0, prefix.size()) == prefix;
}
```

### 速率限制

```yaml
gateway:
  auth:
    rate_limit:
      enabled: true
      default_limit:
        requests_per_second: 10
        burst: 20
      
      # 按权限级别设置不同限制
      per_permission:
        "payment:*":
          requests_per_second: 5  # 支付接口更严格
          burst: 10
```

**实现逻辑**:

```cpp
// 代码路径: src/gateway/auth_rate_limiter.cc

bool AuthRateLimiter::allow_request(const AuthContext& ctx) {
    std::string key = ctx.user_id;
    
    // 查找最严格的速率限制
    RateLimit limit = config_.default_limit;
    for (const auto& perm : ctx.permissions) {
        if (config_.per_permission.count(perm)) {
            limit = std::min(limit, config_.per_permission[perm]);
        }
    }
    
    // Token Bucket 算法
    auto& bucket = buckets_[key];
    auto now = std::chrono::steady_clock::now();
    
    // 补充 Token
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - bucket.last_refill
    ).count();
    
    double tokens_to_add = elapsed * limit.requests_per_second / 1000.0;
    bucket.tokens = std::min(bucket.tokens + tokens_to_add, limit.burst);
    bucket.last_refill = now;
    
    // 消耗 Token
    if (bucket.tokens >= 1.0) {
        bucket.tokens -= 1.0;
        return true; // ✅ 允许请求
    }
    
    return false; // ❌ 超过速率限制
}
```

---

## 📋 完整配置示例

```yaml
# /etc/hunterclaw/gateway.yaml
gateway:
  listen:
    address: "0.0.0.0"
    port: 8080
    tls:
      enabled: true
      cert_file: "/etc/hunterclaw/certs/server.crt"
      key_file: "/etc/hunterclaw/certs/server.key"
  
  auth:
    # 1. API Key 鉴权
    api_keys:
      - key: "${HUNTERCLAW_API_KEY_DEV}"
        permissions: ["bot:message:*", "user:profile:read"]
      
      - key: |
          {
            "source": "file",
            "provider": "default",
            "id": "/etc/hunterclaw/secrets/production.key"
          }
        permissions: ["*"]
    
    # 2. Bearer Token 鉴权
    bearer_tokens:
      - token: "${DISCORD_BOT_TOKEN}"
        permissions: ["discord:*"]
        metadata:
          provider: "discord"
      
      - token: |
          {
            "source": "exec",
            "provider": "github",
            "id": "/usr/local/bin/get-github-token"
          }
        permissions: ["github:repo:*"]
    
    # 3. Basic Auth 鉴权
    basic_auth:
      - username: "admin"
        password: "${ADMIN_PASSWORD}"
        permissions: ["*"]
      
      - username: "readonly"
        password: |
          {
            "source": "file",
            "provider": "default",
            "id": "/etc/hunterclaw/secrets/readonly.pass"
          }
        permissions: ["bot:*:read"]
    
    # 4. Challenge-Response 签名鉴权
    challenge_response:
      - client_id: "client_prod_001"
        secret: "${CLIENT_SECRET_001}"
        permissions: ["payment:*", "order:*"]
        nonce_ttl: 300
      
      - client_id: "client_prod_002"
        secret: |
          {
            "source": "exec",
            "provider": "vault",
            "id": "/usr/local/bin/vault read -field=value secret/hunterclaw/client_002"
          }
        permissions: ["analytics:*"]
        nonce_ttl: 600
    
    # 5. Trusted Proxy 鉴权
    trusted_proxy:
      enabled: true
      proxy_ips:
        - "10.0.0.0/8"
        - "172.16.0.0/12"
        - "192.168.1.100"
      headers:
        user_id: "X-Authenticated-User"
        permissions: "X-User-Permissions"
        metadata: "X-User-Metadata"
    
    # 速率限制
    rate_limit:
      enabled: true
      default_limit:
        requests_per_second: 10
        burst: 20
      per_permission:
        "payment:*":
          requests_per_second: 5
          burst: 10
        "analytics:*:write":
          requests_per_second: 20
          burst: 40
  
  # SecretRef 配置
  secret_ref:
    file:
      max_bytes: 16384  # 16KB
      reject_symlink: true
    exec:
      timeout_ms: 5000
      max_output_bytes: 16384
      trusted_dirs:
        - "/usr/local/bin"
        - "/opt/hunterclaw/scripts"
```

---

## 🎯 总结

### 鉴权模式选择指南

| 场景 | 推荐模式 | 原因 |
|------|----------|------|
| 简单 API 调用 | API Key | 实现简单，适合开发测试 |
| 第三方服务集成 | Bearer Token | 符合 OAuth2.0 标准 |
| 传统 Web 应用 | Basic Auth | 浏览器原生支持 |
| 高安全要求 | Challenge-Response | 防重放攻击，最高安全性 |
| 企业内部部署 | Trusted Proxy | 集成现有认证系统 |

### 安全最佳实践

1. ✅ **使用 SecretRef** - 避免明文密钥
2. ✅ **启用速率限制** - 防止暴力破解
3. ✅ **启用 TLS** - 加密传输
4. ✅ **最小权限原则** - 仅授予必要权限
5. ✅ **定期轮转密钥** - 定期更新 API Key/Token
6. ✅ **监控异常请求** - 记录鉴权失败日志

---

**文档版本**: v1.0  
**最后更新**: 2026-03-16  
**作者**: HunterClaw Team
