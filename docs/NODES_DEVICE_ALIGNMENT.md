# HunterClaw Nodes and Device Implementation

## Overview

This document describes the implementation of **Nodes** and **Device** layers in HunterClaw, which are strictly aligned with OpenClaw's architecture.

## Directory Structure

```
HunterClaw/
├── include/hunterclaw/
│   ├── nodes/
│   │   ├── node.h                  # Node data structure
│   │   └── node_registry.h         # Node registry and management
│   └── device/
│       ├── device_identity.h       # Device identity and authentication
│       └── device_manager.h        # Device pairing and lifecycle
├── src/
│   ├── nodes/
│   │   ├── node.cc                 # Node implementation
│   │   └── node_registry.cc        # NodeRegistry implementation
│   └── device/
│       ├── device_identity.cc      # Device identity implementation
│       └── device_manager.cc       # DeviceManager implementation
└── docs/
    ├── NODES_DEVICE_ALIGNMENT.md   # This document
    └── NODES_DEVICE_EXAMPLES.md    # Usage examples
```

## Core Components

### 1. Node (节点)

**Header:** `include/hunterclaw/nodes/node.h`

**Purpose:** Represents a connected device/agent that can execute commands.

**Key Fields:**
- `node_id`: Unique node identifier
- `platform`: Platform type (ios/android/macos)
- `commands`: List of supported commands
- `caps`: List of capabilities
- `permissions`: Command permission mappings
- `status`: Connection status

**Alignment with OpenClaw:**
```typescript
// OpenClaw NodeSession
interface NodeSession {
  nodeId: string;
  connId: string;
  caps: string[];
  commands: string[];
  platform: string;
  permissions: Record<string, boolean>;
}

// HunterClaw Node
struct Node {
    std::string node_id;
    std::string conn_id;
    std::vector<std::string> caps;
    std::vector<std::string> commands;
    std::string platform;
    std::map<std::string, bool> permissions;
};
```

### 2. NodeRegistry (节点注册表)

**Header:** `include/hunterclaw/nodes/node_registry.h`

**Purpose:** Manages registered nodes and routes commands to them.

**Key Methods:**
- `register_node()`: Register a new node (OpenClaw: `node.register`)
- `invoke()`: Invoke command on a node (OpenClaw: `NodeRegistry.invoke`)
- `list_nodes()`: List all nodes (OpenClaw: `node.list`)
- `describe()`: Get node details (OpenClaw: `node.describe`)

**Command Routing:**
```
NodeRegistry::invoke(node_id, command, params)
    ↓
Check node exists and is connected
    ↓
Check command support and permissions
    ↓
Route to ToolManager with node context
    ↓
Return NodeInvokeResult
```

### 3. DeviceIdentity (设备身份)

**Header:** `include/hunterclaw/device/device_identity.h`

**Purpose:** Device authentication using ED25519 key pairs.

**Key Features:**
- ED25519 public/private key pairs
- `device_id = SHA256(public_key)`
- Signature verification
- Message signing

**Alignment with OpenClaw:**
```typescript
// OpenClaw
interface DeviceIdentity {
  deviceId: string;           // SHA256(publicKey)
  publicKeyPem: string;       // ED25519 公钥
  privateKeyPem: string;      // ED25519 私钥
}

// HunterClaw
struct DeviceIdentity {
    std::string device_id;        // SHA256(public_key)
    std::string public_key_pem;   // ED25519 公钥
    std::string private_key_pem;  // ED25519 私钥
};
```

### 4. DeviceManager (设备管理器)

**Header:** `include/hunterclaw/device/device_manager.h`

**Purpose:** Handles device pairing and lifecycle management.

**Key Methods:**
- `request_pairing()`: Request device pairing (OpenClaw: `device.pair.request`)
- `approve_pairing()`: Approve pairing (OpenClaw: `device.pair.approve`)
- `verify_token()`: Verify authentication token (OpenClaw: `device.token.verify`)
- `rotate_token()`: Rotate token (OpenClaw: `device.token.rotate`)

**Pairing Flow:**
```
1. request_pairing(device_id, public_key, display_name, platform, role)
   ↓
2. approve_pairing(request_id)
   ↓ (if role == "node")
3. Create Sandbox
   ↓
4. Register Node in NodeRegistry
   ↓
5. Return PairedDevice with token
```

## Architecture Diagram

```
┌─────────────────────────────────────────────────┐
│              DeviceManager                      │
│  - request_pairing()                            │
│  - approve_pairing()                            │
│  - verify_token()                               │
└──────────────┬──────────────────────────────────┘
               │ (role="node")
               ↓
┌─────────────────────────────────────────────────┐
│              NodeRegistry                       │
│  - register_node()                              │
│  - invoke(node_id, command, params)             │
│  - list_nodes()                                 │
└──────────────┬──────────────────────────────────┘
               │ (route to tools)
               ↓
┌─────────────────────────────────────────────────┐
│              Tool System                        │
│  - execute_tool()                               │
│  - BrowserTool, TerminalTool, etc.              │
└──────────────┬──────────────────────────────────┘
               │ (execute in sandbox)
               ↓
┌─────────────────────────────────────────────────┐
│              SandboxManager                     │
│  - create_sandbox()                             │
│  - execute_command()                            │
└─────────────────────────────────────────────────┘
```

## Integration Points

### ToolManager Integration

**Requirement:** ToolManager needs to support node context in command execution.

**Proposed Method:**
```cpp
// Add to ToolManager
core::Result<ToolResult> execute_tool_with_context(
    const std::string& tool_name,
    const std::string& params_json,
    const std::map<std::string, std::string>& context,  // NEW: node context
    int timeout_ms = 30000
);
```

**Context Fields:**
- `node_id`: Executing node identifier
- `device_id`: Associated device identifier
- `sandbox_id`: Associated sandbox identifier
- `idempotency_key`: For deduplication

### SandboxManager Integration

**Requirement:** SandboxManager needs to associate sandboxes with devices.

**Proposed Method:**
```cpp
// Add to SandboxManager
core::Result<SandboxInfo> create_sandbox_for_device(
    const std::string& device_id,
    const SandboxConfig& config
);

// Filter sandboxes by device
std::vector<SandboxInfo> get_sandboxes_by_device(
    const std::string& device_id
);
```

## API Mapping: OpenClaw ↔ HunterClaw

| OpenClaw API | HunterClaw API | Status |
|--------------|----------------|--------|
| `device.pair.request()` | `DeviceManager::request_pairing()` | ✅ |
| `device.pair.approve()` | `DeviceManager::approve_pairing()` | ✅ |
| `device.pair.reject()` | `DeviceManager::reject_pairing()` | ✅ |
| `device.token.verify()` | `DeviceManager::verify_token()` | ✅ |
| `device.token.rotate()` | `DeviceManager::rotate_token()` | ✅ |
| `device.token.revoke()` | `DeviceManager::revoke_token()` | ✅ |
| `device.list()` | `DeviceManager::list_paired_devices()` | ✅ |
| `node.register()` | `NodeRegistry::register_node()` | ✅ |
| `node.invoke()` | `NodeRegistry::invoke()` | ✅ |
| `node.list()` | `NodeRegistry::list_nodes()` | ✅ |
| `node.describe()` | `NodeRegistry::describe()` | ✅ |
| `node.unregister()` | `NodeRegistry::unregister_node()` | ✅ |

## Data Flow Examples

### Example 1: Device Pairing and Node Registration

```
User initiates pairing
    ↓
DeviceManager::request_pairing(device_id, public_key, ...)
    ↓
Store in pending_requests_
    ↓
User approves
    ↓
DeviceManager::approve_pairing(request_id)
    ↓
Generate authentication token
    ↓
If role="node":
    ├─> SandboxManager::create_sandbox(config)
    └─> NodeRegistry::register_node(node)
    ↓
Return PairedDevice with token
```

### Example 2: Node Command Invocation

```
Client sends command
    ↓
NodeRegistry::invoke(node_id, command, params)
    ↓
Validate node exists and is connected
    ↓
Check command in node.commands
    ↓
Check permission in node.permissions[command]
    ↓
ToolManager::execute_tool_with_context(
    command, params, {node_id, ...}
)
    ↓
Tool executes in associated sandbox
    ↓
Return NodeInvokeResult
```

## Event System

Both `NodeRegistry` and `DeviceManager` support event callbacks:

```cpp
// Node events
node_registry->set_event_callback([](
    const std::string& node_id,
    const std::string& event_type,
    const std::string& event_data
) {
    // Handle: node.registered, node.unregistered, node.error
});

// Device events
device_mgr->set_event_callback([](
    const std::string& device_id,
    const std::string& event_type,
    const std::string& event_data
) {
    // Handle: pairing.requested, pairing.approved, token.rotated
});
```

## Security Considerations

1. **Device Authentication:**
   - ED25519 key pairs for device identity
   - SHA256-derived device IDs
   - Token-based authentication with expiration

2. **Permission System:**
   - Node-level command permissions
   - Token scopes for fine-grained access
   - Sandbox isolation for command execution

3. **Token Management:**
   - Automatic expiration (default 30 days)
   - Token rotation support
   - Token revocation on unpair

## Testing Strategy

### Unit Tests
- Node data structure methods
- DeviceIdentity crypto operations
- NodeRegistry registration/invocation logic
- DeviceManager pairing flow

### Integration Tests
- End-to-end pairing and node registration
- Command routing through NodeRegistry
- Token verification and rotation
- Sandbox creation for node devices

### Compatibility Tests
- Verify alignment with OpenClaw API
- Test message format compatibility
- Validate authentication protocol

## Next Steps

1. ✅ Implement core data structures (Node, DeviceIdentity)
2. ✅ Implement NodeRegistry
3. ✅ Implement DeviceManager
4. ⏳ Add ToolManager context support
5. ⏳ Add SandboxManager device filtering
6. ⏳ Write unit tests
7. ⏳ Add CMakeLists.txt entries
8. ⏳ Implement WebSocket transport layer
9. ⏳ Add APNS wake mechanism (iOS)
10. ⏳ Production JWT token generation

## Dependencies

- OpenSSL (for ED25519 crypto)
- Existing ToolManager (for command routing)
- Existing SandboxManager (for execution environment)
- WebSocket server (for node communication)

## Build Integration

Add to `CMakeLists.txt`:

```cmake
# Nodes library
add_library(hunterclaw_nodes
    src/nodes/node.cc
    src/nodes/node_registry.cc
)
target_include_directories(hunterclaw_nodes PUBLIC include)
target_link_libraries(hunterclaw_nodes hunterclaw_tools hunterclaw_sandbox)

# Device library
add_library(hunterclaw_device
    src/device/device_identity.cc
    src/device/device_manager.cc
)
target_include_directories(hunterclaw_device PUBLIC include)
target_link_libraries(hunterclaw_device 
    hunterclaw_nodes 
    hunterclaw_sandbox 
    OpenSSL::Crypto
)
```

## Summary

The Nodes and Device implementation provides:
- ✅ Strict alignment with OpenClaw architecture
- ✅ ED25519-based device authentication
- ✅ Node registry and command routing
- ✅ Device pairing and lifecycle management
- ✅ Event-driven architecture
- ✅ Security-first design
- ✅ Clean integration with existing Tool and Sandbox systems

See `NODES_DEVICE_EXAMPLES.md` for detailed usage examples.
