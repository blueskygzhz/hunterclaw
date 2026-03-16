#!/usr/bin/env python3
"""
HunterClaw Gateway - Bearer Token 认证实际演示

演示内容:
1. Bearer Token 基本用法
2. 不同格式的 Token 传递
3. 错误处理
4. 安全最佳实践

运行: python3 bearer_token_demo.py
"""

import time
import hmac
import hashlib
import requests
from typing import Optional, Dict, Any
from dataclasses import dataclass
from enum import Enum


class Color:
    """终端颜色"""
    RESET = '\033[0m'
    BOLD = '\033[1m'
    GREEN = '\033[92m'
    BLUE = '\033[94m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    CYAN = '\033[96m'
    MAGENTA = '\033[95m'


def print_header(title: str):
    """打印标题"""
    width = 80
    print(f"\n{Color.BOLD}{Color.CYAN}{'=' * width}{Color.RESET}")
    print(f"{Color.BOLD}{Color.CYAN}{title.center(width)}{Color.RESET}")
    print(f"{Color.BOLD}{Color.CYAN}{'=' * width}{Color.RESET}\n")


def print_section(title: str):
    """打印章节"""
    print(f"\n{Color.BOLD}{Color.YELLOW}▶ {title}{Color.RESET}\n")


def print_step(step_num: int, description: str, details: Dict[str, str] = None):
    """打印步骤"""
    print(f"{Color.BOLD}{Color.GREEN}[Step {step_num}]{Color.RESET} {description}")
    if details:
        for key, value in details.items():
            # 截断过长的值
            if len(str(value)) > 60:
                value = str(value)[:57] + "..."
            print(f"  {Color.CYAN}{key:20s}{Color.RESET}: {value}")
    print()


def print_error(message: str):
    """打印错误"""
    print(f"{Color.RED}❌ {message}{Color.RESET}\n")


def print_success(message: str):
    """打印成功"""
    print(f"{Color.GREEN}✅ {message}{Color.RESET}\n")


def print_warning(message: str):
    """打印警告"""
    print(f"{Color.YELLOW}⚠️  {message}{Color.RESET}\n")


def print_code(code: str, language: str = ""):
    """打印代码块"""
    print(f"{Color.BLUE}```{language}")
    print(code)
    print(f"```{Color.RESET}\n")


# ============================================================================
# 模拟的 Gateway 服务器
# ============================================================================

class MockGateway:
    """模拟的 Gateway 服务器"""
    
    def __init__(self, valid_token: str):
        self.valid_token = valid_token
        self.request_log = []
        self.failed_attempts = {}  # client_ip -> [timestamps]
        
    def constant_time_compare(self, a: str, b: str) -> bool:
        """常量时间比较（防止时序攻击）"""
        if len(a) != len(b):
            return False
        
        result = 0
        for x, y in zip(a, b):
            result |= ord(x) ^ ord(y)
        
        return result == 0
    
    def check_rate_limit(self, client_ip: str) -> tuple[bool, int]:
        """检查速率限制"""
        now = time.time()
        window = 60  # 1 分钟窗口
        max_attempts = 5
        lockout = 300  # 锁定 5 分钟
        
        if client_ip not in self.failed_attempts:
            self.failed_attempts[client_ip] = []
        
        # 清理过期记录
        self.failed_attempts[client_ip] = [
            ts for ts in self.failed_attempts[client_ip]
            if now - ts < window
        ]
        
        # 检查是否被锁定
        if self.failed_attempts[client_ip]:
            last_fail = self.failed_attempts[client_ip][-1]
            if len(self.failed_attempts[client_ip]) >= max_attempts:
                time_since_lock = now - last_fail
                if time_since_lock < lockout:
                    return False, int(lockout - time_since_lock)
        
        return True, 0
    
    def record_failure(self, client_ip: str):
        """记录失败"""
        if client_ip not in self.failed_attempts:
            self.failed_attempts[client_ip] = []
        self.failed_attempts[client_ip].append(time.time())
    
    def reset_limit(self, client_ip: str):
        """重置速率限制"""
        if client_ip in self.failed_attempts:
            del self.failed_attempts[client_ip]
    
    def verify_token(self, auth_header: str, client_ip: str) -> tuple[bool, str, dict]:
        """
        验证 Bearer Token
        
        Returns:
            (success, reason, response_data)
        """
        # 1. 检查速率限制
        allowed, retry_after = self.check_rate_limit(client_ip)
        if not allowed:
            return False, "RATE_LIMITED", {
                "error": "RATE_LIMITED",
                "message": "Too many failed authentication attempts",
                "retry_after_ms": retry_after * 1000
            }
        
        # 2. 提取 Token
        if not auth_header.startswith("Bearer "):
            self.record_failure(client_ip)
            return False, "INVALID_FORMAT", {
                "error": "INVALID_FORMAT",
                "message": "Authorization header must start with 'Bearer '"
            }
        
        token = auth_header[7:].strip()  # 去掉 "Bearer " 前缀
        
        # 3. 验证 Token（使用常量时间比较）
        if not self.constant_time_compare(token, self.valid_token):
            self.record_failure(client_ip)
            return False, "NOT_AUTHENTICATED", {
                "error": "NOT_AUTHENTICATED",
                "message": "Invalid bearer token"
            }
        
        # 4. 验证成功
        self.reset_limit(client_ip)
        return True, "OK", {
            "status": "success",
            "message": "Authentication successful",
            "scopes": [
                "agent.run",
                "session.read",
                "session.write",
                "admin.full"
            ]
        }
    
    def handle_request(
        self,
        method: str,
        path: str,
        headers: Dict[str, str],
        body: Dict[str, Any],
        client_ip: str
    ) -> tuple[int, dict]:
        """处理请求"""
        # 记录请求
        self.request_log.append({
            "timestamp": time.time(),
            "method": method,
            "path": path,
            "client_ip": client_ip,
            "headers": headers
        })
        
        # 提取 Authorization 头
        auth_header = headers.get("Authorization", "")
        if not auth_header:
            return 401, {
                "error": "MISSING_AUTH",
                "message": "Missing Authorization header"
            }
        
        # 验证 Token
        success, reason, response = self.verify_token(auth_header, client_ip)
        
        if not success:
            if reason == "RATE_LIMITED":
                return 429, response
            else:
                return 401, response
        
        # 成功，执行业务逻辑
        if path == "/api/transfer":
            return 200, {
                "status": "success",
                "transaction_id": "txn-" + str(int(time.time())),
                "data": body
            }
        elif path == "/api/data":
            return 200, {
                "status": "success",
                "data": body
            }
        else:
            return 404, {
                "error": "NOT_FOUND",
                "message": f"Path {path} not found"
            }


# ============================================================================
# 客户端实现
# ============================================================================

class HunterClawClient:
    """HunterClaw Gateway 客户端"""
    
    def __init__(self, base_url: str, bearer_token: str, mock_gateway: MockGateway = None):
        self.base_url = base_url
        self.bearer_token = bearer_token
        self.mock_gateway = mock_gateway  # 用于演示
        self.client_ip = "192.168.1.100"  # 模拟客户端 IP
    
    def _get_headers(self) -> dict:
        """构造请求头"""
        return {
            "Authorization": f"Bearer {self.bearer_token}",
            "Content-Type": "application/json"
        }
    
    def post(self, path: str, data: dict) -> tuple[int, dict]:
        """发送 POST 请求"""
        if self.mock_gateway:
            # 使用模拟 Gateway
            return self.mock_gateway.handle_request(
                "POST",
                path,
                self._get_headers(),
                data,
                self.client_ip
            )
        else:
            # 真实 HTTP 请求
            url = f"{self.base_url}{path}"
            response = requests.post(url, headers=self._get_headers(), json=data)
            return response.status_code, response.json()


# ============================================================================
# 演示场景
# ============================================================================

def demo_basic_usage():
    """演示 1: 基本用法"""
    print_header("演示 1: Bearer Token 基本用法")
    
    # 1. 初始化
    print_section("初始化客户端和服务器")
    
    valid_token = "sk-hunterclaw-abc123"
    gateway = MockGateway(valid_token)
    client = HunterClawClient(
        base_url="https://gateway.example.com",
        bearer_token=valid_token,
        mock_gateway=gateway
    )
    
    print_step(1, "创建 Gateway 服务器", {
        "Valid Token": valid_token,
        "Rate Limit": "5 attempts / 1 minute"
    })
    
    print_step(2, "创建客户端", {
        "Base URL": "https://gateway.example.com",
        "Bearer Token": valid_token
    })
    
    # 2. 发送请求
    print_section("发送认证请求")
    
    print_step(3, "构造请求", {
        "Method": "POST",
        "Path": "/api/transfer",
        "Headers": "Authorization: Bearer sk-hunterclaw-abc123"
    })
    
    status, response = client.post("/api/transfer", {
        "from": "Alice",
        "to": "Bob",
        "amount": 1000
    })
    
    print_step(4, "Gateway 验证流程", {
        "1. 提取 Token": "sk-hunterclaw-abc123",
        "2. 常量时间比较": "✅ 匹配",
        "3. 权限检查": "✅ 通过"
    })
    
    if status == 200:
        print_success(f"请求成功! 状态码: {status}")
        print(f"{Color.CYAN}响应数据:{Color.RESET}")
        print_code(str(response), "json")
    else:
        print_error(f"请求失败! 状态码: {status}")
        print_code(str(response), "json")


def demo_token_formats():
    """演示 2: 不同的 Token 格式"""
    print_header("演示 2: 不同的 Bearer Token 格式")
    
    valid_token = "sk-hunterclaw-xyz789"
    gateway = MockGateway(valid_token)
    
    test_cases = [
        {
            "name": "正确格式 (推荐)",
            "auth_header": f"Bearer {valid_token}",
            "should_succeed": True
        },
        {
            "name": "缺少 'Bearer ' 前缀",
            "auth_header": valid_token,
            "should_succeed": False
        },
        {
            "name": "错误的 Token",
            "auth_header": "Bearer sk-hunterclaw-wrong",
            "should_succeed": False
        },
        {
            "name": "空格问题",
            "auth_header": f"Bearer  {valid_token}",  # 两个空格
            "should_succeed": True  # 会 trim
        },
        {
            "name": "大小写错误",
            "auth_header": f"bearer {valid_token}",  # 小写
            "should_succeed": False
        }
    ]
    
    for i, test in enumerate(test_cases, 1):
        print_section(f"测试 {i}: {test['name']}")
        
        print_step(1, "发送请求", {
            "Authorization": test["auth_header"][:50] + ("..." if len(test["auth_header"]) > 50 else "")
        })
        
        success, reason, response = gateway.verify_token(
            test["auth_header"],
            "192.168.1.100"
        )
        
        if success == test["should_succeed"]:
            print_success(f"✅ 测试通过! (预期: {'成功' if test['should_succeed'] else '失败'})")
        else:
            print_error(f"❌ 测试失败! (预期: {'成功' if test['should_succeed'] else '失败'}，实际: {'成功' if success else '失败'})")
        
        if not success:
            print(f"{Color.RED}原因: {reason}{Color.RESET}")
            print_code(str(response), "json")


def demo_rate_limiting():
    """演示 3: 速率限制"""
    print_header("演示 3: 速率限制和暴力破解防护")
    
    valid_token = "sk-hunterclaw-secure123"
    gateway = MockGateway(valid_token)
    
    print_section("场景: 攻击者尝试暴力破解")
    
    # 尝试多次错误的 Token
    for i in range(7):
        print_step(i + 1, f"尝试 #{i + 1}", {
            "Token": f"sk-hunterclaw-wrong{i}",
            "Client IP": "192.168.1.100"
        })
        
        success, reason, response = gateway.verify_token(
            f"Bearer sk-hunterclaw-wrong{i}",
            "192.168.1.100"
        )
        
        if reason == "RATE_LIMITED":
            retry_after = response["retry_after_ms"] // 1000
            print_error(f"速率限制触发! 请 {retry_after} 秒后重试")
            print_code(str(response), "json")
            break
        else:
            print_warning(f"认证失败 ({i + 1}/5)")
    
    # 尝试使用正确的 Token（但被锁定）
    print_section("使用正确 Token 尝试（但已被锁定）")
    
    print_step(1, "发送请求", {
        "Token": valid_token + " (正确!)",
        "Client IP": "192.168.1.100"
    })
    
    success, reason, response = gateway.verify_token(
        f"Bearer {valid_token}",
        "192.168.1.100"
    )
    
    if not success and reason == "RATE_LIMITED":
        print_error("即使 Token 正确，仍被速率限制阻止!")
        print_code(str(response), "json")


def demo_security_best_practices():
    """演示 4: 安全最佳实践"""
    print_header("演示 4: 安全最佳实践")
    
    print_section("❌ 不安全的做法")
    
    print_warning("1. Token 硬编码在代码中")
    print_code("""
# ❌ 不要这样做
bearer_token = "sk-hunterclaw-abc123"
client = HunterClawClient(base_url, bearer_token)
    """, "python")
    
    print_warning("2. Token 泄露到 Git 仓库")
    print_code("""
# config.yaml (❌ 不要提交到 Git)
gateway:
  auth:
    bearer_token: "sk-hunterclaw-abc123"
    """, "yaml")
    
    print_section("✅ 安全的做法")
    
    print_success("1. 使用环境变量")
    print_code("""
# ✅ 推荐做法
import os

bearer_token = os.getenv("HUNTERCLAW_TOKEN")
if not bearer_token:
    raise ValueError("HUNTERCLAW_TOKEN environment variable not set")

client = HunterClawClient(base_url, bearer_token)
    """, "python")
    
    print_success("2. 从密钥文件读取")
    print_code("""
# ✅ 推荐做法
def load_token_from_file(filepath: str) -> str:
    with open(filepath, 'r') as f:
        return f.read().strip()

# 文件权限: chmod 600
bearer_token = load_token_from_file("/etc/hunterclaw/secrets/token")
client = HunterClawClient(base_url, bearer_token)
    """, "python")
    
    print_success("3. 使用密钥管理服务")
    print_code("""
# ✅ 推荐做法 (AWS Secrets Manager)
import boto3

def get_token_from_aws() -> str:
    client = boto3.client('secretsmanager')
    response = client.get_secret_value(SecretId='hunterclaw-bearer-token')
    return response['SecretString']

bearer_token = get_token_from_aws()
client = HunterClawClient(base_url, bearer_token)
    """, "python")


def demo_error_handling():
    """演示 5: 错误处理"""
    print_header("演示 5: 错误处理和调试")
    
    valid_token = "sk-hunterclaw-prod123"
    gateway = MockGateway(valid_token)
    
    error_scenarios = [
        {
            "name": "缺少 Authorization 头",
            "headers": {"Content-Type": "application/json"},
            "expected_error": "MISSING_AUTH"
        },
        {
            "name": "错误的 Token 格式",
            "headers": {"Authorization": "InvalidFormat"},
            "expected_error": "INVALID_FORMAT"
        },
        {
            "name": "Token 不匹配",
            "headers": {"Authorization": "Bearer sk-wrong-token"},
            "expected_error": "NOT_AUTHENTICATED"
        }
    ]
    
    for i, scenario in enumerate(error_scenarios, 1):
        print_section(f"错误场景 {i}: {scenario['name']}")
        
        print_step(1, "发送请求", {
            "Headers": str(scenario["headers"])
        })
        
        if "Authorization" in scenario["headers"]:
            success, reason, response = gateway.verify_token(
                scenario["headers"]["Authorization"],
                "192.168.1.100"
            )
        else:
            success, reason, response = False, "MISSING_AUTH", {
                "error": "MISSING_AUTH",
                "message": "Missing Authorization header"
            }
        
        print_error(f"认证失败: {reason}")
        print_code(str(response), "json")
        
        print(f"{Color.YELLOW}调试建议:{Color.RESET}")
        if reason == "MISSING_AUTH":
            print("  • 确认请求头中包含 'Authorization' 字段")
        elif reason == "INVALID_FORMAT":
            print("  • 确认格式为: 'Bearer <token>'")
            print("  • 注意 'Bearer' 后面有一个空格")
        elif reason == "NOT_AUTHENTICATED":
            print("  • 确认 Token 是否正确")
            print("  • 检查环境变量是否正确设置")
            print("  • 联系管理员获取有效 Token")
        print()


def main():
    """主函数"""
    print(f"\n{Color.BOLD}{Color.MAGENTA}")
    print("╔═══════════════════════════════════════════════════════════════════════════════╗")
    print("║                                                                               ║")
    print("║        HunterClaw Gateway - Bearer Token 认证实际演示                        ║")
    print("║                                                                               ║")
    print("║        演示内容:                                                             ║")
    print("║        1. Bearer Token 基本用法                                              ║")
    print("║        2. 不同格式的 Token 传递                                              ║")
    print("║        3. 速率限制和暴力破解防护                                            ║")
    print("║        4. 安全最佳实践                                                       ║")
    print("║        5. 错误处理和调试                                                     ║")
    print("║                                                                               ║")
    print("╚═══════════════════════════════════════════════════════════════════════════════╝")
    print(Color.RESET)
    
    try:
        # 演示 1: 基本用法
        demo_basic_usage()
        input(f"\n{Color.YELLOW}按 Enter 继续下一个演示...{Color.RESET}")
        
        # 演示 2: Token 格式
        demo_token_formats()
        input(f"\n{Color.YELLOW}按 Enter 继续下一个演示...{Color.RESET}")
        
        # 演示 3: 速率限制
        demo_rate_limiting()
        input(f"\n{Color.YELLOW}按 Enter 继续下一个演示...{Color.RESET}")
        
        # 演示 4: 安全最佳实践
        demo_security_best_practices()
        input(f"\n{Color.YELLOW}按 Enter 继续下一个演示...{Color.RESET}")
        
        # 演示 5: 错误处理
        demo_error_handling()
        
        print(f"\n{Color.BOLD}{Color.GREEN}🎉 演示完成!{Color.RESET}")
        print(f"\n{Color.CYAN}详细文档: BEARER_TOKEN_FLOW.md{Color.RESET}\n")
        
    except KeyboardInterrupt:
        print(f"\n\n{Color.YELLOW}演示已中断{Color.RESET}\n")


if __name__ == "__main__":
    main()
