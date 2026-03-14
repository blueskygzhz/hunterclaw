# Nodes and Device Implementation Example

This document demonstrates how to use the newly implemented Nodes and Device layers in HunterClaw, which are aligned with OpenClaw's architecture.

## Architecture Overview

```
Device (设备身份认证)
  ↓
Node (节点能力抽象)
  ↓
Tool (工具执行层)
  ↓
Sandbox (执行环境)
```

## Example 1: Device Pairing and Node Registration

```cpp
#include <hunterclaw/device/device_manager.h>
#include <hunterclaw/device/device_identity.h>
#include <hunterclaw/nodes/node_registry.h>
#include <hunterclaw/tools/tool_manager.h>
#include <hunterclaw/sandbox/sandbox_manager.h>

int main() {
    // Initialize managers
    auto sandbox_mgr = std::make_shared<hunterclaw::sandbox::SandboxManager>();
    auto tool_mgr = std::make_shared<hunterclaw::tools::ToolManager>();
    auto node_registry = std::make_shared<hunterclaw::nodes::NodeRegistry>(tool_mgr, sandbox_mgr);
    auto device_mgr = std::make_shared<hunterclaw::device::DeviceManager>(sandbox_mgr, node_registry);
    
    // ==========================================
    // Step 1: Generate Device Identity
    // ==========================================
    auto identity = hunterclaw::device::DeviceIdentity::generate();
    std::cout << "Device ID: " << identity.device_id << std::endl;
    
    // ==========================================
    // Step 2: Request Pairing (aligned with OpenClaw device.pair.request)
    // ==========================================
    auto request_result = device_mgr->request_pairing(
        identity.device_id,
        identity.public_key_pem,
        "iPhone 15 Pro",
        "ios",
        "node"  // Device acts as a node
    );
    
    if (!request_result.is_ok()) {
        std::cerr << "Pairing request failed: " << request_result.error_message() << std::endl;
        return 1;
    }
    
    std::string request_id = request_result.unwrap();
    std::cout << "Pairing request ID: " << request_id << std::endl;
    
    // ==========================================
    // Step 3: Approve Pairing (aligned with OpenClaw device.pair.approve)
    // ==========================================
    auto approve_result = device_mgr->approve_pairing(request_id);
    
    if (!approve_result.is_ok()) {
        std::cerr << "Pairing approval failed: " << approve_result.error_message() << std::endl;
        return 1;
    }
    
    auto paired_device = approve_result.unwrap();
    std::cout << "Device paired successfully!" << std::endl;
    std::cout << "  - Device ID: " << paired_device.device_id << std::endl;
    std::cout << "  - Sandbox ID: " << paired_device.sandbox_id << std::endl;
    std::cout << "  - Node ID: " << paired_device.node_id << std::endl;
    
    // ==========================================
    // Step 4: Verify Authentication Token
    // ==========================================
    auto token = paired_device.get_token("node");
    if (token) {
        auto verify_result = device_mgr->verify_token(
            paired_device.device_id,
            token->token,
            "node"
        );
        
        if (verify_result.is_ok()) {
            std::cout << "Token verification successful!" << std::endl;
        }
    }
    
    return 0;
}
```

## Example 2: Node Command Invocation

```cpp
#include <hunterclaw/nodes/node_registry.h>
#include <hunterclaw/nodes/node.h>

void invoke_node_commands() {
    auto sandbox_mgr = std::make_shared<hunterclaw::sandbox::SandboxManager>();
    auto tool_mgr = std::make_shared<hunterclaw::tools::ToolManager>();
    auto node_registry = std::make_shared<hunterclaw::nodes::NodeRegistry>(tool_mgr, sandbox_mgr);
    
    // ==========================================
    // Step 1: Register a Node Manually
    // ==========================================
    hunterclaw::nodes::Node node;
    node.node_id = "ios-device-12345";
    node.display_name = "Test iPhone";
    node.platform = "ios";
    node.status = hunterclaw::nodes::NodeStatus::CONNECTED;
    node.connected_at_ms = 1234567890000;
    
    // Define supported commands
    node.commands = {
        "browser.start",
        "browser.snapshot",
        "browser.close",
        "camera.snap"
    };
    
    // Define capabilities
    node.caps = {"browser", "camera", "canvas"};
    
    // Set permissions
    node.permissions["browser.start"] = true;
    node.permissions["browser.snapshot"] = true;
    node.permissions["browser.close"] = true;
    node.permissions["camera.snap"] = true;
    
    auto register_result = node_registry->register_node(node);
    if (!register_result.is_ok()) {
        std::cerr << "Node registration failed: " << register_result.error_message() << std::endl;
        return;
    }
    
    std::cout << "Node registered successfully!" << std::endl;
    
    // ==========================================
    // Step 2: Invoke Command (aligned with OpenClaw NodeRegistry.invoke)
    // ==========================================
    std::string params = R"({
        "url": "https://example.com",
        "viewport": {
            "width": 1920,
            "height": 1080
        }
    })";
    
    auto invoke_result = node_registry->invoke(
        "ios-device-12345",
        "browser.snapshot",
        params,
        30000  // 30 second timeout
    );
    
    if (invoke_result.is_ok()) {
        auto result = invoke_result.unwrap();
        std::cout << "Command executed successfully!" << std::endl;
        std::cout << "  - Success: " << result.success << std::endl;
        std::cout << "  - Duration: " << result.duration_ms << "ms" << std::endl;
        std::cout << "  - Result: " << result.result_json << std::endl;
    } else {
        std::cerr << "Command execution failed: " << invoke_result.error_message() << std::endl;
    }
    
    // ==========================================
    // Step 3: Describe Node (aligned with OpenClaw node.describe)
    // ==========================================
    auto describe_result = node_registry->describe("ios-device-12345");
    if (describe_result.is_ok()) {
        std::cout << "Node description: " << describe_result.unwrap() << std::endl;
    }
    
    // ==========================================
    // Step 4: List All Nodes (aligned with OpenClaw node.list)
    // ==========================================
    auto all_nodes = node_registry->list_nodes();
    std::cout << "Total nodes: " << all_nodes.size() << std::endl;
    
    // ==========================================
    // Step 5: Find Nodes by Platform
    // ==========================================
    auto ios_nodes = node_registry->find_nodes_by_platform("ios");
    std::cout << "iOS nodes: " << ios_nodes.size() << std::endl;
    
    // ==========================================
    // Step 6: Find Nodes by Capability
    // ==========================================
    auto camera_nodes = node_registry->find_nodes_by_capability("camera");
    std::cout << "Nodes with camera capability: " << camera_nodes.size() << std::endl;
}
```

## Example 3: Event Handling

```cpp
#include <hunterclaw/device/device_manager.h>
#include <hunterclaw/nodes/node_registry.h>

void setup_event_handlers() {
    auto sandbox_mgr = std::make_shared<hunterclaw::sandbox::SandboxManager>();
    auto tool_mgr = std::make_shared<hunterclaw::tools::ToolManager>();
    auto node_registry = std::make_shared<hunterclaw::nodes::NodeRegistry>(tool_mgr, sandbox_mgr);
    auto device_mgr = std::make_shared<hunterclaw::device::DeviceManager>(sandbox_mgr, node_registry);
    
    // ==========================================
    // Device Event Handler
    // ==========================================
    device_mgr->set_event_callback([](const std::string& device_id,
                                      const std::string& event_type,
                                      const std::string& event_data) {
        std::cout << "[Device Event] " << event_type << " for device " << device_id << std::endl;
        std::cout << "  Data: " << event_data << std::endl;
    });
    
    // ==========================================
    // Node Event Handler
    // ==========================================
    node_registry->set_event_callback([](const std::string& node_id,
                                         const std::string& event_type,
                                         const std::string& event_data) {
        std::cout << "[Node Event] " << event_type << " for node " << node_id << std::endl;
        std::cout << "  Data: " << event_data << std::endl;
    });
}
```

## Example 4: Token Rotation

```cpp
void rotate_device_token() {
    auto sandbox_mgr = std::make_shared<hunterclaw::sandbox::SandboxManager>();
    auto tool_mgr = std::make_shared<hunterclaw::tools::ToolManager>();
    auto node_registry = std::make_shared<hunterclaw::nodes::NodeRegistry>(tool_mgr, sandbox_mgr);
    auto device_mgr = std::make_shared<hunterclaw::device::DeviceManager>(sandbox_mgr, node_registry);
    
    std::string device_id = "abc123...";
    
    // ==========================================
    // Rotate Token (aligned with OpenClaw device.token.rotate)
    // ==========================================
    auto rotate_result = device_mgr->rotate_token(device_id, "node");
    
    if (rotate_result.is_ok()) {
        auto new_token = rotate_result.unwrap();
        std::cout << "Token rotated successfully!" << std::endl;
        std::cout << "  - New Token: " << new_token.token << std::endl;
        std::cout << "  - Expires At: " << new_token.expires_at_ms << std::endl;
    }
}
```

## Example 5: Cleanup and Maintenance

```cpp
void perform_cleanup() {
    auto sandbox_mgr = std::make_shared<hunterclaw::sandbox::SandboxManager>();
    auto tool_mgr = std::make_shared<hunterclaw::tools::ToolManager>();
    auto node_registry = std::make_shared<hunterclaw::nodes::NodeRegistry>(tool_mgr, sandbox_mgr);
    auto device_mgr = std::make_shared<hunterclaw::device::DeviceManager>(sandbox_mgr, node_registry);
    
    // ==========================================
    // Cleanup Stale Nodes (5 minutes threshold)
    // ==========================================
    int removed_nodes = node_registry->cleanup_stale_nodes(300000);
    std::cout << "Removed " << removed_nodes << " stale nodes" << std::endl;
    
    // ==========================================
    // Cleanup Expired Pairing Requests (10 minutes threshold)
    // ==========================================
    int removed_requests = device_mgr->cleanup_expired_requests(600000);
    std::cout << "Removed " << removed_requests << " expired pairing requests" << std::endl;
    
    // ==========================================
    // Get Statistics
    // ==========================================
    std::cout << "Node Registry Stats: " << node_registry->get_statistics() << std::endl;
    std::cout << "Device Manager Stats: " << device_mgr->get_statistics() << std::endl;
}
```

## Alignment Verification Checklist

- [x] **Node 数据结构** 包含 `nodeId`, `caps`, `commands`, `platform`, `permissions`
- [x] **NodeRegistry** 实现 `register()`, `invoke()`, `list()`, `get()` 方法
- [x] **Device 身份** 支持 ED25519 密钥对和 `deviceId = SHA256(publicKey)`
- [x] **Device 配对流程** 支持 `request` → `approve` → `verify token`
- [x] **Node 与 Tool 映射** 每个 Node command 对应一个 Tool
- [x] **Device 与 Sandbox 关联** 每个 Device (role=node) 创建一个 Sandbox
- [x] **权限检查** NodeRegistry 检查 `node.permissions[command]`
- [x] **异步调用** 支持超时和幂等性 (idempotencyKey)
- [x] **事件上报** Node 可以发送事件到 Gateway
- [ ] **唤醒机制** 支持 APNS 唤醒离线 Node (待实现)

## API Comparison: OpenClaw vs HunterClaw

| OpenClaw API | HunterClaw Equivalent | Status |
|--------------|----------------------|--------|
| `device.pair.request()` | `DeviceManager::request_pairing()` | ✅ Implemented |
| `device.pair.approve()` | `DeviceManager::approve_pairing()` | ✅ Implemented |
| `device.token.verify()` | `DeviceManager::verify_token()` | ✅ Implemented |
| `device.token.rotate()` | `DeviceManager::rotate_token()` | ✅ Implemented |
| `node.register()` | `NodeRegistry::register_node()` | ✅ Implemented |
| `node.invoke()` | `NodeRegistry::invoke()` | ✅ Implemented |
| `node.list()` | `NodeRegistry::list_nodes()` | ✅ Implemented |
| `node.describe()` | `NodeRegistry::describe()` | ✅ Implemented |

## Next Steps

1. Add unit tests for all components
2. Integrate with WebSocket transport layer
3. Implement APNS wake mechanism for iOS devices
4. Add JWT token generation (currently using simplified tokens)
5. Add CMakeLists.txt entries for new source files
