#include <gtest/gtest.h>
#include "hunterclaw/react/verbose_reasoning_config.h"
#include "hunterclaw/react/model_catalog.h"

using namespace hunterclaw::react;

// ============================================================================
// Verbose Level 解析测试
// ============================================================================

class VerboseLevelTest : public ::testing::Test {
protected:
    AgentDefaultsConfig defaults_;
    SessionLevelOverrides session_;
    MessageDirectives directives_;
    
    void SetUp() override {
        defaults_.verbose_default = VerboseLevel::Off;
    }
};

TEST_F(VerboseLevelTest, InlineDirectiveHasHighestPriority) {
    // OpenClaw 对齐: 内联指令优先级最高
    defaults_.verbose_default = VerboseLevel::Off;
    session_.verbose_level = VerboseLevel::On;
    directives_.verbose_level = VerboseLevel::Full;
    
    auto level = resolve_verbose_level(directives_, &session_, defaults_);
    EXPECT_EQ(level, VerboseLevel::Full);  // ✅ 内联指令生效
}

TEST_F(VerboseLevelTest, SessionOverrideSecondPriority) {
    // OpenClaw 对齐: 会话覆盖次之
    defaults_.verbose_default = VerboseLevel::Off;
    session_.verbose_level = VerboseLevel::On;
    directives_.verbose_level = std::nullopt;
    
    auto level = resolve_verbose_level(directives_, &session_, defaults_);
    EXPECT_EQ(level, VerboseLevel::On);  // ✅ 会话覆盖生效
}

TEST_F(VerboseLevelTest, GlobalDefaultLowestPriority) {
    // OpenClaw 对齐: 全局配置优先级最低
    defaults_.verbose_default = VerboseLevel::Full;
    session_.verbose_level = std::nullopt;
    directives_.verbose_level = std::nullopt;
    
    auto level = resolve_verbose_level(directives_, &session_, defaults_);
    EXPECT_EQ(level, VerboseLevel::Full);  // ✅ 全局配置生效
}

TEST_F(VerboseLevelTest, DefaultsToOffWhenNoConfig) {
    // OpenClaw 对齐: 默认 Off
    defaults_.verbose_default = std::nullopt;
    session_.verbose_level = std::nullopt;
    directives_.verbose_level = std::nullopt;
    
    auto level = resolve_verbose_level(directives_, &session_, defaults_);
    EXPECT_EQ(level, VerboseLevel::Off);  // ✅ 默认 Off
}

// ============================================================================
// Reasoning Level 解析测试
// ============================================================================

class ReasoningLevelTest : public ::testing::Test {
protected:
    AgentDefaultsConfig defaults_;
    SessionLevelOverrides session_;
    MessageDirectives directives_;
    ModelCapabilities model_caps_;
    
    void SetUp() override {
        defaults_.auto_enable_reasoning_for_capable_models = true;
        model_caps_.provider = "anthropic";
        model_caps_.model_id = "claude-opus-4";
        model_caps_.supports_reasoning = true;  // ✅ 支持推理
    }
};

TEST_F(ReasoningLevelTest, InlineDirectiveHasHighestPriority) {
    // OpenClaw 对齐: 内联指令优先级最高
    directives_.reasoning_level = ReasoningLevel::Off;
    session_.reasoning_level = ReasoningLevel::On;
    
    auto level = resolve_reasoning_level(
        directives_, &session_, defaults_, model_caps_, false);
    EXPECT_EQ(level, ReasoningLevel::Off);  // ✅ 内联指令生效 (即使模型支持)
}

TEST_F(ReasoningLevelTest, SessionOverrideSecondPriority) {
    // OpenClaw 对齐: 会话覆盖次之
    directives_.reasoning_level = std::nullopt;
    session_.reasoning_level = ReasoningLevel::On;
    
    auto level = resolve_reasoning_level(
        directives_, &session_, defaults_, model_caps_, false);
    EXPECT_EQ(level, ReasoningLevel::On);  // ✅ 会话覆盖生效
}

TEST_F(ReasoningLevelTest, AutoEnableForCapableModels) {
    // OpenClaw 对齐: 模型能力自动检测
    directives_.reasoning_level = std::nullopt;
    session_.reasoning_level = std::nullopt;
    
    auto level = resolve_reasoning_level(
        directives_, &session_, defaults_, model_caps_, false);
    EXPECT_EQ(level, ReasoningLevel::On);  // ✅ 自动启用 (模型支持)
}

TEST_F(ReasoningLevelTest, NoAutoEnableWhenThinkingActive) {
    // OpenClaw 对齐: thinking 激活时不自动启用 reasoning
    directives_.reasoning_level = std::nullopt;
    session_.reasoning_level = std::nullopt;
    
    auto level = resolve_reasoning_level(
        directives_, &session_, defaults_, model_caps_, true);  // thinking_active = true
    EXPECT_EQ(level, ReasoningLevel::Off);  // ✅ 不自动启用
}

TEST_F(ReasoningLevelTest, NoAutoEnableWhenModelNotSupport) {
    // OpenClaw 对齐: 模型不支持时不自动启用
    model_caps_.supports_reasoning = false;  // ❌ 不支持推理
    directives_.reasoning_level = std::nullopt;
    session_.reasoning_level = std::nullopt;
    
    auto level = resolve_reasoning_level(
        directives_, &session_, defaults_, model_caps_, false);
    EXPECT_EQ(level, ReasoningLevel::Off);  // ✅ 不自动启用
}

TEST_F(ReasoningLevelTest, NoAutoEnableWhenConfigDisabled) {
    // HunterClaw 扩展: 配置禁用自动启用
    defaults_.auto_enable_reasoning_for_capable_models = false;
    directives_.reasoning_level = std::nullopt;
    session_.reasoning_level = std::nullopt;
    
    auto level = resolve_reasoning_level(
        directives_, &session_, defaults_, model_caps_, false);
    EXPECT_EQ(level, ReasoningLevel::Off);  // ✅ 不自动启用
}

// ============================================================================
// 工具输出过滤测试
// ============================================================================

TEST(ToolOutputFilterTest, OffLevelShouldNotIncludeOutput) {
    // OpenClaw 对齐: verbose=off 时不显示工具输出
    EXPECT_FALSE(should_include_tool_output(VerboseLevel::Off));
}

TEST(ToolOutputFilterTest, OnLevelShouldNotIncludeOutput) {
    // OpenClaw 对齐: verbose=on 时不显示工具输出 (只显示名称)
    EXPECT_FALSE(should_include_tool_output(VerboseLevel::On));
}

TEST(ToolOutputFilterTest, FullLevelShouldIncludeOutput) {
    // OpenClaw 对齐: verbose=full 时显示完整输出
    EXPECT_TRUE(should_include_tool_output(VerboseLevel::Full));
}

TEST(ToolOutputFilterTest, FilterDataForOnLevel) {
    // OpenClaw 对齐: verbose=on 时删除 result 和 partialResult
    ToolEventData data;
    data.tool_name = "read_file";
    data.args = {{"path", "/test.py"}};
    data.result = {{"content", "test content"}};
    data.partial_result = {{"chunk", "partial"}};
    
    filter_tool_event_data(data, VerboseLevel::On);
    
    // ✅ result 和 partialResult 应该被清空
    EXPECT_TRUE(data.result.is_null() || 
                (data.result.is_object() && data.result.empty()));
    EXPECT_TRUE(data.partial_result.is_null() || 
                (data.partial_result.is_object() && data.partial_result.empty()));
}

TEST(ToolOutputFilterTest, KeepDataForFullLevel) {
    // OpenClaw 对齐: verbose=full 时保留完整输出
    ToolEventData data;
    data.tool_name = "read_file";
    data.args = {{"path", "/test.py"}};
    data.result = {{"content", "test content"}};
    
    auto original_result = data.result;
    filter_tool_event_data(data, VerboseLevel::Full);
    
    // ✅ result 应该保持不变
    EXPECT_EQ(data.result, original_result);
}

// ============================================================================
// 模型目录测试
// ============================================================================

class ModelCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 清空目录以确保测试独立性
        // 注意: 实际使用时不要清空内置模型
    }
};

TEST_F(ModelCatalogTest, ClaudeOpus4SupportsReasoning) {
    // OpenClaw 对齐: Claude Opus 4 支持推理
    auto& catalog = ModelCatalog::instance();
    EXPECT_TRUE(catalog.supports_reasoning("anthropic", "claude-opus-4"));
    EXPECT_TRUE(catalog.supports_reasoning("anthropic", "claude-opus-4-20250514"));
}

TEST_F(ModelCatalogTest, ClaudeSonnet4SupportsReasoning) {
    // OpenClaw 对齐: Claude Sonnet 4 支持推理
    auto& catalog = ModelCatalog::instance();
    EXPECT_TRUE(catalog.supports_reasoning("anthropic", "claude-sonnet-4-20250514"));
}

TEST_F(ModelCatalogTest, Claude35SonnetNoReasoning) {
    // OpenClaw 对齐: Claude 3.5 Sonnet 不支持推理
    auto& catalog = ModelCatalog::instance();
    EXPECT_FALSE(catalog.supports_reasoning("anthropic", "claude-3-5-sonnet-20241022"));
}

TEST_F(ModelCatalogTest, OpenAIO1SupportsReasoning) {
    // OpenClaw 对齐: OpenAI o1 系列支持推理
    auto& catalog = ModelCatalog::instance();
    EXPECT_TRUE(catalog.supports_reasoning("openai", "o1"));
    EXPECT_TRUE(catalog.supports_reasoning("openai", "o1-preview"));
    EXPECT_TRUE(catalog.supports_reasoning("openai", "o1-mini"));
}

TEST_F(ModelCatalogTest, GPT4oNoReasoning) {
    // OpenClaw 对齐: GPT-4o 不支持推理
    auto& catalog = ModelCatalog::instance();
    EXPECT_FALSE(catalog.supports_reasoning("openai", "gpt-4o"));
    EXPECT_FALSE(catalog.supports_reasoning("openai", "gpt-4o-mini"));
}

TEST_F(ModelCatalogTest, GeminiThinkingSupportsReasoning) {
    // OpenClaw 对齐: Gemini 2.0 Thinking 支持推理
    auto& catalog = ModelCatalog::instance();
    EXPECT_TRUE(catalog.supports_reasoning("google", "gemini-2.0-flash-thinking-exp"));
}

TEST_F(ModelCatalogTest, DeepseekReasonerSupportsReasoning) {
    // Deepseek Reasoner 支持推理
    auto& catalog = ModelCatalog::instance();
    EXPECT_TRUE(catalog.supports_reasoning("deepseek", "deepseek-reasoner"));
}

TEST_F(ModelCatalogTest, UnknownModelDefaultsToNoReasoning) {
    // 未知模型默认不支持推理
    auto& catalog = ModelCatalog::instance();
    EXPECT_FALSE(catalog.supports_reasoning("unknown", "unknown-model"));
}

TEST_F(ModelCatalogTest, RegisterCustomModel) {
    // 测试自定义模型注册
    ModelCapabilities custom;
    custom.provider = "custom";
    custom.model_id = "test-reasoning-model";
    custom.supports_reasoning = true;
    
    auto& catalog = ModelCatalog::instance();
    catalog.register_model(custom);
    
    EXPECT_TRUE(catalog.supports_reasoning("custom", "test-reasoning-model"));
}

// ============================================================================
// 推理格式化测试
// ============================================================================

TEST(ReasoningFormatTest, FormatReasoningText) {
    // OpenClaw 对齐: "Reasoning:\n_<content>_"
    std::string raw = "I need to analyze the file first";
    std::string formatted = format_reasoning_text(raw);
    
    EXPECT_EQ(formatted, "Reasoning:\n_I need to analyze the file first_");
}

TEST(ReasoningFormatTest, EmptyReasoningReturnsEmpty) {
    std::string formatted = format_reasoning_text("");
    EXPECT_EQ(formatted, "");
}

TEST(ReasoningFormatTest, ReasoningBeforeAnswerInMessageEndMode) {
    // OpenClaw 对齐: message_end 模式下先发推理
    bool before = should_emit_reasoning_before_answer(
        BlockStreamingBreak::MessageEnd, false);
    EXPECT_TRUE(before);
}

TEST(ReasoningFormatTest, ReasoningAfterAnswerInTextEndMode) {
    // OpenClaw 对齐: text_end 模式下后发推理
    bool before = should_emit_reasoning_before_answer(
        BlockStreamingBreak::TextEnd, false);
    EXPECT_FALSE(before);
}

// ============================================================================
// 完整配置解析测试
// ============================================================================

TEST(FullConfigTest, ResolveCompleteConfig) {
    // 测试完整配置解析
    AgentDefaultsConfig defaults;
    defaults.verbose_default = VerboseLevel::Full;
    defaults.block_streaming_default = true;
    defaults.block_streaming_break = BlockStreamingBreak::TextEnd;
    defaults.auto_enable_reasoning_for_capable_models = true;
    
    MessageDirectives directives;
    SessionLevelOverrides session;
    
    ModelCapabilities caps;
    caps.provider = "anthropic";
    caps.model_id = "claude-opus-4";
    caps.supports_reasoning = true;
    
    auto resolved = resolve_verbose_reasoning_config(
        directives, &session, defaults, caps, false);
    
    EXPECT_EQ(resolved.verbose_level, VerboseLevel::Full);
    EXPECT_EQ(resolved.reasoning_level, ReasoningLevel::On);  // 自动启用
    EXPECT_TRUE(resolved.block_streaming_enabled);
    EXPECT_EQ(resolved.streaming_break, BlockStreamingBreak::TextEnd);
}

// ============================================================================
// 字符串转换测试
// ============================================================================

TEST(StringConversionTest, VerboseLevelToString) {
    EXPECT_EQ(to_string(VerboseLevel::Off), "off");
    EXPECT_EQ(to_string(VerboseLevel::On), "on");
    EXPECT_EQ(to_string(VerboseLevel::Full), "full");
}

TEST(StringConversionTest, ParseVerboseLevel) {
    EXPECT_EQ(*parse_verbose_level("off"), VerboseLevel::Off);
    EXPECT_EQ(*parse_verbose_level("on"), VerboseLevel::On);
    EXPECT_EQ(*parse_verbose_level("full"), VerboseLevel::Full);
    EXPECT_EQ(*parse_verbose_level("OFF"), VerboseLevel::Off);  // 大小写不敏感
    EXPECT_FALSE(parse_verbose_level("invalid").has_value());
}

TEST(StringConversionTest, ReasoningLevelToString) {
    EXPECT_EQ(to_string(ReasoningLevel::Off), "off");
    EXPECT_EQ(to_string(ReasoningLevel::On), "on");
    EXPECT_EQ(to_string(ReasoningLevel::Stream), "stream");
}

TEST(StringConversionTest, ParseReasoningLevel) {
    EXPECT_EQ(*parse_reasoning_level("off"), ReasoningLevel::Off);
    EXPECT_EQ(*parse_reasoning_level("on"), ReasoningLevel::On);
    EXPECT_EQ(*parse_reasoning_level("stream"), ReasoningLevel::Stream);
    EXPECT_FALSE(parse_reasoning_level("invalid").has_value());
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
