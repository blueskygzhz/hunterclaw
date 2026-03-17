#include "hunterclaw/device/device_manager.h"
#include "hunterclaw/sandbox/sandbox.h"  // ✅ SandboxManager 在 sandbox.h 中定义
#include "hunterclaw/nodes/node_registry.h"
#include "hunterclaw/nodes/node.h"
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

namespace hunterclaw {
namespace device {

DeviceManager::DeviceManager(std::shared_ptr<sandbox::SandboxManager> sandbox_manager,
                             std::shared_ptr<nodes::NodeRegistry> node_registry)
    : sandbox_manager_(sandbox_manager)
    , node_registry_(node_registry) {
}

core::Result<std::string> DeviceManager::request_pairing(
    const std::string& device_id,
    const std::string& public_key,
    const std::string& display_name,
    const std::string& platform,
    const std::string& role
) {
    ++total_pairing_requests_;
    
    // Validate inputs
    if (device_id.empty() || public_key.empty()) {
        return core::Result<std::string>::Err("Device ID and public key are required");
    }
    
    // Check if already paired
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (paired_devices_.count(device_id) > 0) {
            return core::Result<std::string>::Err("Device already paired: " + device_id);
        }
    }
    
    // Create pairing request
    PairingRequest request;
    request.request_id = generate_request_id();
    request.device_id = device_id;
    request.public_key = public_key;
    request.display_name = display_name;
    request.platform = platform;
    request.role = role;
    request.created_at_ms = current_time_ms();
    request.approved = false;
    
    auto validation = validate_pairing_request(request);
    if (!validation.is_ok()) {
        return core::Result<std::string>::Err(validation.error);
    }
    
    // Store request
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_requests_[request.request_id] = request;
    }
    
    // Emit event
    if (event_callback_) {
        event_callback_(device_id, "pairing.requested", "{}");
    }
    
    return core::Result<std::string>::Ok(request.request_id);
}

core::Result<PairedDevice> DeviceManager::approve_pairing(const std::string& request_id) {
    PairingRequest request;
    
    // Get request
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_requests_.find(request_id);
        if (it == pending_requests_.end()) {
            return core::Result<PairedDevice>::Err("Pairing request not found: " + request_id);
        }
        request = it->second;
        
        if (request.approved) {
            return core::Result<PairedDevice>::Err("Pairing request already approved");
        }
    }
    
    // Create paired device
    PairedDevice device;
    device.device_id = request.device_id;
    device.public_key = request.public_key;
    device.display_name = request.display_name;
    device.platform = request.platform;
    device.role = string_to_device_role(request.role);
    device.created_at_ms = request.created_at_ms;
    device.approved_at_ms = current_time_ms();
    device.last_seen_ms = device.approved_at_ms;
    
    // Generate default scopes
    if (device.role == DeviceRole::NODE) {
        device.scopes = {"node:execute", "node:report"};
    } else {
        device.scopes = {"device:control"};
    }
    
    // Generate authentication token
    auto token = generate_token(device.device_id, request.role, device.scopes);
    device.tokens[request.role] = token;
    
    // Create node resources if role is NODE
    if (device.role == DeviceRole::NODE) {
        auto node_result = create_node_resources(device);
        if (!node_result.is_ok()) {
            return core::Result<PairedDevice>::Err("Failed to create node resources: " + node_result.error);
        }
    }
    
    // Store paired device
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paired_devices_[device.device_id] = device;
        
        // Mark request as approved and remove from pending
        request.approved = true;
        request.approved_at_ms = device.approved_at_ms;
        pending_requests_.erase(request_id);
    }
    
    ++approved_pairings_;
    
    // Emit event
    if (event_callback_) {
        event_callback_(device.device_id, "pairing.approved", "{}");
    }
    
    return core::Result<PairedDevice>::Ok(device);
}

core::Result<void> DeviceManager::reject_pairing(const std::string& request_id,
                                                 const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = pending_requests_.find(request_id);
    if (it == pending_requests_.end()) {
        return core::Result<void>::Err("Pairing request not found: " + request_id);
    }
    
    std::string device_id = it->second.device_id;
    pending_requests_.erase(it);
    
    ++rejected_pairings_;
    
    // Emit event
    if (event_callback_) {
        event_callback_(device_id, "pairing.rejected", "{\"reason\":\"" + reason + "\"}");
    }
    
    return core::Result<void>::Ok();
}

std::vector<PairingRequest> DeviceManager::list_pending_requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<PairingRequest> result;
    result.reserve(pending_requests_.size());
    
    for (const auto& [id, request] : pending_requests_) {
        result.push_back(request);
    }
    
    return result;
}

std::optional<PairingRequest> DeviceManager::get_pairing_request(const std::string& request_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = pending_requests_.find(request_id);
    if (it == pending_requests_.end()) {
        return std::nullopt;
    }
    
    return it->second;
}

core::Result<bool> DeviceManager::verify_token(
    const std::string& device_id,
    const std::string& token,
    const std::string& role
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = paired_devices_.find(device_id);
    if (it == paired_devices_.end()) {
        return core::Result<bool>::Err("Device not paired: " + device_id);
    }
    
    auto& device = it->second;
    auto token_it = device.tokens.find(role);
    if (token_it == device.tokens.end()) {
        return core::Result<bool>::Err("Token not found for role: " + role);
    }
    
    auto& device_token = token_it->second;
    
    // Check expiration
    if (device_token.is_expired()) {
        return core::Result<bool>::Err("Token expired");
    }
    
    // Check token match
    if (device_token.token != token) {
        return core::Result<bool>::Err("Token mismatch");
    }
    
    // Update last seen
    device.last_seen_ms = current_time_ms();
    
    return core::Result<bool>::Ok(true);
}

core::Result<DeviceAuthToken> DeviceManager::rotate_token(
    const std::string& device_id,
    const std::string& role
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = paired_devices_.find(device_id);
    if (it == paired_devices_.end()) {
        return core::Result<DeviceAuthToken>::Err("Device not paired: " + device_id);
    }
    
    auto& device = it->second;
    
    // Generate new token
    auto new_token = generate_token(device_id, role, device.scopes);
    device.tokens[role] = new_token;
    
    // Emit event
    if (event_callback_) {
        event_callback_(device_id, "token.rotated", "{\"role\":\"" + role + "\"}");
    }
    
    return core::Result<DeviceAuthToken>::Ok(new_token);
}

core::Result<void> DeviceManager::revoke_token(
    const std::string& device_id,
    const std::string& role
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = paired_devices_.find(device_id);
    if (it == paired_devices_.end()) {
        return core::Result<void>::Err("Device not paired: " + device_id);
    }
    
    auto& device = it->second;
    device.tokens.erase(role);
    
    // Emit event
    if (event_callback_) {
        event_callback_(device_id, "token.revoked", "{\"role\":\"" + role + "\"}");
    }
    
    return core::Result<void>::Ok();
}

std::vector<PairedDevice> DeviceManager::list_paired_devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<PairedDevice> result;
    result.reserve(paired_devices_.size());
    
    for (const auto& [id, device] : paired_devices_) {
        result.push_back(device);
    }
    
    return result;
}

std::optional<PairedDevice> DeviceManager::get_paired_device(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = paired_devices_.find(device_id);
    if (it == paired_devices_.end()) {
        return std::nullopt;
    }
    
    return it->second;
}

std::vector<PairedDevice> DeviceManager::find_devices_by_role(DeviceRole role) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<PairedDevice> result;
    
    for (const auto& [id, device] : paired_devices_) {
        if (device.role == role) {
            result.push_back(device);
        }
    }
    
    return result;
}

std::vector<PairedDevice> DeviceManager::find_devices_by_platform(const std::string& platform) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<PairedDevice> result;
    
    for (const auto& [id, device] : paired_devices_) {
        if (device.platform == platform) {
            result.push_back(device);
        }
    }
    
    return result;
}

core::Result<void> DeviceManager::unpair_device(const std::string& device_id) {
    PairedDevice device;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = paired_devices_.find(device_id);
        if (it == paired_devices_.end()) {
            return core::Result<void>::Err("Device not paired: " + device_id);
        }
        
        device = it->second;
        paired_devices_.erase(it);
    }
    
    // Cleanup node resources if role is NODE
    if (device.role == DeviceRole::NODE) {
        auto cleanup_result = cleanup_node_resources(device);
        if (!cleanup_result.is_ok()) {
            // Log error but continue
        }
    }
    
    // Emit event
    if (event_callback_) {
        event_callback_(device_id, "device.unpaired", "{}");
    }
    
    return core::Result<void>::Ok();
}

void DeviceManager::update_heartbeat(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = paired_devices_.find(device_id);
    if (it != paired_devices_.end()) {
        it->second.last_seen_ms = current_time_ms();
    }
}

void DeviceManager::set_event_callback(DeviceEventCallback callback) {
    event_callback_ = callback;
}

void DeviceManager::emit_event(const std::string& device_id,
                               const std::string& event_type,
                               const std::string& event_data) {
    if (event_callback_) {
        event_callback_(device_id, event_type, event_data);
    }
}

int DeviceManager::cleanup_expired_requests(int64_t threshold_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    int removed = 0;
    auto current = current_time_ms();
    
    auto it = pending_requests_.begin();
    while (it != pending_requests_.end()) {
        if ((current - it->second.created_at_ms) > threshold_ms) {
            it = pending_requests_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    
    return removed;
}

std::string DeviceManager::get_statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::ostringstream oss;
    oss << "{";
    oss << "\"totalPairingRequests\":" << total_pairing_requests_.load() << ",";
    oss << "\"approvedPairings\":" << approved_pairings_.load() << ",";
    oss << "\"rejectedPairings\":" << rejected_pairings_.load() << ",";
    oss << "\"pairedDevices\":" << paired_devices_.size() << ",";
    oss << "\"pendingRequests\":" << pending_requests_.size();
    oss << "}";
    
    return oss.str();
}

core::Result<void> DeviceManager::validate_pairing_request(const PairingRequest& request) const {
    if (request.device_id.empty()) {
        return core::Result<void>::Err("Device ID cannot be empty");
    }
    if (request.public_key.empty()) {
        return core::Result<void>::Err("Public key cannot be empty");
    }
    if (request.platform.empty()) {
        return core::Result<void>::Err("Platform cannot be empty");
    }
    if (request.role != "node" && request.role != "operator") {
        return core::Result<void>::Err("Invalid role: " + request.role);
    }
    return core::Result<void>::Ok();
}

DeviceAuthToken DeviceManager::generate_token(
    const std::string& device_id,
    const std::string& role,
    const std::vector<std::string>& scopes
) {
    // Generate random token (simplified - should use JWT in production)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    std::ostringstream token_oss;
    token_oss << std::hex << std::setfill('0');
    for (int i = 0; i < 32; ++i) {
        token_oss << std::setw(2) << dis(gen);
    }
    
    DeviceAuthToken token;
    token.token = token_oss.str();
    token.role = role;
    token.scopes = scopes;
    token.issued_at_ms = current_time_ms();
    token.expires_at_ms = token.issued_at_ms + (30 * 24 * 60 * 60 * 1000LL); // 30 days
    
    return token;
}

core::Result<void> DeviceManager::create_node_resources(PairedDevice& device) {
    // Create sandbox for node
    if (sandbox_manager_) {
        sandbox::SandboxConfig config;
        config.metadata["device_id"] = device.device_id;
        config.metadata["platform"] = device.platform;
        config.metadata["display_name"] = device.display_name;
        
        auto sandbox_info = sandbox_manager_->create_sandbox(config);
        device.sandbox_id = sandbox_info.id;
    }
    
    // Register node if registry available
    if (node_registry_) {
        nodes::Node node;
        node.node_id = device.device_id;
        node.display_name = device.display_name;
        node.platform = device.platform;
        node.status = nodes::NodeStatus::CONNECTED;
        node.connected_at_ms = device.approved_at_ms;
        node.metadata["device_id"] = device.device_id;
        node.metadata["sandbox_id"] = device.sandbox_id;
        
        // Default commands (can be customized based on platform)
        node.commands = {"browser.start", "browser.snapshot", "browser.close"};
        node.caps = {"browser"};
        
        auto register_result = node_registry_->register_node(node);
        if (register_result.is_ok()) {
            device.node_id = node.node_id;
        }
    }
    
    return core::Result<void>::Ok();
}

core::Result<void> DeviceManager::cleanup_node_resources(const PairedDevice& device) {
    // Unregister node
    if (node_registry_ && !device.node_id.empty()) {
        node_registry_->unregister_node(device.node_id);
    }
    
    // Cleanup sandbox
    if (sandbox_manager_ && !device.sandbox_id.empty()) {
        sandbox_manager_->delete_sandbox(device.sandbox_id);
    }
    
    return core::Result<void>::Ok();
}

int64_t DeviceManager::current_time_ms() const {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

std::string DeviceManager::generate_request_id() const {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    std::ostringstream oss;
    oss << "req_" << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        oss << std::setw(2) << dis(gen);
    }
    
    return oss.str();
}

} // namespace device
} // namespace hunterclaw
