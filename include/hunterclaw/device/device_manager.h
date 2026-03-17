#pragma once

#include "hunterclaw/device/device_identity.h"
#include "hunterclaw/core/result.h"
#include <memory>
#include <functional>
#include <optional>
#include <mutex>
#include <atomic>

namespace hunterclaw {

// Forward declarations
namespace sandbox {
class SandboxManager;
}

namespace nodes {
class NodeRegistry;
}

namespace device {

/**
 * @brief Device event callback type
 */
using DeviceEventCallback = std::function<void(const std::string& device_id,
                                               const std::string& event_type,
                                               const std::string& event_data)>;

/**
 * @brief DeviceManager handles device pairing and authentication
 * 
 * This class aligns with OpenClaw's device management interface and provides:
 * - Device pairing request/approval flow
 * - Token generation and verification
 * - Integration with SandboxManager for node devices
 * - Integration with NodeRegistry for node registration
 */
class DeviceManager {
public:
    /**
     * @brief Constructor
     * @param sandbox_manager Shared SandboxManager for node devices
     * @param node_registry Shared NodeRegistry for node registration
     */
    DeviceManager(std::shared_ptr<sandbox::SandboxManager> sandbox_manager,
                  std::shared_ptr<nodes::NodeRegistry> node_registry = nullptr);
    
    ~DeviceManager() = default;
    
    // ============ Pairing Flow ============
    
    /**
     * @brief Request device pairing (aligned with OpenClaw device.pair.request)
     * 
     * @param device_id Device identifier (SHA256 of public key)
     * @param public_key Device public key in PEM format
     * @param display_name Human-readable device name
     * @param platform Platform (ios/android/macos)
     * @param role Requested role ("node" or "operator")
     * @return Result with request_id or error
     */
    core::Result<std::string> request_pairing(
        const std::string& device_id,
        const std::string& public_key,
        const std::string& display_name,
        const std::string& platform,
        const std::string& role
    );
    
    /**
     * @brief Approve device pairing (aligned with OpenClaw device.pair.approve)
     * 
     * This will:
     * 1. Validate the pairing request
     * 2. Generate authentication tokens
     * 3. If role=node, create associated sandbox and register node
     * 4. Return paired device information
     * 
     * @param request_id Pairing request identifier
     * @return Result with PairedDevice or error
     */
    core::Result<PairedDevice> approve_pairing(const std::string& request_id);
    
    /**
     * @brief Reject device pairing
     * @param request_id Pairing request identifier
     * @param reason Rejection reason
     * @return Result with error if failed
     */
    core::Result<void> reject_pairing(const std::string& request_id,
                                      const std::string& reason = "");
    
    /**
     * @brief List pending pairing requests
     * @return Vector of pending requests
     */
    std::vector<PairingRequest> list_pending_requests() const;
    
    /**
     * @brief Get pairing request by ID
     * @param request_id Request identifier
     * @return Optional request
     */
    std::optional<PairingRequest> get_pairing_request(const std::string& request_id) const;
    
    // ============ Token Management ============
    
    /**
     * @brief Verify device token (aligned with OpenClaw device.token.verify)
     * 
     * @param device_id Device identifier
     * @param token Authentication token
     * @param role Expected role ("node" or "operator")
     * @return Result with true if valid, error otherwise
     */
    core::Result<bool> verify_token(
        const std::string& device_id,
        const std::string& token,
        const std::string& role
    );
    
    /**
     * @brief Rotate device token (aligned with OpenClaw device.token.rotate)
     * 
     * @param device_id Device identifier
     * @param role Token role to rotate
     * @return Result with new token or error
     */
    core::Result<DeviceAuthToken> rotate_token(
        const std::string& device_id,
        const std::string& role
    );
    
    /**
     * @brief Revoke device token
     * @param device_id Device identifier
     * @param role Token role to revoke
     * @return Result with error if failed
     */
    core::Result<void> revoke_token(
        const std::string& device_id,
        const std::string& role
    );
    
    // ============ Device Query ============
    
    /**
     * @brief List all paired devices
     * @return Vector of paired devices
     */
    std::vector<PairedDevice> list_paired_devices() const;
    
    /**
     * @brief Get paired device by ID
     * @param device_id Device identifier
     * @return Optional paired device
     */
    std::optional<PairedDevice> get_paired_device(const std::string& device_id) const;
    
    /**
     * @brief Find devices by role
     * @param role Device role
     * @return Vector of matching devices
     */
    std::vector<PairedDevice> find_devices_by_role(DeviceRole role) const;
    
    /**
     * @brief Find devices by platform
     * @param platform Platform name
     * @return Vector of matching devices
     */
    std::vector<PairedDevice> find_devices_by_platform(const std::string& platform) const;
    
    // ============ Device Lifecycle ============
    
    /**
     * @brief Unpair a device
     * 
     * This will:
     * 1. Revoke all tokens
     * 2. If role=node, cleanup sandbox and unregister node
     * 3. Remove pairing record
     * 
     * @param device_id Device identifier
     * @return Result with error if failed
     */
    core::Result<void> unpair_device(const std::string& device_id);
    
    /**
     * @brief Update device heartbeat
     * @param device_id Device identifier
     */
    void update_heartbeat(const std::string& device_id);
    
    // ============ Event Handling ============
    
    /**
     * @brief Set event callback for device events
     * @param callback Event callback function
     */
    void set_event_callback(DeviceEventCallback callback);
    
    /**
     * @brief Emit a device event
     * @param device_id Device identifier
     * @param event_type Event type
     * @param event_data Event data (JSON string)
     */
    void emit_event(const std::string& device_id,
                    const std::string& event_type,
                    const std::string& event_data);
    
    // ============ Maintenance ============
    
    /**
     * @brief Cleanup expired pairing requests
     * @param threshold_ms Age threshold in milliseconds
     * @return Number of requests cleaned up
     */
    int cleanup_expired_requests(int64_t threshold_ms = 600000); // 10 minutes
    
    /**
     * @brief Get statistics
     * @return JSON string with manager statistics
     */
    std::string get_statistics() const;
    
private:
    // Internal helpers
    core::Result<void> validate_pairing_request(const PairingRequest& request) const;
    DeviceAuthToken generate_token(const std::string& device_id,
                                   const std::string& role,
                                   const std::vector<std::string>& scopes);
    core::Result<void> create_node_resources(PairedDevice& device);
    core::Result<void> cleanup_node_resources(const PairedDevice& device);
    int64_t current_time_ms() const;
    std::string generate_request_id() const;
    
    // Dependencies
    std::shared_ptr<sandbox::SandboxManager> sandbox_manager_;
    std::shared_ptr<nodes::NodeRegistry> node_registry_;
    
    // State
    mutable std::mutex mutex_;
    std::map<std::string, PairingRequest> pending_requests_;
    std::map<std::string, PairedDevice> paired_devices_;
    DeviceEventCallback event_callback_;
    
    // Statistics
    std::atomic<uint64_t> total_pairing_requests_{0};
    std::atomic<uint64_t> approved_pairings_{0};
    std::atomic<uint64_t> rejected_pairings_{0};
};

} // namespace device
} // namespace hunterclaw
