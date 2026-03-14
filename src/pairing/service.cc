// HunterClaw - Pairing System Implementation
// 严格对齐 OpenClaw pairing/

#include "hunterclaw/pairing/service.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <unordered_set>

using json = nlohmann::json;

namespace hunterclaw {
namespace pairing {

namespace {

std::string generate_timestamp() {
  auto now = std::chrono::system_clock::now();
  auto time_t = std::chrono::system_clock::to_time_t(now);
  std::stringstream ss;
  ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

int64_t parse_timestamp(const std::string& timestamp) {
  std::tm tm = {};
  std::istringstream ss(timestamp);
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  if (ss.fail()) return 0;
  return std::mktime(&tm) * 1000;
}

bool is_expired(const PairingRequest& request, int64_t now_ms) {
  int64_t created_at = parse_timestamp(request.created_at);
  if (created_at == 0) return true;
  return now_ms - created_at > PAIRING_PENDING_TTL_MS;
}

std::string to_lower(const std::string& str) {
  std::string result = str;
  std::transform(result.begin(), result.end(), result.begin(), ::tolower);
  return result;
}

std::string safe_channel_key(const std::string& channel) {
  std::string safe = to_lower(channel);
  std::replace_if(
      safe.begin(), safe.end(), [](char c) { return c == '/' || c == ':' || c == '*'; }, '_');
  if (safe.empty()) throw std::runtime_error("invalid pairing channel");
  return safe;
}

}  // namespace

// ============================================================================
// Setup Code Generation
// ============================================================================

std::string generate_setup_code() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, strlen(PAIRING_CODE_ALPHABET) - 1);

  std::string code;
  code.reserve(PAIRING_CODE_LENGTH);
  for (size_t i = 0; i < PAIRING_CODE_LENGTH; ++i) {
    code += PAIRING_CODE_ALPHABET[dis(gen)];
  }
  return code;
}

std::string generate_unique_pairing_code(const std::vector<std::string>& existing_codes) {
  std::unordered_set<std::string> existing_set(existing_codes.begin(), existing_codes.end());

  for (int attempt = 0; attempt < 500; ++attempt) {
    std::string code = generate_setup_code();
    if (existing_set.find(code) == existing_set.end()) {
      return code;
    }
  }

  throw std::runtime_error("failed to generate unique pairing code");
}

// ============================================================================
// Pairing Challenge
// ============================================================================

PairingChallengeResult issue_pairing_challenge(const PairingChallengeParams& params) {
  // Upsert pairing request
  auto [code, created] = params.upsert_pairing_request(params.sender_id, params.meta);

  if (!created) {
    return {false, std::nullopt};
  }

  // Notify callback
  if (params.on_created) {
    params.on_created(code);
  }

  // Build reply text
  std::string reply_text;
  if (params.build_reply_text) {
    reply_text = params.build_reply_text(code, params.sender_id_line);
  } else {
    reply_text = build_pairing_reply({params.channel, params.sender_id_line, code});
  }

  // Send reply
  try {
    params.send_pairing_reply(reply_text);
  } catch (const std::exception& e) {
    if (params.on_reply_error) {
      params.on_reply_error(e.what());
    }
  }

  return {true, code};
}

// ============================================================================
// Pairing Messages
// ============================================================================

std::string build_pairing_reply(const BuildPairingReplyParams& params) {
  std::ostringstream oss;
  oss << "HunterClaw: access not configured.\n"
      << "\n"
      << params.id_line << "\n"
      << "\n"
      << "Pairing code: " << params.code << "\n"
      << "\n"
      << "Ask the bot owner to approve with:\n"
      << "hunterclaw pairing approve " << params.channel << " " << params.code;
  return oss.str();
}

// ============================================================================
// Pairing Setup Resolution
// ============================================================================

PairingSetupResolution resolve_pairing_setup(const ResolvePairingSetupOptions& options) {
  PairingSetupResolution result;
  result.ok = false;

  // Try to resolve URL
  std::string url;
  if (options.public_url) {
    url = *options.public_url;
    result.url_source = "public_url";
  } else if (options.env.count("OPENCLAW_GATEWAY_URL")) {
    url = options.env.at("OPENCLAW_GATEWAY_URL");
    result.url_source = "env.OPENCLAW_GATEWAY_URL";
  } else {
    url = "ws://localhost:3000";
    result.url_source = "default";
  }

  // Force secure if needed
  if (options.force_secure && url.compare(0, 5, "ws://") == 0) {
    url.replace(0, 5, "wss://");
  }

  // Try to resolve auth
  std::optional<std::string> token;
  std::optional<std::string> password;
  PairingAuthLabel auth_label = PairingAuthLabel::TOKEN;

  if (options.env.count("OPENCLAW_GATEWAY_TOKEN")) {
    token = options.env.at("OPENCLAW_GATEWAY_TOKEN");
    auth_label = PairingAuthLabel::TOKEN;
  } else if (options.env.count("OPENCLAW_GATEWAY_PASSWORD")) {
    password = options.env.at("OPENCLAW_GATEWAY_PASSWORD");
    auth_label = PairingAuthLabel::PASSWORD;
  } else {
    result.ok = false;
    result.error = "no auth configured";
    return result;
  }

  result.ok = true;
  result.payload = PairingSetupPayload{url, token, password};
  result.auth_label = auth_label;
  return result;
}

// ============================================================================
// Pairing Store Implementation
// ============================================================================

struct PairingStore::Impl {
  std::string channel;
  std::vector<PairingRequest> requests;
  std::unordered_map<std::string, std::vector<std::string>> allow_from_by_account;
  std::string store_path;
};

PairingStore::PairingStore() : impl_(std::make_unique<Impl>()) {}

PairingStore::PairingStore(const std::string& channel) : impl_(std::make_unique<Impl>()) {
  impl_->channel = channel;
  impl_->store_path = "/tmp/hunterclaw_pairing_" + safe_channel_key(channel) + ".json";
  load();
}

PairingStore::~PairingStore() = default;

std::pair<std::string, bool> PairingStore::upsert_request(
    const std::string& id, const std::unordered_map<std::string, std::string>& meta) {
  // Check if request already exists
  for (auto& req : impl_->requests) {
    if (req.id == id) {
      req.last_seen_at = generate_timestamp();
      save();
      return {req.code, false};
    }
  }

  // Collect existing codes
  std::vector<std::string> existing_codes;
  existing_codes.reserve(impl_->requests.size());
  for (const auto& req : impl_->requests) {
    existing_codes.push_back(req.code);
  }

  // Generate new code
  std::string code = generate_unique_pairing_code(existing_codes);

  // Create new request
  PairingRequest req;
  req.id = id;
  req.code = code;
  req.created_at = generate_timestamp();
  req.last_seen_at = req.created_at;
  req.meta = meta;

  impl_->requests.push_back(req);

  // Prune if too many
  if (impl_->requests.size() > PAIRING_PENDING_MAX) {
    // Sort by last_seen_at
    std::sort(impl_->requests.begin(), impl_->requests.end(),
              [](const PairingRequest& a, const PairingRequest& b) {
                return parse_timestamp(a.last_seen_at) < parse_timestamp(b.last_seen_at);
              });
    // Keep only the most recent
    impl_->requests.erase(impl_->requests.begin(),
                          impl_->requests.end() - PAIRING_PENDING_MAX);
  }

  save();
  return {code, true};
}

std::optional<PairingRequest> PairingStore::get_request(const std::string& code) const {
  for (const auto& req : impl_->requests) {
    if (req.code == code) {
      return req;
    }
  }
  return std::nullopt;
}

std::vector<PairingRequest> PairingStore::list_requests() const { return impl_->requests; }

bool PairingStore::remove_request(const std::string& code) {
  auto it = std::remove_if(impl_->requests.begin(), impl_->requests.end(),
                           [&code](const PairingRequest& req) { return req.code == code; });

  if (it != impl_->requests.end()) {
    impl_->requests.erase(it, impl_->requests.end());
    save();
    return true;
  }
  return false;
}

void PairingStore::prune_expired_requests() {
  int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  auto it = std::remove_if(impl_->requests.begin(), impl_->requests.end(),
                           [now_ms](const PairingRequest& req) { return is_expired(req, now_ms); });

  if (it != impl_->requests.end()) {
    impl_->requests.erase(it, impl_->requests.end());
    save();
  }
}

void PairingStore::add_allow_from(const std::string& entry) {
  add_allow_from("default", entry);
}

void PairingStore::remove_allow_from(const std::string& entry) {
  remove_allow_from("default", entry);
}

std::vector<std::string> PairingStore::list_allow_from() const {
  return list_allow_from("default");
}

bool PairingStore::is_allowed(const std::string& entry) const {
  return is_allowed("default", entry);
}

void PairingStore::clear_allow_from() { impl_->allow_from_by_account.clear(); }

void PairingStore::add_allow_from(const std::string& account_id, const std::string& entry) {
  auto& list = impl_->allow_from_by_account[account_id];
  if (std::find(list.begin(), list.end(), entry) == list.end()) {
    list.push_back(entry);
    save();
  }
}

void PairingStore::remove_allow_from(const std::string& account_id, const std::string& entry) {
  auto& list = impl_->allow_from_by_account[account_id];
  auto it = std::remove(list.begin(), list.end(), entry);
  if (it != list.end()) {
    list.erase(it, list.end());
    save();
  }
}

std::vector<std::string> PairingStore::list_allow_from(const std::string& account_id) const {
  auto it = impl_->allow_from_by_account.find(account_id);
  return it != impl_->allow_from_by_account.end() ? it->second : std::vector<std::string>();
}

bool PairingStore::is_allowed(const std::string& account_id, const std::string& entry) const {
  auto list = list_allow_from(account_id);
  return std::find(list.begin(), list.end(), entry) != list.end();
}

void PairingStore::save() {
  try {
    json j;
    j["version"] = 1;
    j["channel"] = impl_->channel;

    // Save requests
    json requests_array = json::array();
    for (const auto& req : impl_->requests) {
      json req_obj;
      req_obj["id"] = req.id;
      req_obj["code"] = req.code;
      req_obj["createdAt"] = req.created_at;
      req_obj["lastSeenAt"] = req.last_seen_at;
      req_obj["meta"] = req.meta;
      requests_array.push_back(req_obj);
    }
    j["requests"] = requests_array;

    // Save allow_from
    json allow_from_obj = json::object();
    for (const auto& [account_id, entries] : impl_->allow_from_by_account) {
      allow_from_obj[account_id] = entries;
    }
    j["allowFrom"] = allow_from_obj;

    // Write to file
    std::ofstream file(impl_->store_path);
    file << j.dump(2);
  } catch (const std::exception& e) {
    // Ignore save errors
  }
}

void PairingStore::load() {
  try {
    std::ifstream file(impl_->store_path);
    if (!file.is_open()) return;

    json j;
    file >> j;

    if (j.contains("channel")) {
      impl_->channel = j["channel"].get<std::string>();
    }

    if (j.contains("requests") && j["requests"].is_array()) {
      impl_->requests.clear();
      for (const auto& req_obj : j["requests"]) {
        PairingRequest req;
        req.id = req_obj.value("id", "");
        req.code = req_obj.value("code", "");
        req.created_at = req_obj.value("createdAt", "");
        req.last_seen_at = req_obj.value("lastSeenAt", "");
        if (req_obj.contains("meta")) {
          req.meta = req_obj["meta"].get<std::unordered_map<std::string, std::string>>();
        }
        impl_->requests.push_back(req);
      }
    }

    if (j.contains("allowFrom") && j["allowFrom"].is_object()) {
      impl_->allow_from_by_account.clear();
      for (auto& [account_id, entries] : j["allowFrom"].items()) {
        impl_->allow_from_by_account[account_id] = entries.get<std::vector<std::string>>();
      }
    }
  } catch (const std::exception& e) {
    // Ignore load errors
  }
}

std::string PairingStore::get_channel() const { return impl_->channel; }

void PairingStore::set_channel(const std::string& channel) {
  impl_->channel = channel;
  impl_->store_path = "/tmp/hunterclaw_pairing_" + safe_channel_key(channel) + ".json";
  load();
}

// ============================================================================
// Default Pairing Adapter
// ============================================================================

struct DefaultPairingAdapter::Impl {
  PairingStore store;
};

DefaultPairingAdapter::DefaultPairingAdapter(const std::string& channel)
    : impl_(std::make_unique<Impl>()) {
  impl_->store.set_channel(channel);
}

DefaultPairingAdapter::~DefaultPairingAdapter() = default;

PairingChallengeResult DefaultPairingAdapter::issue_challenge(
    const PairingChallengeParams& params) {
  return issue_pairing_challenge(params);
}

bool DefaultPairingAdapter::is_allowed(const std::string& sender_id) const {
  return impl_->store.is_allowed(sender_id);
}

void DefaultPairingAdapter::add_allowed(const std::string& sender_id) {
  impl_->store.add_allow_from(sender_id);
}

void DefaultPairingAdapter::remove_allowed(const std::string& sender_id) {
  impl_->store.remove_allow_from(sender_id);
}

PairingSetupResolution DefaultPairingAdapter::resolve_setup(
    const ResolvePairingSetupOptions& options) const {
  return resolve_pairing_setup(options);
}

// ============================================================================
// Pairing Policy
// ============================================================================

std::string pairing_policy_to_string(PairingPolicy policy) {
  switch (policy) {
    case PairingPolicy::OPEN:
      return "open";
    case PairingPolicy::CHALLENGE:
      return "challenge";
    case PairingPolicy::CLOSED:
      return "closed";
    default:
      return "unknown";
  }
}

PairingPolicy parse_pairing_policy(const std::string& str) {
  std::string lower = to_lower(str);
  if (lower == "open") return PairingPolicy::OPEN;
  if (lower == "challenge") return PairingPolicy::CHALLENGE;
  if (lower == "closed") return PairingPolicy::CLOSED;
  return PairingPolicy::CHALLENGE;  // Default
}

}  // namespace pairing
}  // namespace hunterclaw
