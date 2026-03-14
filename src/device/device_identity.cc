#include "hunterclaw/device/device_identity.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>

namespace hunterclaw {
namespace device {

// Helper: Convert bytes to hex string
static std::string bytes_to_hex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    }
    return oss.str();
}

DeviceIdentity DeviceIdentity::generate() {
    DeviceIdentity identity;
    
    // Generate ED25519 key pair using OpenSSL
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ctx) {
        return identity;
    }
    
    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen_init(ctx) <= 0 ||
        EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return identity;
    }
    
    // Extract public key
    BIO* pub_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(pub_bio, pkey);
    
    char* pub_data = nullptr;
    long pub_len = BIO_get_mem_data(pub_bio, &pub_data);
    identity.public_key_pem = std::string(pub_data, pub_len);
    BIO_free(pub_bio);
    
    // Extract private key
    BIO* priv_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(priv_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    
    char* priv_data = nullptr;
    long priv_len = BIO_get_mem_data(priv_bio, &priv_data);
    identity.private_key_pem = std::string(priv_data, priv_len);
    BIO_free(priv_bio);
    
    // Compute device_id
    identity.device_id = compute_device_id(identity.public_key_pem);
    
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    
    return identity;
}

std::string DeviceIdentity::compute_device_id(const std::string& public_key_pem) {
    // Compute SHA256 hash of public key
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(public_key_pem.data()),
           public_key_pem.size(),
           hash);
    
    return bytes_to_hex(hash, SHA256_DIGEST_LENGTH);
}

bool DeviceIdentity::verify_signature(const std::string& message,
                                      const std::vector<uint8_t>& signature) const {
    // Parse public key
    BIO* bio = BIO_new_mem_buf(public_key_pem.data(), public_key_pem.size());
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    
    if (!pkey) {
        return false;
    }
    
    // Verify signature
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool result = false;
    
    if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1) {
        if (EVP_DigestVerify(ctx,
                            signature.data(),
                            signature.size(),
                            reinterpret_cast<const unsigned char*>(message.data()),
                            message.size()) == 1) {
            result = true;
        }
    }
    
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    
    return result;
}

std::vector<uint8_t> DeviceIdentity::sign_message(const std::string& message) const {
    std::vector<uint8_t> signature;
    
    // Parse private key
    BIO* bio = BIO_new_mem_buf(private_key_pem.data(), private_key_pem.size());
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    
    if (!pkey) {
        return signature;
    }
    
    // Sign message
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    size_t sig_len = 0;
    
    if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1) {
        // Get signature length
        EVP_DigestSign(ctx,
                      nullptr,
                      &sig_len,
                      reinterpret_cast<const unsigned char*>(message.data()),
                      message.size());
        
        // Allocate and sign
        signature.resize(sig_len);
        EVP_DigestSign(ctx,
                      signature.data(),
                      &sig_len,
                      reinterpret_cast<const unsigned char*>(message.data()),
                      message.size());
        signature.resize(sig_len);
    }
    
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    
    return signature;
}

bool DeviceAuthToken::is_expired() const {
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return now_ms >= expires_at_ms;
}

bool DeviceAuthToken::has_scope(const std::string& scope) const {
    return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
}

bool PairedDevice::has_valid_token(const std::string& role_str) const {
    auto it = tokens.find(role_str);
    if (it == tokens.end()) {
        return false;
    }
    return !it->second.is_expired();
}

const DeviceAuthToken* PairedDevice::get_token(const std::string& role_str) const {
    auto it = tokens.find(role_str);
    if (it == tokens.end()) {
        return nullptr;
    }
    return &it->second;
}

std::string device_role_to_string(DeviceRole role) {
    switch (role) {
        case DeviceRole::NODE: return "node";
        case DeviceRole::OPERATOR: return "operator";
        default: return "unknown";
    }
}

DeviceRole string_to_device_role(const std::string& role_str) {
    if (role_str == "node") return DeviceRole::NODE;
    if (role_str == "operator") return DeviceRole::OPERATOR;
    return DeviceRole::UNKNOWN;
}

} // namespace device
} // namespace hunterclaw
