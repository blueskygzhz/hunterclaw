#pragma once
// ============================================================================
// model_auth.h
// 严格对齐 OpenClaw: model-auth.ts (737行)
//                   + model-auth-env.ts (68行)
//                   + model-auth-markers.ts (100行)
//                   + model-auth-env-vars.ts (~20行)
//                   + model-auth-runtime-shared.ts (~40行)
//                   + model-auth-label.ts (72行)
//                   + provider-auth-aliases.ts (~50行)
// ============================================================================

#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <set>

namespace hunterclaw {
namespace llm {

// ============================================================================
// Auth Markers (对齐 model-auth-markers.ts)
// ============================================================================

// 对齐 OC: known markers
extern const std::string OLLAMA_LOCAL_AUTH_MARKER;
extern const std::string CUSTOM_LOCAL_AUTH_MARKER;
extern const std::string GCP_VERTEX_CREDENTIALS_MARKER;
extern const std::string NON_ENV_SECRETREF_MARKER;
extern const std::string MINIMAX_OAUTH_MARKER;
extern const std::string OAUTH_API_KEY_MARKER_PREFIX;

/// 对齐: isNonSecretApiKeyMarker
bool is_non_secret_api_key_marker(const std::string& value);

/// 对齐: isKnownEnvApiKeyMarker
bool is_known_env_api_key_marker(const std::string& value);

/// 对齐: isOAuthApiKeyMarker
bool is_oauth_api_key_marker(const std::string& value);

/// 对齐: isAwsSdkAuthMarker
bool is_aws_sdk_auth_marker(const std::string& value);

// ============================================================================
// Provider Auth Aliases (对齐 provider-auth-aliases.ts)
// ============================================================================

/// 对齐: resolveProviderIdForAuth
std::string resolve_provider_id_for_auth(const std::string& provider);

// ============================================================================
// Env API Key (对齐 model-auth-env.ts + model-auth-env-vars.ts)
// ============================================================================

/// 对齐: EnvApiKeyResult
struct EnvApiKeyResult {
    std::string api_key;
    std::string source;
};

/// 对齐: resolveProviderEnvApiKeyCandidates — provider → env var names
std::map<std::string, std::vector<std::string>> resolve_provider_env_api_key_candidates();

/// 对齐: resolveEnvApiKey
std::optional<EnvApiKeyResult> resolve_env_api_key(const std::string& provider);

/// 对齐: listKnownProviderEnvApiKeyNames
std::set<std::string> list_known_provider_env_api_key_names();

// ============================================================================
// Resolved Provider Auth (对齐 model-auth-runtime-shared.ts)
// ============================================================================

enum class AuthMode {
    ApiKey,
    OAuth,
    Token,
    AwsSdk,
    Unknown
};

std::string auth_mode_to_string(AuthMode mode);

struct ResolvedProviderAuth {
    std::string api_key;
    std::optional<std::string> profile_id;
    std::string source;
    AuthMode mode = AuthMode::ApiKey;
};

/// 对齐: requireApiKey — throw if no API key available
std::string require_api_key(const ResolvedProviderAuth& auth);

/// 对齐: resolveAwsSdkEnvVarName
std::optional<std::string> resolve_aws_sdk_env_var_name(const std::string& provider);

// ============================================================================
// Model Auth Resolution (对齐 model-auth.ts core)
// ============================================================================

/// 对齐: getCustomProviderApiKey
std::optional<std::string> get_custom_provider_api_key(
    const core::Config& cfg,
    const std::string& provider);

/// 对齐: resolveUsableCustomProviderApiKey
struct ResolvedCustomProviderApiKey {
    std::string api_key;
    std::string source;
};

std::optional<ResolvedCustomProviderApiKey> resolve_usable_custom_provider_api_key(
    const core::Config& cfg,
    const std::string& provider);

/// 对齐: hasUsableCustomProviderApiKey
bool has_usable_custom_provider_api_key(
    const core::Config& cfg,
    const std::string& provider);

/// 对齐: resolveApiKeyForProvider (simplified sync version)
ResolvedProviderAuth resolve_api_key_for_provider(
    const std::string& provider,
    const core::Config& cfg,
    const std::string& profile_id = "",
    const std::string& preferred_profile = "");

/// 对齐: hasAvailableAuthForProvider
bool has_available_auth_for_provider(
    const std::string& provider,
    const core::Config& cfg);

// ============================================================================
// Model Auth Mode (对齐 model-auth.ts:537-596)
// ============================================================================

/// 对齐: ModelAuthMode
enum class ModelAuthMode {
    ApiKey,
    OAuth,
    Token,
    Mixed,
    AwsSdk,
    Unknown
};

std::string model_auth_mode_to_string(ModelAuthMode mode);

/// 对齐: resolveModelAuthMode
ModelAuthMode resolve_model_auth_mode(
    const std::string& provider,
    const core::Config& cfg);

// ============================================================================
// Auth Label (对齐 model-auth-label.ts)
// ============================================================================

/// 对齐: resolveModelAuthLabel
std::string resolve_model_auth_label(
    const std::string& provider,
    const core::Config& cfg);

} // namespace llm
} // namespace hunterclaw
