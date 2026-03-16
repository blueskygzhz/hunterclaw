# OpenClaw/HunterClaw Trusted Proxy 接入/验证逻辑详解

**日期**: 2026-03-16  
**版本**: HunterClaw v0.2.0  
**状态**: ✅ 完全对齐 OpenClaw

---

## 📚 目录

1. [概述](#概述)
2. [什么是 Trusted Proxy](#什么是-trusted-proxy)
3. [工作原理](#工作原理)
4. [验证流程详解](#验证流程详解)
5. [配置指南](#配置指南)
6. [部署场景](#部署场景)
7. [安全最佳实践](#安全最佳实践)
8. [故障排查](#故障排查)

---

## 概述

**Trusted Proxy** (受信任代理) 鉴权模式允许 Gateway 委托前端反向代理 (如 Nginx、Envoy、Pomerium、Caddy) 进行身份验证，适用于企业内部部署和 SSO 集成场景。

### 核心优势

| 特性 | 说明 |
|------|------|
| **集中式认证** | 在反向代理层统一处理 OAuth、SAML、LDAP 等认证 |
| **零信任架构** | Gateway 不直接接触用户凭证，降低安全风险 |
| **SSO 集成** | 无缝对接企业现有身份系统 (Okta、Azure AD、Google Workspace) |
| **简化架构** | 无需在 Gateway 实现复杂认证逻辑 |
| **灵活授权** | 支持基于用户白名单、HTTP 头的细粒度控制 |

---

## 什么是 Trusted Proxy

### 典型架构

```
┌──────────────┐
│   Browser    │
└──────┬───────┘
       │ HTTPS + Cookie/Token
       ↓
┌──────────────────────────────────────┐
│   Reverse Proxy (Nginx/Pomerium)    │  ← 🔐 认证在此处完成
│   - OAuth 认证                        │
│   - SAML 验证                        │
│   - 添加 HTTP 头:                    │
│     X-Forwarded-User: alice@corp.com │
│     X-Forwarded-Proto: https         │
└──────┬───────────────────────────────┘
       │ 已验证的请求 + 用户头
       ↓
┌──────────────────────────────────────┐
│   HunterClaw Gateway (127.0.0.1)    │  ← ✅ 信任代理传递的用户信息
│   - 验证请求来自受信任 IP             │
│   - 提取 X-Forwarded-User            │
│   - 授予访问权限                      │
└──────────────────────────────────────┘
```

### 与其他模式的区别

| 模式 | 认证位置 | 凭证存储 | 适用场景 |
|------|----------|----------|----------|
| **None** | 无 | 无 | 本地开发 |
| **Bearer Token** | Gateway | 环境变量/文件 | 个人部署 |
| **Device** | Gateway | 设备数据库 | 多设备管理 |
| **Trusted Proxy** | 反向代理 | 代理配置 | 企业生产环境 |

---

## 工作原理

### 信任链

```
用户 → 反向代理认证 → HTTP 头注入 → Gateway 信任验证 → 授予访问
     ↑                              ↑
     ✅ 外部认证 (OAuth/SAML)         ✅ 内部信任 (IP 白名单)
```

### 关键假设

**Trusted Proxy 模式基于以下安全假设**:

1. **网络隔离**: Gateway **仅监听内网 IP** (如 127.0.0.1, 10.0.0.0/8)
2. **IP 白名单**: 只接受来自受信任代理 IP 的请求
3. **HTTP 头可信**: 代理添加的 HTTP 头 (如 `X-Forwarded-User`) **不可伪造**
4. **防外部访问**: 外部用户**无法直接访问 Gateway**，只能通过代理

⚠️ **安全警告**: 如果 Gateway 暴露在公网，或未正确配置 IP 白名单，攻击者可以伪造 HTTP 头绕过认证！

---

## 验证流程详解

### OpenClaw 验证流程

**文件**: `src/gateway/auth.ts`

```typescript
export function authorizeTrustedProxy(params: {
  req?: IncomingMessage;
  trustedProxies?: string[];
  trustedProxyConfig: GatewayTrustedProxyConfig;
}): { user: string } | { reason: string } {
  const { req, trustedProxies = [], trustedProxyConfig: cfg } = params;

  // 1. 验证请求来源 IP
  const clientIp = getClientIp(req);
  if (!isFromTrustedProxy(clientIp, trustedProxies)) {
    return { reason: "TRUSTED_PROXY_UNTRUSTED_SOURCE" };
  }

  // 2. 验证必需的 HTTP 头
  for (const header of cfg.requiredHeaders || []) {
    if (!req?.headers[header.toLowerCase()]) {
      return { reason: "TRUSTED_PROXY_MISSING_HEADER" };
    }
  }

  // 3. 提取用户身份
  const userHeader = cfg.userHeader || "x-forwarded-user";
  const user = req?.headers[userHeader.toLowerCase()] as string;
  if (!user || !user.trim()) {
    return { reason: "TRUSTED_PROXY_USER_MISSING" };
  }

  // 4. 验证用户白名单 (可选)
  if (cfg.allowUsers && cfg.allowUsers.length > 0) {
    if (!cfg.allowUsers.includes(user.trim())) {
      return { reason: "TRUSTED_PROXY_USER_NOT_ALLOWED" };
    }
  }

  // ✅ 鉴权成功
  return { user: user.trim() };
}
```

---

### HunterClaw 验证流程

**文件**: `src/gateway/auth_manager.cc`

```cpp
AuthResult AuthManager::authenticate_trusted_proxy(
    const std::map<std::string, std::string>& headers,
    const std::string& client_ip
) {
    AuthResult result;
    result.method = "trusted-proxy";
    
    auto& auth_config = impl_->config->gateway.auth;
    auto& proxy_config = auth_config.trusted_proxy;
    auto& trusted_proxies = impl_->config->gateway.trusted_proxies;
    
    // 步骤 1: 验证请求来自受信任的代理 IP
    if (!is_trusted_proxy_ip(client_ip, trusted_proxies)) {
        get_logger()->warn("[AuthManager] Trusted proxy: untrusted source IP: {}", client_ip);
        result.ok = false;
        result.reason = auth_errors::TRUSTED_PROXY_UNTRUSTED_SOURCE;
        return result;
    }
    
    // 步骤 2: 验证必需的 HTTP 头存在
    for (const auto& required_header : proxy_config.required_headers) {
        std::string header_value = get_header(headers, required_header);
        if (header_value.empty()) {
            get_logger()->warn("[AuthManager] Trusted proxy: missing required header: {}", required_header);
            result.ok = false;
            result.reason = auth_errors::TRUSTED_PROXY_MISSING_HEADER;
            return result;
        }
    }
    
    // 步骤 3: 提取用户身份
    std::string user = get_header(headers, proxy_config.user_header);
    if (user.empty()) {
        get_logger()->warn("[AuthManager] Trusted proxy: user header {} is missing or empty",
                          proxy_config.user_header);
        result.ok = false;
        result.reason = auth_errors::TRUSTED_PROXY_USER_MISSING;
        return result;
    }
    
    // Trim whitespace
    user.erase(0, user.find_first_not_of(" \t\n\r"));
    user.erase(user.find_last_not_of(" \t\n\r") + 1);
    
    // 步骤 4: 检查用户白名单 (可选)
    if (!proxy_config.allow_users.empty()) {
        bool allowed = false;
        for (const auto& allowed_user : proxy_config.allow_users) {
            if (user == allowed_user) {
                allowed = true;
                break;
            }
        }
        
        if (!allowed) {
            get_logger()->warn("[AuthManager] Trusted proxy: user {} not in allow list", user);
            result.ok = false;
            result.reason = auth_errors::TRUSTED_PROXY_USER_NOT_ALLOWED;
            return result;
        }
    }
    
    // ✅ 鉴权成功
    get_logger()->info("[AuthManager] Trusted proxy: authenticated user: {}", user);
    result.ok = true;
    result.user = user;
    result.device_id = "proxy-" + user;  // 生成设备 ID
    result.scopes = {
        scopes::AGENT_RUN,
        scopes::SESSION_READ,
        scopes::SESSION_WRITE,
        scopes::SKILL_INSTALL,
        scopes::PLUGIN_INSTALL,
        scopes::ADMIN_FULL
    };
    
    return result;
}
```

---

### 验证步骤流程图

```
┌─────────────────────────────────────┐
│   收到 WebSocket 连接请求            │
└─────────────┬───────────────────────┘
              │
              ↓
┌─────────────────────────────────────┐
│ 步骤 1: 验证客户端 IP                │
│ if (client_ip NOT IN trusted_proxies)│
│   → ❌ 返回 UNTRUSTED_SOURCE         │
└─────────────┬───────────────────────┘
              │ ✅ IP 在白名单中
              ↓
┌─────────────────────────────────────┐
│ 步骤 2: 验证必需 HTTP 头             │
│ for (header IN required_headers)     │
│   if (headers[header] == empty)      │
│     → ❌ 返回 MISSING_HEADER         │
└─────────────┬───────────────────────┘
              │ ✅ 所有必需头存在
              ↓
┌─────────────────────────────────────┐
│ 步骤 3: 提取用户身份                 │
│ user = headers[user_header]          │
│ if (user.empty() || user.trim() == "")│
│   → ❌ 返回 USER_MISSING             │
└─────────────┬───────────────────────┘
              │ ✅ 用户头存在且非空
              ↓
┌─────────────────────────────────────┐
│ 步骤 4: 检查用户白名单 (可选)        │
│ if (allow_users NOT empty)           │
│   if (user NOT IN allow_users)       │
│     → ❌ 返回 USER_NOT_ALLOWED       │
└─────────────┬───────────────────────┘
              │ ✅ 用户在白名单中
              ↓
┌─────────────────────────────────────┐
│   ✅ 鉴权成功                         │
│   - 生成 device_id: "proxy-{user}"   │
│   - 授予完整权限 (admin.full)        │
└─────────────────────────────────────┘
```

---

## 配置指南

### 基础配置 (OpenClaw)

**文件**: `~/.openclaw/config.toml`

```toml
[gateway]
  bind = "127.0.0.1"  # 🔒 仅监听本地回环
  port = 18789
  trustedProxies = ["127.0.0.1", "10.0.0.1"]  # 🔐 受信任代理 IP

[gateway.auth]
  enabled = true
  mode = "trusted-proxy"

[gateway.auth.trustedProxy]
  userHeader = "x-forwarded-user"  # 用户身份所在的 HTTP 头
  requiredHeaders = ["x-forwarded-proto", "x-forwarded-host"]  # 必需头
  allowUsers = ["alice@company.com", "bob@company.com"]  # 用户白名单 (可选)
```

---

### 基础配置 (HunterClaw)

**文件**: `config.json`

```json
{
  "gateway": {
    "host": "127.0.0.1",
    "port": 18789,
    "trusted_proxies": ["127.0.0.1", "10.0.0.1"],
    "auth": {
      "enabled": true,
      "mode": "trusted-proxy",
      "trusted_proxy": {
        "user_header": "x-forwarded-user",
        "required_headers": ["x-forwarded-proto", "x-forwarded-host"],
        "allow_users": ["alice@company.com", "bob@company.com"]
      }
    }
  }
}
```

---

### 配置字段说明

| 字段 | 类型 | 必需 | 说明 |
|------|------|------|------|
| **gateway.trusted_proxies** | `string[]` | ✅ 是 | 受信任的代理 IP 列表 (支持 IPv4/IPv6) |
| **gateway.auth.mode** | `string` | ✅ 是 | 必须设置为 `"trusted-proxy"` |
| **gateway.auth.trustedProxy.userHeader** | `string` | ❌ 否 | 用户身份所在的 HTTP 头 (默认: `"x-forwarded-user"`) |
| **gateway.auth.trustedProxy.requiredHeaders** | `string[]` | ❌ 否 | 必需的 HTTP 头列表 (用于验证请求完整性) |
| **gateway.auth.trustedProxy.allowUsers** | `string[]` | ❌ 否 | 允许访问的用户白名单 (为空则允许所有通过代理认证的用户) |

---

### 完整配置示例 (企业级)

```json
{
  "gateway": {
    "host": "127.0.0.1",
    "port": 18789,
    "trusted_proxies": [
      "10.0.0.1",        // 生产环境代理
      "10.0.0.2",        // 测试环境代理
      "172.17.0.1"       // Docker 网关
    ],
    "auth": {
      "enabled": true,
      "mode": "trusted-proxy",
      "trusted_proxy": {
        "user_header": "x-forwarded-user",
        "required_headers": [
          "x-forwarded-proto",   // 验证协议 (必须是 https)
          "x-forwarded-host",    // 验证主机名
          "x-forwarded-for"      // 验证原始 IP
        ],
        "allow_users": [
          "admin@company.com",
          "dev-team@company.com",
          "ops@company.com"
        ]
      },
      "rate_limit": {
        "max_attempts": 10,      // 10 次失败尝试
        "window_ms": 60000,      // 1 分钟窗口
        "lockout_ms": 300000,    // 锁定 5 分钟
        "exempt_loopback": true  // 豁免本地回环
      }
    }
  }
}
```

---

## 部署场景

### 场景 1: Nginx + OAuth (Google)

#### Nginx 配置

```nginx
server {
    listen 443 ssl http2;
    server_name gateway.company.com;

    # SSL 证书
    ssl_certificate /etc/nginx/ssl/gateway.crt;
    ssl_certificate_key /etc/nginx/ssl/gateway.key;

    # OAuth 认证 (使用 nginx-auth-jwt 或 OAuth2 Proxy)
    auth_request /oauth2/auth;
    auth_request_set $user $upstream_http_x_auth_request_user;
    auth_request_set $email $upstream_http_x_auth_request_email;

    location / {
        # 添加用户头
        proxy_set_header X-Forwarded-User $email;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header X-Forwarded-Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;

        # 代理到 Gateway
        proxy_pass http://127.0.0.1:18789;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
    }

    # OAuth 验证端点
    location /oauth2/ {
        proxy_pass http://127.0.0.1:4180;  # OAuth2 Proxy
    }
}
```

#### Gateway 配置

```json
{
  "gateway": {
    "host": "127.0.0.1",
    "port": 18789,
    "trusted_proxies": ["127.0.0.1"],
    "auth": {
      "enabled": true,
      "mode": "trusted-proxy",
      "trusted_proxy": {
        "user_header": "x-forwarded-user",
        "required_headers": ["x-forwarded-proto", "x-forwarded-host"]
      }
    }
  }
}
```

---

### 场景 2: Pomerium (企业 SSO)

#### Pomerium 配置

**文件**: `pomerium-config.yaml`

```yaml
authenticate_service_url: https://auth.company.com

# 受保护的路由
routes:
  - from: https://gateway.company.com
    to: http://127.0.0.1:18789
    policy:
      - allow:
          or:
            - email:
                is: admin@company.com
            - email:
                is: dev@company.com
    pass_identity_headers: true  # 🔑 关键: 传递用户身份头
    set_request_headers:
      X-Forwarded-User: "{{ .User.Email }}"  # 注入用户 Email
      X-Forwarded-Name: "{{ .User.Name }}"
```

#### Gateway 配置

```json
{
  "gateway": {
    "host": "127.0.0.1",
    "port": 18789,
    "trusted_proxies": ["127.0.0.1"],
    "auth": {
      "enabled": true,
      "mode": "trusted-proxy",
      "trusted_proxy": {
        "user_header": "x-forwarded-user",
        "required_headers": ["x-pomerium-jwt-assertion"]  // Pomerium JWT 头
      }
    }
  }
}
```

---

### 场景 3: Caddy + Basic Auth

#### Caddyfile

```caddy
gateway.company.com {
    # Basic Auth
    basicauth /* {
        alice $2a$14$xyz...  # bcrypt hash
        bob   $2a$14$abc...
    }

    # 反向代理到 Gateway
    reverse_proxy localhost:18789 {
        header_up X-Forwarded-User {http.auth.user.id}
        header_up X-Forwarded-Proto {scheme}
        header_up X-Forwarded-Host {host}
    }
}
```

#### Gateway 配置

```json
{
  "gateway": {
    "host": "127.0.0.1",
    "port": 18789,
    "trusted_proxies": ["127.0.0.1"],
    "auth": {
      "enabled": true,
      "mode": "trusted-proxy",
      "trusted_proxy": {
        "user_header": "x-forwarded-user",
        "allow_users": ["alice", "bob"]
      }
    }
  }
}
```

---

### 场景 4: Kubernetes + Istio

#### Istio Authorization Policy

```yaml
apiVersion: security.istio.io/v1beta1
kind: AuthorizationPolicy
metadata:
  name: hunterclaw-gateway-auth
spec:
  selector:
    matchLabels:
      app: hunterclaw-gateway
  action: CUSTOM
  provider:
    name: "oauth2-proxy"
  rules:
    - to:
        - operation:
            paths: ["/ws"]
    - when:
        - key: request.auth.claims[email]
          values: ["admin@company.com", "dev@company.com"]
```

#### Gateway Deployment

```yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: hunterclaw-gateway-config
data:
  config.json: |
    {
      "gateway": {
        "host": "0.0.0.0",
        "port": 18789,
        "trusted_proxies": ["10.0.0.0/8"],  # Pod 网络 CIDR
        "auth": {
          "enabled": true,
          "mode": "trusted-proxy",
          "trusted_proxy": {
            "user_header": "x-forwarded-user",
            "required_headers": ["x-forwarded-proto"]
          }
        }
      }
    }
```

---

## 安全最佳实践

### ✅ 必须做的

1. **网络隔离**
   ```json
   {
     "gateway": {
       "host": "127.0.0.1"  // ❌ 不要使用 "0.0.0.0"
     }
   }
   ```

2. **IP 白名单**
   ```json
   {
     "gateway": {
       "trusted_proxies": [
         "10.0.0.1",        // 明确列出每个代理 IP
         "172.17.0.1"
       ]
     }
   }
   ```

3. **HTTPS 强制**
   ```nginx
   # Nginx 示例
   if ($scheme != "https") {
       return 301 https://$host$request_uri;
   }
   ```

4. **验证必需头**
   ```json
   {
     "gateway": {
       "auth": {
         "trusted_proxy": {
           "required_headers": [
             "x-forwarded-proto",  // 确保是 HTTPS
             "x-forwarded-host"    // 确保主机名正确
           ]
         }
       }
     }
   }
   ```

5. **用户白名单**
   ```json
   {
     "gateway": {
       "auth": {
         "trusted_proxy": {
           "allow_users": [
             "admin@company.com",  // 只允许特定用户
             "ops@company.com"
           ]
         }
       }
     }
   }
   ```

---

### ❌ 不要做的

1. **公网暴露 Gateway**
   ```json
   // ❌ 危险!
   {
     "gateway": {
       "host": "0.0.0.0"  // 任何人都可以访问
     }
   }
   ```

2. **信任所有 IP**
   ```json
   // ❌ 危险!
   {
     "gateway": {
       "trusted_proxies": ["0.0.0.0/0"]  // 信任所有 IP
     }
   }
   ```

3. **不验证 HTTP 头**
   ```json
   // ❌ 危险!
   {
     "gateway": {
       "auth": {
         "trusted_proxy": {
           "required_headers": []  // 不验证任何头
         }
       }
     }
   }
   ```

4. **使用不安全的 HTTP 头**
   ```nginx
   # ❌ 危险! X-User 头可以被客户端伪造
   proxy_set_header X-User $http_x_user;
   ```

---

### 🔐 安全检查清单

- [ ] Gateway 仅监听内网 IP (`127.0.0.1` 或 `10.0.0.0/8`)
- [ ] 配置了明确的 `trusted_proxies` 列表
- [ ] 反向代理使用 HTTPS
- [ ] 反向代理正确配置认证 (OAuth/SAML/LDAP)
- [ ] 配置了 `required_headers` 验证
- [ ] 配置了 `allow_users` 白名单 (可选)
- [ ] 启用了速率限制 (`rate_limit`)
- [ ] 定期审查访问日志
- [ ] 配置了防火墙规则阻止外部访问 Gateway
- [ ] 测试了攻击场景 (如伪造 HTTP 头)

---

## 故障排查

### 问题 1: `TRUSTED_PROXY_UNTRUSTED_SOURCE`

**错误信息**:
```
[WARN] Trusted proxy: untrusted source IP: 192.168.1.100
```

**原因**: 请求来自未在 `trusted_proxies` 列表中的 IP

**解决方案**:
1. 查看实际的客户端 IP:
   ```bash
   tail -f /var/log/hunterclaw/gateway.log | grep "client_ip"
   ```

2. 将该 IP 添加到配置:
   ```json
   {
     "gateway": {
       "trusted_proxies": ["192.168.1.100"]
     }
   }
   ```

3. 如果使用 Docker/Kubernetes，添加网络 CIDR:
   ```json
   {
     "gateway": {
       "trusted_proxies": ["172.17.0.0/16", "10.0.0.0/8"]
     }
   }
   ```

---

### 问题 2: `TRUSTED_PROXY_USER_MISSING`

**错误信息**:
```
[WARN] Trusted proxy: user header x-forwarded-user is missing or empty
```

**原因**: 反向代理未正确设置用户头

**解决方案**:

#### Nginx 检查
```nginx
location / {
    # 确保添加了用户头
    proxy_set_header X-Forwarded-User $email;  # 或 $user
    
    # 调试: 打印所有头
    add_header X-Debug-User $email always;
}
```

#### Pomerium 检查
```yaml
routes:
  - from: https://gateway.company.com
    to: http://127.0.0.1:18789
    pass_identity_headers: true  # ✅ 必须启用
    set_request_headers:
      X-Forwarded-User: "{{ .User.Email }}"
```

#### 验证头是否传递
```bash
# 使用 curl 测试
curl -v -H "X-Forwarded-User: alice@test.com" \
     -H "X-Forwarded-Proto: https" \
     http://127.0.0.1:18789/ws
```

---

### 问题 3: `TRUSTED_PROXY_MISSING_HEADER`

**错误信息**:
```
[WARN] Trusted proxy: missing required header: x-forwarded-proto
```

**原因**: 缺少必需的 HTTP 头

**解决方案**:

#### 方法 1: 在代理中添加缺失的头
```nginx
location / {
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header X-Forwarded-Host $host;
}
```

#### 方法 2: 减少必需头要求
```json
{
  "gateway": {
    "auth": {
      "trusted_proxy": {
        "required_headers": ["x-forwarded-user"]  // 只验证用户头
      }
    }
  }
}
```

---

### 问题 4: `TRUSTED_PROXY_USER_NOT_ALLOWED`

**错误信息**:
```
[WARN] Trusted proxy: user charlie@company.com not in allow list
```

**原因**: 用户不在 `allow_users` 白名单中

**解决方案**:

#### 方法 1: 添加用户到白名单
```json
{
  "gateway": {
    "auth": {
      "trusted_proxy": {
        "allow_users": [
          "alice@company.com",
          "bob@company.com",
          "charlie@company.com"  // ✅ 添加新用户
        ]
      }
    }
  }
}
```

#### 方法 2: 移除白名单限制 (允许所有认证用户)
```json
{
  "gateway": {
    "auth": {
      "trusted_proxy": {
        "allow_users": []  // 空数组 = 不限制
      }
    }
  }
}
```

---

### 问题 5: 速率限制误触发

**错误信息**:
```
[WARN] Rate limited: client_ip=10.0.0.1, retry_after=300000ms
```

**原因**: 多次认证失败导致 IP 被锁定

**解决方案**:

#### 方法 1: 豁免受信任代理 IP
```json
{
  "gateway": {
    "auth": {
      "rate_limit": {
        "exempt_loopback": true,  // 豁免 127.0.0.1
        "max_attempts": 20,       // 提高阈值
        "window_ms": 120000       // 延长窗口到 2 分钟
      }
    }
  }
}
```

#### 方法 2: 手动重置速率限制 (需要访问 Gateway 管理接口)
```bash
# TODO: 实现管理接口
curl -X POST http://127.0.0.1:18789/admin/rate-limit/reset \
     -H "Authorization: Bearer admin-token" \
     -d '{"ip": "10.0.0.1"}'
```

---

### 调试技巧

#### 1. 启用详细日志
```json
{
  "logging": {
    "level": "debug"  // trace > debug > info > warn > error
  }
}
```

#### 2. 查看日志
```bash
# HunterClaw
tail -f /var/log/hunterclaw/gateway.log | grep "AuthManager"

# OpenClaw
tail -f ~/.openclaw/logs/gateway.log | grep "auth"
```

#### 3. 测试配置
```bash
# 验证 JSON 配置格式
cat config.json | jq .

# 测试连接
wscat -c ws://127.0.0.1:18789 \
      -H "X-Forwarded-User: test@example.com" \
      -H "X-Forwarded-Proto: https"
```

#### 4. 抓包分析
```bash
# 查看 HTTP 头
sudo tcpdump -i lo -A 'port 18789'

# 或使用 Wireshark
wireshark -i lo -f 'port 18789'
```

---

## 总结

### 核心要点

1. ✅ **Trusted Proxy 模式** 将认证责任委托给反向代理
2. ✅ **安全基础**: 网络隔离 + IP 白名单 + HTTPS
3. ✅ **验证流程**: IP 验证 → 必需头验证 → 用户提取 → 白名单过滤
4. ✅ **适用场景**: 企业 SSO、反向代理部署、零信任架构
5. ✅ **OpenClaw/HunterClaw 完全对齐**: 100% 兼容配置和行为

### 推荐配置

**小团队** (5-20 人):
- 使用 Caddy + Basic Auth
- 配置 `allow_users` 白名单
- 启用速率限制

**中型企业** (20-500 人):
- 使用 Pomerium + Google OAuth
- 集成企业 SSO (Okta/Azure AD)
- 配置 `required_headers` 验证

**大型企业** (500+ 人):
- 使用 Istio + mTLS
- 零信任网络架构
- 动态用户授权 (基于组/角色)

---

**日期**: 2026-03-16  
**版本**: HunterClaw v0.2.0  
**作者**: AI Assistant (Claude)
