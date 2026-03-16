#!/usr/bin/env python3
"""
HunterClaw Gateway - Challenge Nonce 流程可视化演示

演示两种 Nonce 管理模式:
1. 客户端自行生成 Nonce
2. 服务端预先下发 Nonce

使用 ASCII 图表展示完整的请求流程
"""

import time
import uuid
import hmac
import hashlib
from enum import Enum
from typing import Dict, Optional, Tuple


class NonceMode(Enum):
    CLIENT_GENERATED = "client_generated"
    SERVER_ISSUED = "server_issued"


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
            if len(value) > 60:
                value = value[:57] + "..."
            print(f"  {Color.CYAN}{key:15s}{Color.RESET}: {value}")
    print()


def print_error(message: str):
    """打印错误"""
    print(f"{Color.RED}❌ {message}{Color.RESET}\n")


def print_success(message: str):
    """打印成功"""
    print(f"{Color.GREEN}✅ {message}{Color.RESET}\n")


def print_arrow(direction: str = "right"):
    """打印箭头"""
    if direction == "right":
        print(f"{Color.BLUE}{'─' * 30}▶{Color.RESET}")
    else:
        print(f"{Color.BLUE}◀{'─' * 30}{Color.RESET}")


def hmac_sha256(secret: str, message: str) -> str:
    """计算 HMAC-SHA256 签名"""
    return hmac.new(
        secret.encode('utf-8'),
        message.encode('utf-8'),
        hashlib.sha256
    ).hexdigest()


class NonceManager:
    """Nonce 管理器（模拟服务端）"""
    
    def __init__(self):
        self.used_nonces = set()
        self.pending_challenges = {}
    
    def check_nonce(self, nonce: str) -> bool:
        """检查 Nonce 是否有效（客户端生成模式）"""
        if nonce in self.used_nonces:
            return False
        return True
    
    def mark_nonce_used(self, nonce: str):
        """标记 Nonce 为已使用"""
        self.used_nonces.add(nonce)
    
    def create_challenge(self, client_id: str) -> Tuple[str, int]:
        """创建 Challenge（服务端下发模式）"""
        nonce = str(uuid.uuid4())
        expires_at = int(time.time()) + 300  # 5 分钟
        self.pending_challenges[nonce] = {
            "client_id": client_id,
            "expires_at": expires_at,
            "used": False
        }
        return nonce, expires_at
    
    def verify_challenge(self, nonce: str, client_id: str) -> bool:
        """验证 Challenge"""
        if nonce not in self.pending_challenges:
            return False
        
        challenge = self.pending_challenges[nonce]
        
        # 检查客户端 ID
        if challenge["client_id"] != client_id:
            return False
        
        # 检查是否已使用
        if challenge["used"]:
            return False
        
        # 检查是否过期
        if time.time() > challenge["expires_at"]:
            return False
        
        # 标记为已使用
        challenge["used"] = True
        return True


class ChallengeClient:
    """Challenge-Response 客户端"""
    
    def __init__(self, client_id: str, secret: str, nonce_mode: NonceMode):
        self.client_id = client_id
        self.secret = secret
        self.nonce_mode = nonce_mode
    
    def request_challenge(self) -> Tuple[str, int]:
        """请求 Challenge（仅服务端下发模式）"""
        print_step(1, "客户端请求 Challenge", {
            "Client-ID": self.client_id,
            "Endpoint": "GET /auth/challenge"
        })
        return None, None  # 实际由服务端生成
    
    def generate_nonce(self) -> str:
        """生成 Nonce（客户端生成模式）"""
        return str(uuid.uuid4())
    
    def sign_request(self, method: str, path: str, nonce: str, body: str = "") -> Dict[str, str]:
        """生成签名"""
        timestamp = str(int(time.time()))
        
        # 构造签名字符串
        sign_str = f"{method}\n{path}\n{timestamp}\n{nonce}\n{body}"
        
        # 计算签名
        signature = hmac_sha256(self.secret, sign_str)
        
        return {
            "X-Client-ID": self.client_id,
            "X-Timestamp": timestamp,
            "X-Nonce": nonce,
            "X-Signature": signature,
            "sign_str": sign_str  # 用于展示
        }


class Gateway:
    """Gateway 服务器"""
    
    def __init__(self, secret: str):
        self.secret = secret
        self.nonce_manager = NonceManager()
    
    def handle_challenge_request(self, client_id: str) -> Dict[str, any]:
        """处理 Challenge 请求"""
        print_step(2, "Gateway 生成 Challenge", {
            "Action": "生成随机 Nonce 并缓存"
        })
        
        nonce, expires_at = self.nonce_manager.create_challenge(client_id)
        
        print_step(3, "返回 Challenge", {
            "Nonce": nonce,
            "Expires At": str(expires_at)
        })
        print_arrow("left")
        
        return {"nonce": nonce, "expires_at": expires_at}
    
    def verify_request(
        self,
        method: str,
        path: str,
        headers: Dict[str, str],
        body: str,
        nonce_mode: NonceMode
    ) -> bool:
        """验证请求"""
        client_id = headers.get("X-Client-ID")
        timestamp = headers.get("X-Timestamp")
        nonce = headers.get("X-Nonce")
        signature = headers.get("X-Signature")
        
        print_section(f"Gateway 验证请求 (模式: {nonce_mode.value})")
        
        # 1. 检查 Nonce
        if nonce_mode == NonceMode.CLIENT_GENERATED:
            print_step(1, "检查 Nonce (客户端生成模式)", {
                "Nonce": nonce,
                "Status": "检查是否已使用"
            })
            if not self.nonce_manager.check_nonce(nonce):
                print_error("Nonce 已被使用 (重放攻击)")
                return False
            print_success("Nonce 有效 (首次使用)")
        else:
            print_step(1, "验证 Challenge (服务端下发模式)", {
                "Nonce": nonce,
                "Status": "检查 Challenge 缓存"
            })
            if not self.nonce_manager.verify_challenge(nonce, client_id):
                print_error("Challenge 无效或已使用")
                return False
            print_success("Challenge 有效")
        
        # 2. 检查时间戳
        req_time = int(timestamp)
        now = int(time.time())
        time_diff = abs(now - req_time)
        
        print_step(2, "检查时间戳", {
            "Timestamp": timestamp,
            "Current": str(now),
            "Difference": f"{time_diff} 秒"
        })
        
        if time_diff > 300:
            print_error(f"时间戳过期 (差异 {time_diff} 秒 > 300 秒)")
            return False
        print_success("时间戳有效")
        
        # 3. 验证签名
        sign_str = f"{method}\n{path}\n{timestamp}\n{nonce}\n{body}"
        expected_sig = hmac_sha256(self.secret, sign_str)
        
        print_step(3, "验证签名", {
            "Sign String": sign_str.replace("\n", "\\n")[:60] + "...",
            "Received Sig": signature[:40] + "...",
            "Expected Sig": expected_sig[:40] + "..."
        })
        
        if signature != expected_sig:
            print_error("签名不匹配")
            return False
        print_success("签名匹配")
        
        # 4. 标记 Nonce 为已使用（客户端生成模式）
        if nonce_mode == NonceMode.CLIENT_GENERATED:
            self.nonce_manager.mark_nonce_used(nonce)
            print_step(4, "标记 Nonce 为已使用", {
                "Nonce": nonce,
                "Status": "已标记"
            })
        
        return True


def demo_client_generated_nonce():
    """演示: 客户端生成 Nonce"""
    print_header("演示 1: 客户端自行生成 Nonce (推荐)")
    
    # 初始化
    client = ChallengeClient(
        client_id="client-1",
        secret="sk-prod-secret-123",
        nonce_mode=NonceMode.CLIENT_GENERATED
    )
    gateway = Gateway(secret="sk-prod-secret-123")
    
    # 流程
    print_section("客户端准备请求")
    
    # 1. 生成 Nonce
    nonce = client.generate_nonce()
    print_step(1, "生成 Nonce", {
        "Nonce": nonce,
        "Method": "UUID v4"
    })
    
    # 2. 签名请求
    method = "POST"
    path = "/api/transfer"
    body = '{"from":"Alice","to":"Bob","amount":1000}'
    
    print_step(2, "计算签名", {
        "Method": method,
        "Path": path,
        "Body": body
    })
    
    headers = client.sign_request(method, path, nonce, body)
    
    print_step(3, "发送请求", {
        "X-Client-ID": headers["X-Client-ID"],
        "X-Timestamp": headers["X-Timestamp"],
        "X-Nonce": headers["X-Nonce"],
        "X-Signature": headers["X-Signature"][:40] + "..."
    })
    print_arrow("right")
    
    # 3. Gateway 验证
    success = gateway.verify_request(
        method, path, headers, body,
        NonceMode.CLIENT_GENERATED
    )
    
    if success:
        print_success("✅ 请求验证成功! 业务逻辑继续执行...")
    else:
        print_error("❌ 请求验证失败!")
    
    # 4. 重放攻击测试
    print_section("测试重放攻击")
    print_step(1, "重新发送相同请求 (重放攻击)", {
        "X-Nonce": headers["X-Nonce"],
        "X-Signature": headers["X-Signature"][:40] + "..."
    })
    print_arrow("right")
    
    success = gateway.verify_request(
        method, path, headers, body,
        NonceMode.CLIENT_GENERATED
    )
    
    if not success:
        print_success("✅ 重放攻击被成功阻止!")
    else:
        print_error("❌ 重放攻击防御失败!")


def demo_server_issued_nonce():
    """演示: 服务端下发 Nonce"""
    print_header("演示 2: 服务端预先下发 Nonce (更安全)")
    
    # 初始化
    client = ChallengeClient(
        client_id="client-2",
        secret="sk-prod-secret-456",
        nonce_mode=NonceMode.SERVER_ISSUED
    )
    gateway = Gateway(secret="sk-prod-secret-456")
    
    # 流程
    print_section("第一阶段: 获取 Challenge")
    
    # 1. 请求 Challenge
    client.request_challenge()
    print_arrow("right")
    
    # 2. Gateway 返回 Challenge
    challenge = gateway.handle_challenge_request(client.client_id)
    nonce = challenge["nonce"]
    
    print_section("第二阶段: 发送业务请求")
    
    # 3. 签名请求
    method = "POST"
    path = "/api/payment"
    body = '{"amount":5000,"to":"merchant-123"}'
    
    print_step(1, "使用 Challenge Nonce 签名", {
        "Nonce": nonce,
        "Method": method,
        "Path": path
    })
    
    headers = client.sign_request(method, path, nonce, body)
    
    print_step(2, "发送业务请求", {
        "X-Client-ID": headers["X-Client-ID"],
        "X-Nonce": headers["X-Nonce"],
        "X-Signature": headers["X-Signature"][:40] + "..."
    })
    print_arrow("right")
    
    # 4. Gateway 验证
    success = gateway.verify_request(
        method, path, headers, body,
        NonceMode.SERVER_ISSUED
    )
    
    if success:
        print_success("✅ 请求验证成功! 支付处理中...")
    else:
        print_error("❌ 请求验证失败!")
    
    # 5. 重放攻击测试
    print_section("测试重放攻击")
    print_step(1, "重新发送相同请求 (重放攻击)", {
        "X-Nonce": headers["X-Nonce"],
        "X-Signature": headers["X-Signature"][:40] + "..."
    })
    print_arrow("right")
    
    success = gateway.verify_request(
        method, path, headers, body,
        NonceMode.SERVER_ISSUED
    )
    
    if not success:
        print_success("✅ 重放攻击被成功阻止!")
    else:
        print_error("❌ 重放攻击防御失败!")


def demo_comparison():
    """演示: 两种模式对比"""
    print_header("演示 3: 两种模式对比分析")
    
    print(f"{Color.BOLD}{'属性':<20} {'客户端生成':<30} {'服务端下发':<30}{Color.RESET}")
    print("-" * 80)
    
    comparisons = [
        ("网络往返次数", "1 RTT", "2 RTT"),
        ("服务端负载", "较低 (仅验证)", "较高 (需缓存 Challenge)"),
        ("安全级别", "⭐⭐⭐⭐", "⭐⭐⭐⭐⭐"),
        ("实现复杂度", "简单", "中等"),
        ("Nonce 质量", "依赖客户端", "服务端完全控制"),
        ("频率限制", "需额外机制", "天然支持"),
        ("适用场景", "高并发 API、移动应用", "金融支付、高安全场景"),
        ("时间开销", "~10 μs/请求", "~25 μs/请求 (含 Challenge)"),
    ]
    
    for attr, mode1, mode2 in comparisons:
        print(f"{attr:<20} {Color.GREEN}{mode1:<30}{Color.RESET} {Color.BLUE}{mode2:<30}{Color.RESET}")
    
    print("\n" + Color.BOLD + Color.YELLOW + "推荐选择:" + Color.RESET)
    print(f"  • {Color.GREEN}高并发场景{Color.RESET}: 客户端生成 (减少 RTT)")
    print(f"  • {Color.BLUE}高安全场景{Color.RESET}: 服务端下发 (完全控制)")
    print()


def main():
    """主函数"""
    print(f"\n{Color.BOLD}{Color.MAGENTA}")
    print("╔═══════════════════════════════════════════════════════════════════════════════╗")
    print("║                                                                               ║")
    print("║        HunterClaw Gateway - Challenge Nonce 流程可视化演示                   ║")
    print("║                                                                               ║")
    print("║        演示两种 Nonce 管理模式:                                              ║")
    print("║        1. 客户端自行生成 Nonce (推荐 - 高并发场景)                          ║")
    print("║        2. 服务端预先下发 Nonce (推荐 - 高安全场景)                          ║")
    print("║                                                                               ║")
    print("╚═══════════════════════════════════════════════════════════════════════════════╝")
    print(Color.RESET)
    
    try:
        # 演示 1: 客户端生成 Nonce
        demo_client_generated_nonce()
        
        input(f"\n{Color.YELLOW}按 Enter 继续下一个演示...{Color.RESET}")
        
        # 演示 2: 服务端下发 Nonce
        demo_server_issued_nonce()
        
        input(f"\n{Color.YELLOW}按 Enter 查看对比分析...{Color.RESET}")
        
        # 演示 3: 对比分析
        demo_comparison()
        
        print(f"\n{Color.BOLD}{Color.GREEN}🎉 演示完成!{Color.RESET}")
        print(f"\n{Color.CYAN}详细文档: CHALLENGE_NONCE_FLOW.md{Color.RESET}\n")
        
    except KeyboardInterrupt:
        print(f"\n\n{Color.YELLOW}演示已中断{Color.RESET}\n")


if __name__ == "__main__":
    main()
