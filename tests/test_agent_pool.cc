#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/agents/agent_instance.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>

using namespace hunterclaw;
using namespace hunterclaw::agents;

// ============================================================================
// Fixtures
// ============================================================================

class AgentPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建测试配置
        config = std::make_shared<core::Config>();
        config->agents.default_agent = "default";
        
        // 添加测试 Agent 配置
        AgentConfig default_config;
        default_config.id = "default";
        default_config.workspace = "/tmp/hunterclaw_test/workspace";
        default_config.skills = {"*"};  // 所有工具
        config->agent_configs["default"] = default_config;
        
        AgentConfig restricted_config;
        restricted_config.id = "restricted";
        restricted_config.workspace = "/tmp/hunterclaw_test/workspace";
        restricted_config.skills = {"read_file", "write_file"};  // 仅限部分工具
        config->agent_configs["restricted"] = restricted_config;
        
        // 创建管理器 (stub)
        skill_mgr = std::make_shared<skills::SkillManager>(config);
        channel_mgr = std::make_shared<channels::ChannelManager>(config);
        
        // 创建 AgentPool
        AgentPool::Config pool_config;
        pool_config.idle_ttl = std::chrono::seconds(5);  // 5 秒清理
        pool_config.max_agents = 10;
        pool_config.auto_cleanup = false;  // 手动控制清理
        
        agent_pool = std::make_shared<AgentPool>(
            config, skill_mgr, channel_mgr, pool_config
        );
    }
    
    void TearDown() override {
        agent_pool.reset();
    }
    
    std::shared_ptr<core::Config> config;
    std::shared_ptr<skills::SkillManager> skill_mgr;
    std::shared_ptr<channels::ChannelManager> channel_mgr;
    std::shared_ptr<AgentPool> agent_pool;
};

// ============================================================================
// Test Cases
// ============================================================================

TEST_F(AgentPoolTest, CreateAgent) {
    // 创建 Agent
    auto agent = agent_pool->get_or_create_agent("session-1");
    
    ASSERT_NE(agent, nullptr);
    EXPECT_EQ(agent->context().session_id, "session-1");
    EXPECT_EQ(agent->context().agent_id, "default");
    EXPECT_FALSE(agent->context().workspace_dir.empty());
}

TEST_F(AgentPoolTest, ReuseExistingAgent) {
    // 创建 Agent
    auto agent1 = agent_pool->get_or_create_agent("session-1");
    ASSERT_NE(agent1, nullptr);
    
    // 再次获取 (应该返回同一个实例)
    auto agent2 = agent_pool->get_or_create_agent("session-1");
    ASSERT_NE(agent2, nullptr);
    
    // 验证是同一个实例
    EXPECT_EQ(agent1.get(), agent2.get());
}

TEST_F(AgentPoolTest, MultipleSessionsMultipleAgents) {
    // 创建 3 个不同 Session 的 Agent
    auto agent_a = agent_pool->get_or_create_agent("session-a");
    auto agent_b = agent_pool->get_or_create_agent("session-b");
    auto agent_c = agent_pool->get_or_create_agent("session-c");
    
    ASSERT_NE(agent_a, nullptr);
    ASSERT_NE(agent_b, nullptr);
    ASSERT_NE(agent_c, nullptr);
    
    // 验证都是独立实例
    EXPECT_NE(agent_a.get(), agent_b.get());
    EXPECT_NE(agent_b.get(), agent_c.get());
    EXPECT_NE(agent_a.get(), agent_c.get());
    
    // 验证 Workspace 隔离
    EXPECT_NE(agent_a->context().workspace_dir, agent_b->context().workspace_dir);
    EXPECT_NE(agent_b->context().workspace_dir, agent_c->context().workspace_dir);
}

TEST_F(AgentPoolTest, ExplicitAgentID) {
    // 显式指定 agent_id
    auto agent = agent_pool->get_or_create_agent("session-1", "restricted");
    
    ASSERT_NE(agent, nullptr);
    EXPECT_EQ(agent->context().agent_id, "restricted");
    
    // 验证工具过滤
    const auto& tools = agent->context().tools;
    EXPECT_EQ(tools.size(), 2);
    EXPECT_NE(std::find(tools.begin(), tools.end(), "read_file"), tools.end());
    EXPECT_NE(std::find(tools.begin(), tools.end(), "write_file"), tools.end());
    EXPECT_EQ(std::find(tools.begin(), tools.end(), "execute_command"), tools.end());
}

TEST_F(AgentPoolTest, ListAgents) {
    // 创建多个 Agent
    agent_pool->get_or_create_agent("session-1");
    agent_pool->get_or_create_agent("session-2");
    agent_pool->get_or_create_agent("session-3", "restricted");
    
    // 列出所有 Agent
    auto agents = agent_pool->list_agents();
    
    EXPECT_EQ(agents.size(), 3);
    
    // 验证信息完整性
    for (const auto& info : agents) {
        EXPECT_FALSE(info.session_id.empty());
        EXPECT_FALSE(info.agent_id.empty());
        EXPECT_FALSE(info.workspace_dir.empty());
    }
}

TEST_F(AgentPoolTest, RemoveAgent) {
    // 创建 Agent
    auto agent = agent_pool->get_or_create_agent("session-1");
    ASSERT_NE(agent, nullptr);
    
    // 移除
    agent_pool->remove_agent("session-1");
    
    // 验证列表为空
    auto agents = agent_pool->list_agents();
    EXPECT_EQ(agents.size(), 0);
    
    // 再次获取应该创建新实例
    auto agent2 = agent_pool->get_or_create_agent("session-1");
    ASSERT_NE(agent2, nullptr);
    EXPECT_NE(agent.get(), agent2.get());
}

TEST_F(AgentPoolTest, GetStats) {
    // 创建多个 Agent
    agent_pool->get_or_create_agent("session-1");
    agent_pool->get_or_create_agent("session-2");
    agent_pool->get_or_create_agent("session-3", "restricted");
    agent_pool->get_or_create_agent("session-4", "restricted");
    
    // 获取统计
    auto stats = agent_pool->get_stats();
    
    EXPECT_EQ(stats.total_agents, 4);
    EXPECT_EQ(stats.active_agents, 4);  // 刚创建都是活跃的
    EXPECT_EQ(stats.agents_by_agent_id["default"], 2);
    EXPECT_EQ(stats.agents_by_agent_id["restricted"], 2);
}

TEST_F(AgentPoolTest, IdleCleanup) {
    // 创建 Agent
    agent_pool->get_or_create_agent("session-1");
    agent_pool->get_or_create_agent("session-2");
    
    EXPECT_EQ(agent_pool->list_agents().size(), 2);
    
    // 等待超过 idle_ttl (5 秒)
    std::this_thread::sleep_for(std::chrono::seconds(6));
    
    // 清理闲置 Agent
    size_t cleaned = agent_pool->cleanup_idle_agents();
    EXPECT_EQ(cleaned, 2);
    
    // 验证已清理
    EXPECT_EQ(agent_pool->list_agents().size(), 0);
}

TEST_F(AgentPoolTest, SelectiveIdleCleanup) {
    // 创建 3 个 Agent
    auto agent1 = agent_pool->get_or_create_agent("session-1");
    auto agent2 = agent_pool->get_or_create_agent("session-2");
    auto agent3 = agent_pool->get_or_create_agent("session-3");
    
    // 等待 3 秒
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    // agent2 活跃一下 (touch)
    agent2->touch();
    
    // 等待 3 秒 (总共 6 秒,超过 5 秒 TTL)
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    // 清理 (agent1 和 agent3 应该被清理,agent2 保留)
    size_t cleaned = agent_pool->cleanup_idle_agents();
    EXPECT_EQ(cleaned, 2);
    
    // 验证 agent2 还在
    auto agents = agent_pool->list_agents();
    EXPECT_EQ(agents.size(), 1);
    EXPECT_EQ(agents[0].session_id, "session-2");
}

TEST_F(AgentPoolTest, MaxAgentsLimit) {
    // 创建到达上限 (10 个)
    for (int i = 0; i < 10; i++) {
        auto agent = agent_pool->get_or_create_agent("session-" + std::to_string(i));
        ASSERT_NE(agent, nullptr);
    }
    
    EXPECT_EQ(agent_pool->list_agents().size(), 10);
    
    // 尝试创建第 11 个 (应该触发自动清理)
    // 由于所有 Agent 都是新创建的,无法清理,应该返回 nullptr
    auto agent11 = agent_pool->get_or_create_agent("session-11");
    EXPECT_EQ(agent11, nullptr);
}

TEST_F(AgentPoolTest, MaxAgentsLimitWithAutoCleanup) {
    // 创建到达上限
    for (int i = 0; i < 10; i++) {
        auto agent = agent_pool->get_or_create_agent("session-" + std::to_string(i));
        ASSERT_NE(agent, nullptr);
    }
    
    // 等待超过 TTL
    std::this_thread::sleep_for(std::chrono::seconds(6));
    
    // 创建第 11 个 (应该先清理旧的,然后成功创建)
    auto agent11 = agent_pool->get_or_create_agent("session-11");
    ASSERT_NE(agent11, nullptr);
    EXPECT_EQ(agent11->context().session_id, "session-11");
}

TEST_F(AgentPoolTest, WorkspaceIsolation) {
    // 创建 2 个不同 agent_id 的 Agent
    auto agent_default = agent_pool->get_or_create_agent("session-1", "default");
    auto agent_restricted = agent_pool->get_or_create_agent("session-2", "restricted");
    
    ASSERT_NE(agent_default, nullptr);
    ASSERT_NE(agent_restricted, nullptr);
    
    // 验证 Workspace 路径不同
    std::string ws_default = agent_default->context().workspace_dir;
    std::string ws_restricted = agent_restricted->context().workspace_dir;
    
    EXPECT_NE(ws_default, ws_restricted);
    
    // 验证路径包含 agent_id 和 session_id
    EXPECT_NE(ws_default.find("default"), std::string::npos);
    EXPECT_NE(ws_default.find("session-1"), std::string::npos);
    
    EXPECT_NE(ws_restricted.find("restricted"), std::string::npos);
    EXPECT_NE(ws_restricted.find("session-2"), std::string::npos);
}

TEST_F(AgentPoolTest, ToolsFiltering) {
    // 创建 default Agent (所有工具)
    auto agent_default = agent_pool->get_or_create_agent("session-1", "default");
    ASSERT_NE(agent_default, nullptr);
    
    // 创建 restricted Agent (仅限部分工具)
    auto agent_restricted = agent_pool->get_or_create_agent("session-2", "restricted");
    ASSERT_NE(agent_restricted, nullptr);
    
    // 验证工具数量
    EXPECT_GT(agent_default->context().tools.size(), agent_restricted->context().tools.size());
    
    // 验证 restricted 只有允许的工具
    const auto& tools = agent_restricted->context().tools;
    for (const auto& tool : tools) {
        EXPECT_TRUE(tool == "read_file" || tool == "write_file");
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
