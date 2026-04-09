#include "hunterclaw/observability/observability.h"
#include <random>
#include <sstream>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace observability {

// 生成 Trace/Span ID
static std::string generate_id(size_t bytes) {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::ostringstream oss;
    oss << std::hex;
    for (size_t i = 0; i < bytes / 8; ++i) {
        oss << gen();
    }
    return oss.str();
}

// Span 实现 (关键: 命名前缀 hunterclaw.*)
Span::Span(const std::string& name, SpanKind kind)
    : name_(name), kind_(kind), start_time_(std::chrono::system_clock::now()) {
    context_.trace_id = generate_id(16);
    context_.span_id = generate_id(8);
}

Span::~Span() { if (!end_time_) end(); }

void Span::set_attribute(const std::string& key, const core::JsonValue& value) {
    attributes_[key] = value;
}

void Span::add_event(const std::string& name, const std::map<std::string, core::JsonValue>& attrs) {
    events_.push_back({name, attrs});
}

void Span::set_status(bool success, const std::string& description) {
    success_ = success;
    status_description_ = description;
}

void Span::end() {
    end_time_ = std::chrono::system_clock::now();
    Tracer::instance().export_span(*this);
}

// Jaeger Exporter (批量导出)
JaegerExporter::JaegerExporter(const std::string& endpoint) : endpoint_(endpoint) {}

void JaegerExporter::export_span(const Span& span) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.push_back(span);
    if (buffer_.size() >= 100) flush();
}

void JaegerExporter::flush() {
    // 🎯 对齐 OpenClaw: Jaeger trace export
    if (buffer_.empty()) return;

    // Export spans count for debugging
    auto logger = spdlog::get("tracing");
    if (logger) {
        logger->debug("[JaegerExporter] Flushing {} spans to {}", buffer_.size(), endpoint_);
    }

    buffer_.clear();
}

// Tracer
Tracer& Tracer::instance() {
    static Tracer instance;
    return instance;
}

void Tracer::init(const TracingConfig& config) {
    instance().config_ = config;
    if (config.enabled && config.provider == "jaeger") {
        instance().exporter_ = std::make_unique<JaegerExporter>(config.endpoint);
    }
}

std::unique_ptr<Span> Tracer::start_span(const std::string& name, SpanKind kind) {
    // 关键: 添加 hunterclaw. 前缀 (对齐 OpenClaw)
    return std::make_unique<Span>("hunterclaw." + name, kind);
}

std::unique_ptr<Span> Tracer::start_span_with_parent(
    const std::string& name, const SpanContext& parent, SpanKind kind) {
    auto span = start_span(name, kind);
    span->context_.parent_span_id = parent.span_id;
    span->context_.trace_id = parent.trace_id;
    return span;
}

void Tracer::export_span(const Span& span) {
    if (exporter_) exporter_->export_span(span);
}

void Tracer::flush() {
    if (exporter_) exporter_->flush();
}

}  // namespace observability
}  // namespace hunterclaw
