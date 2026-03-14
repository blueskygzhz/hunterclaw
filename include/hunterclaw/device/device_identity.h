#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace hunterclaw {
namespace device {

/**
 * @brief Device role types
 */
enum class DeviceRole {
    NODE,           // Device acts as a node (executes commands)
    OPERATOR,       // Device acts as an operator (issues commands)
    UNKNOWN
};

/**
 * @brief Device identity (aligned with OpenClaw device identity)
 * 
 * Uses ED25519 key pairs for authentication.
 * device_id is derived as SHA256(public_key)
 */
struct DeviceIdentity {
    std::string device_id;          // SHA256(public_key)
    std::string public_key_pem;     // ED25519 public key in PEM format
    std::string private_key_pem;    // ED25519 private key in PEM format (optional)
    
    /**
     * @brief Generate a new device identity with ED25519 key pair
     * @return New DeviceIdentity
     */
    static DeviceIdentity generate();
    
    /**
     * @brief Compute device_id from public key
     * @param public_key_pem PEM format public key
     * @return Device ID (SHA256 hash)
     */
    static std::string compute_device_id(const std::string& public_key_pem);
    
    /**
     * @brief Verify signature using public key
     * @param message Message that was signed
     * @param signature Signature bytes
     * @return true if signature is valid
     */
    bool verify_signature(const std::string& message, 
                         const std::vector<uint8_t>& signature) const;
    
    /**
     * @brief Sign message using private key
     * @param message Message to sign
     * @return Signature bytes
     */
    std::vector<uint8_t> sign_message(const std::string& message) const;
};

/**
 * @brief Device authentication token
 */
struct DeviceAuthToken {
    std::string token;              // JWT or opaque token
    std::string role;               // "node" or "operator"
    std::vector<std::string> scopes; // Permission scopes
    int64_t issued_at_ms = 0;
    int64_t expires_at_ms = 0;
    
    /**
     * @brief Check if token is expired
     */
    bool is_expired() const;
    
    /**
     * @brief Check if token has scope
     */
    bool has_scope(const std::string& scope) const;
};

/**
 * @brief Pairing request (aligned with OpenClaw device.pair.request)
 */
struct PairingRequest {
    std::string request_id;         // Unique request identifier
    std::string device_id;          // Device requesting pairing
    std::string public_key;         // Device public key
    std::string display_name;       // Human-readable name
    std::string platform;           // Platform (ios/android/macos)
    std::string role;               // Requested role
    int64_t created_at_ms = 0;
    bool approved = false;
    int64_t approved_at_ms = 0;
    
    // Metadata
    std::map<std::string, std::string> metadata;
};

/**
 * @brief Paired device (aligned with OpenClaw paired device)
 */
struct PairedDevice {
    std::string device_id;
    std::string public_key;
    std::string display_name;
    std::string platform;
    DeviceRole role = DeviceRole::UNKNOWN;
    std::vector<std::string> scopes;
    std::map<std::string, DeviceAuthToken> tokens; // role -> token
    int64_t created_at_ms = 0;
    int64_t approved_at_ms = 0;
    int64_t last_seen_ms = 0;
    
    // Associated resources
    std::string sandbox_id;         // If role=NODE, associated sandbox ID
    std::string node_id;            // If role=NODE, associated node ID
    
    // Metadata
    std::map<std::string, std::string> metadata;
    
    /**
     * @brief Check if device has valid token for role
     */
    bool has_valid_token(const std::string& role_str) const;
    
    /**
     * @brief Get token for role
     */
    const DeviceAuthToken* get_token(const std::string& role_str) const;
};

/**
 * @brief Convert DeviceRole to string
 */
std::string device_role_to_string(DeviceRole role);

/**
 * @brief Convert string to DeviceRole
 */
DeviceRole string_to_device_role(const std::string& role_str);

} // namespace device
} // namespace hunterclaw
