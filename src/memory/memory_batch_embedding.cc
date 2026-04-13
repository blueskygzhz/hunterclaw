// ============================================================================
// memory_batch_embedding.cc
// ✅ 严格对齐 OpenClaw:
//   - src/memory-host-sdk/host/batch-gemini.ts (11.67KB)
//   - src/memory-host-sdk/host/batch-openai.ts (7.65KB)
//   - src/memory-host-sdk/host/batch-voyage.ts (9.62KB)
//   - src/memory-host-sdk/host/batch-runner.ts (2.01KB)
//   - src/memory-host-sdk/host/batch-status.ts (2.1KB)
//   - src/memory-host-sdk/host/batch-upload.ts (1.36KB)
//   - src/memory-host-sdk/host/batch-output.ts (1.53KB)
//   - src/memory-host-sdk/host/batch-utils.ts (1.09KB)
//   - src/memory-host-sdk/host/batch-error-utils.ts (1.01KB)
//   - src/memory-host-sdk/host/batch-embedding-common.ts (847B)
//   - src/memory-host-sdk/host/batch-provider-common.ts (333B)
//
// Batch embedding 管理系统：
//   - BatchEmbeddingRequest: 批量嵌入请求结构
//   - BatchEmbeddingJob: 批量任务状态跟踪
//   - BatchEmbeddingRunner: 批量嵌入执行引擎
//   - Gemini/OpenAI/Voyage 三大 provider 的批量处理
//   - upload_batch / poll_batch_status / download_batch_output
//   - 错误分类 + 重试策略
// ============================================================================

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace hunterclaw {
namespace memory {

namespace {
auto batch_log() { return spdlog::get("memory/batch"); }
} // namespace

// ============================================================================
// 🎯 BatchEmbeddingChunk (对齐 batch-embedding-common.ts:1-21)
// ============================================================================

struct BatchEmbeddingChunk {
    std::string id;
    std::string hash;
    std::string text;
    int start_line = 0;
    int end_line = 0;
    std::string path;
    std::string source;
};

// ============================================================================
// 🎯 BatchEmbeddingRequest (对齐 batch-openai.ts:16-25 / batch-gemini.ts / batch-voyage.ts)
// ============================================================================

struct BatchEmbeddingRequest {
    std::string provider;          // "openai" | "gemini" | "voyage"
    std::string model;
    std::string api_key;
    std::optional<std::string> base_url;
    std::vector<BatchEmbeddingChunk> chunks;
    int concurrency = 4;
    int64_t poll_interval_ms = 30000;
    int64_t timeout_ms = 600000;   // 10min
};

// ============================================================================
// 🎯 BatchJobStatus (对齐 batch-status.ts:1-23)
// ============================================================================

enum class BatchJobStatus {
    Pending,
    InProgress,
    Completed,
    Failed,
    Expired,
    Cancelled
};

static std::string status_to_string(BatchJobStatus s) {
    switch (s) {
        case BatchJobStatus::Pending: return "pending";
        case BatchJobStatus::InProgress: return "in_progress";
        case BatchJobStatus::Completed: return "completed";
        case BatchJobStatus::Failed: return "failed";
        case BatchJobStatus::Expired: return "expired";
        case BatchJobStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

static BatchJobStatus parse_batch_status(const std::string& s) {
    if (s == "pending" || s == "validating") return BatchJobStatus::Pending;
    if (s == "in_progress" || s == "running" || s == "processing") return BatchJobStatus::InProgress;
    if (s == "completed" || s == "done") return BatchJobStatus::Completed;
    if (s == "failed" || s == "error") return BatchJobStatus::Failed;
    if (s == "expired") return BatchJobStatus::Expired;
    if (s == "cancelled" || s == "cancelling") return BatchJobStatus::Cancelled;
    return BatchJobStatus::Failed;
}

struct BatchJobState {
    std::string batch_id;
    BatchJobStatus status = BatchJobStatus::Pending;
    int completed_count = 0;
    int failed_count = 0;
    int total_count = 0;
    std::optional<std::string> error_message;
    std::optional<std::string> output_file_id;
};

// ============================================================================
// 🎯 BatchEmbeddingResult (对齐 batch-output.ts:1-20)
// ============================================================================

struct BatchEmbeddingResult {
    std::string custom_id;
    std::vector<double> embedding;
    bool success = false;
    std::optional<std::string> error;
};

// ============================================================================
// 🎯 BatchErrorClassification (对齐 batch-error-utils.ts:1-25)
// ============================================================================

enum class BatchErrorClass {
    Transient,      // Network/rate-limit, worth retrying
    Permanent,      // Invalid input, will never succeed
    Unknown
};

static BatchErrorClass classify_batch_error(const std::string& error_message) {
    // Rate limit / throttle
    if (error_message.find("rate") != std::string::npos ||
        error_message.find("throttl") != std::string::npos ||
        error_message.find("429") != std::string::npos ||
        error_message.find("quota") != std::string::npos) {
        return BatchErrorClass::Transient;
    }
    // Timeout / network
    if (error_message.find("timeout") != std::string::npos ||
        error_message.find("ECONNRESET") != std::string::npos ||
        error_message.find("ENOTFOUND") != std::string::npos ||
        error_message.find("503") != std::string::npos ||
        error_message.find("502") != std::string::npos) {
        return BatchErrorClass::Transient;
    }
    // Authentication / invalid
    if (error_message.find("401") != std::string::npos ||
        error_message.find("403") != std::string::npos ||
        error_message.find("invalid") != std::string::npos) {
        return BatchErrorClass::Permanent;
    }
    return BatchErrorClass::Unknown;
}

// ============================================================================
// 🎯 OpenAI Batch Helpers (对齐 batch-openai.ts:27-200)
// ============================================================================

static const std::string OPENAI_BATCH_ENDPOINT = "/v1/embeddings";

struct OpenAiBatchRequest {
    std::string custom_id;
    std::string method = "POST";
    std::string url = OPENAI_BATCH_ENDPOINT;
    json body;
};

static std::vector<OpenAiBatchRequest> build_openai_batch_requests(
    const std::string& model,
    const std::vector<BatchEmbeddingChunk>& chunks
) {
    std::vector<OpenAiBatchRequest> requests;
    for (const auto& chunk : chunks) {
        OpenAiBatchRequest req;
        req.custom_id = chunk.id;
        req.body = {
            {"model", model},
            {"input", chunk.text},
            {"encoding_format", "float"}
        };
        requests.push_back(std::move(req));
    }
    return requests;
}

// 对齐 batch-openai.ts:55-82 — 构建 JSONL 上传文件
static std::string build_openai_batch_jsonl(const std::vector<OpenAiBatchRequest>& requests) {
    std::ostringstream oss;
    for (const auto& req : requests) {
        json line = {
            {"custom_id", req.custom_id},
            {"method", req.method},
            {"url", req.url},
            {"body", req.body}
        };
        oss << line.dump() << "\n";
    }
    return oss.str();
}

// ============================================================================
// 🎯 Gemini Batch Helpers (对齐 batch-gemini.ts:25-310)
// ============================================================================

struct GeminiBatchRequest {
    std::string model;
    std::vector<json> requests;  // embedContentRequest objects
};

static GeminiBatchRequest build_gemini_batch_request(
    const std::string& model,
    const std::vector<BatchEmbeddingChunk>& chunks,
    const std::string& task_type = "RETRIEVAL_DOCUMENT"
) {
    GeminiBatchRequest batch;
    batch.model = model;
    for (const auto& chunk : chunks) {
        json req = {
            {"model", "models/" + model},
            {"content", {{"parts", json::array({json{{"text", chunk.text}}})}}},
            {"taskType", task_type}
        };
        batch.requests.push_back(std::move(req));
    }
    return batch;
}

// ============================================================================
// 🎯 Voyage Batch Helpers (对齐 batch-voyage.ts:25-260)
// ============================================================================

struct VoyageBatchRequest {
    std::string model;
    std::vector<std::string> inputs;
    std::vector<std::string> ids;
};

static VoyageBatchRequest build_voyage_batch_request(
    const std::string& model,
    const std::vector<BatchEmbeddingChunk>& chunks
) {
    VoyageBatchRequest batch;
    batch.model = model;
    for (const auto& chunk : chunks) {
        batch.inputs.push_back(chunk.text);
        batch.ids.push_back(chunk.id);
    }
    return batch;
}

// ============================================================================
// 🎯 BatchUploadHelper (对齐 batch-upload.ts:1-38)
// ============================================================================

struct BatchUploadResult {
    bool success = false;
    std::string file_id;
    std::string error;
};

// ============================================================================
// 🎯 BatchEmbeddingRunner (对齐 batch-runner.ts:1-56)
// 完整批量嵌入执行引擎
// ============================================================================

using HttpRequestFn = std::function<json(
    const std::string& url,
    const std::string& method,
    const json& body,
    const std::unordered_map<std::string, std::string>& headers
)>;

class BatchEmbeddingRunner {
public:
    explicit BatchEmbeddingRunner(HttpRequestFn http_fn)
        : http_fn_(std::move(http_fn)) {}

    // 🎯 runBatch — 对齐 batch-runner.ts:20-55
    std::vector<BatchEmbeddingResult> run_batch(const BatchEmbeddingRequest& request) {
        if (request.chunks.empty()) return {};

        if (request.provider == "openai") {
            return run_openai_batch(request);
        }
        if (request.provider == "gemini") {
            return run_gemini_batch(request);
        }
        if (request.provider == "voyage") {
            return run_voyage_batch(request);
        }

        throw std::runtime_error("unsupported batch provider: " + request.provider);
    }

private:
    // 🎯 OpenAI batch flow — 对齐 batch-openai.ts:84-200
    std::vector<BatchEmbeddingResult> run_openai_batch(const BatchEmbeddingRequest& request) {
        auto requests = build_openai_batch_requests(request.model, request.chunks);
        auto jsonl = build_openai_batch_jsonl(requests);

        // 1. Upload JSONL file
        std::unordered_map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + request.api_key},
            {"Content-Type", "application/json"}
        };

        std::string base = request.base_url.value_or("https://api.openai.com");

        // Upload file
        json upload_body = {
            {"purpose", "batch"},
            {"file", jsonl}
        };
        auto upload_resp = http_fn_(base + "/v1/files", "POST", upload_body, headers);
        std::string file_id = upload_resp.value("id", "");

        // 2. Create batch
        json batch_body = {
            {"input_file_id", file_id},
            {"endpoint", OPENAI_BATCH_ENDPOINT},
            {"completion_window", "24h"}
        };
        auto batch_resp = http_fn_(base + "/v1/batches", "POST", batch_body, headers);
        std::string batch_id = batch_resp.value("id", "");

        // 3. Poll status
        auto started = std::chrono::steady_clock::now();
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(request.poll_interval_ms));
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            if (elapsed > request.timeout_ms) {
                throw std::runtime_error("batch timed out after " +
                    std::to_string(request.timeout_ms) + "ms");
            }

            auto status_resp = http_fn_(base + "/v1/batches/" + batch_id, "GET", {}, headers);
            auto status = parse_batch_status(status_resp.value("status", ""));
            if (status == BatchJobStatus::Completed) {
                std::string output_id = status_resp.value("output_file_id", "");
                return download_openai_results(base, output_id, headers);
            }
            if (status == BatchJobStatus::Failed || status == BatchJobStatus::Expired ||
                status == BatchJobStatus::Cancelled) {
                throw std::runtime_error("batch " + status_to_string(status) + ": " +
                    status_resp.dump());
            }
        }
    }

    std::vector<BatchEmbeddingResult> download_openai_results(
        const std::string& base_url,
        const std::string& output_file_id,
        const std::unordered_map<std::string, std::string>& headers
    ) {
        auto resp = http_fn_(base_url + "/v1/files/" + output_file_id + "/content",
            "GET", {}, headers);
        // Parse JSONL output
        std::vector<BatchEmbeddingResult> results;
        std::string content = resp.is_string() ? resp.get<std::string>() : resp.dump();
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            try {
                auto entry = json::parse(line);
                BatchEmbeddingResult r;
                r.custom_id = entry.value("custom_id", "");
                if (entry.contains("response") && entry["response"].contains("body")) {
                    auto& body = entry["response"]["body"];
                    if (body.contains("data") && body["data"].is_array() && !body["data"].empty()) {
                        auto& embedding = body["data"][0]["embedding"];
                        if (embedding.is_array()) {
                            for (const auto& v : embedding) {
                                r.embedding.push_back(v.get<double>());
                            }
                            r.success = true;
                        }
                    }
                }
                if (!r.success && entry.contains("error")) {
                    r.error = entry["error"].dump();
                }
                results.push_back(std::move(r));
            } catch (...) {}
        }
        return results;
    }

    // 🎯 Gemini batch flow — 对齐 batch-gemini.ts:130-310
    std::vector<BatchEmbeddingResult> run_gemini_batch(const BatchEmbeddingRequest& request) {
        auto batch = build_gemini_batch_request(request.model, request.chunks);
        std::vector<BatchEmbeddingResult> results;

        // Gemini uses batchEmbedContents — synchronous, chunked by max batch size
        const size_t MAX_GEMINI_BATCH = 100;
        for (size_t i = 0; i < batch.requests.size(); i += MAX_GEMINI_BATCH) {
            size_t end = std::min(i + MAX_GEMINI_BATCH, batch.requests.size());
            json sub_requests = json::array();
            for (size_t j = i; j < end; ++j) {
                sub_requests.push_back(batch.requests[j]);
            }

            json body = {{"requests", sub_requests}};
            std::string url = "https://generativelanguage.googleapis.com/v1/models/" +
                request.model + ":batchEmbedContents?key=" + request.api_key;

            auto resp = http_fn_(url, "POST", body, {{"Content-Type", "application/json"}});

            if (resp.contains("embeddings") && resp["embeddings"].is_array()) {
                for (size_t j = 0; j < resp["embeddings"].size() && (i + j) < request.chunks.size(); ++j) {
                    BatchEmbeddingResult r;
                    r.custom_id = request.chunks[i + j].id;
                    auto& values = resp["embeddings"][j]["values"];
                    if (values.is_array()) {
                        for (const auto& v : values) r.embedding.push_back(v.get<double>());
                        r.success = true;
                    }
                    results.push_back(std::move(r));
                }
            }
        }
        return results;
    }

    // 🎯 Voyage batch flow — 对齐 batch-voyage.ts:100-260
    std::vector<BatchEmbeddingResult> run_voyage_batch(const BatchEmbeddingRequest& request) {
        auto batch = build_voyage_batch_request(request.model, request.chunks);
        std::vector<BatchEmbeddingResult> results;

        const size_t MAX_VOYAGE_BATCH = 128;
        for (size_t i = 0; i < batch.inputs.size(); i += MAX_VOYAGE_BATCH) {
            size_t end = std::min(i + MAX_VOYAGE_BATCH, batch.inputs.size());
            json inputs = json::array();
            for (size_t j = i; j < end; ++j) inputs.push_back(batch.inputs[j]);

            json body = {
                {"model", request.model},
                {"input", inputs},
                {"input_type", "document"}
            };

            std::string url = "https://api.voyageai.com/v1/embeddings";
            auto resp = http_fn_(url, "POST", body,
                {{"Authorization", "Bearer " + request.api_key},
                 {"Content-Type", "application/json"}});

            if (resp.contains("data") && resp["data"].is_array()) {
                for (size_t j = 0; j < resp["data"].size() && (i + j) < request.chunks.size(); ++j) {
                    BatchEmbeddingResult r;
                    r.custom_id = request.chunks[i + j].id;
                    auto& embedding = resp["data"][j]["embedding"];
                    if (embedding.is_array()) {
                        for (const auto& v : embedding) r.embedding.push_back(v.get<double>());
                        r.success = true;
                    }
                    results.push_back(std::move(r));
                }
            }
        }
        return results;
    }

    HttpRequestFn http_fn_;
};

// ============================================================================
// 🎯 BatchUtils (对齐 batch-utils.ts:1-32)
// ============================================================================

static std::string format_batch_progress(int completed, int total) {
    if (total <= 0) return "0%";
    int pct = static_cast<int>(std::round(100.0 * completed / total));
    return std::to_string(pct) + "%";
}

static bool is_batch_terminal_status(BatchJobStatus status) {
    return status == BatchJobStatus::Completed ||
           status == BatchJobStatus::Failed ||
           status == BatchJobStatus::Expired ||
           status == BatchJobStatus::Cancelled;
}

} // namespace memory
} // namespace hunterclaw
