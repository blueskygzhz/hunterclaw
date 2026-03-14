#include "hunterclaw/agents/subagent.h"
#include "hunterclaw/core/utils.h"
#include <iostream>
#include <algorithm>
#include <thread>
#include <sstream>
#include <iomanip>
#include <atomic>

namespace hunterclaw {
namespace agents {

// Type aliases for forward-declared types
using AgentPtr = std::shared_ptr<Agent>;

// ============================================================================
// SubagentRegistry Implementation
// ============================================================================

SubagentRegistry::Ptr SubagentRegistry::create() {
    return std::make_shared<SubagentRegistry>();
}

core::Result<void> SubagentRegistry::register_subagent(const SubagentEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (entry.subagent_run_id.empty()) {
        return core::Result<void>::Err("Subagent run ID cannot be empty");
    }
    
    if (entries_.count(entry.subagent_run_id)) {
        return core::Result<void>::Err(
            "Subagent already registered: " + entry.subagent_run_id
        );
    }
    
    entries_[entry.subagent_run_id] = std::make_shared<SubagentEntry>(entry);
    
    std::cout << "[SubagentRegistry] Registered subagent: " 
              << entry.subagent_run_id 
              << " (requester: " << entry.requester_run_id 
              << ", depth: " << entry.depth_info.depth << ")" << std::endl;
    
    return core::Result<void>::Ok();
}

std::shared_ptr<SubagentEntry> SubagentRegistry::get_entry(
    const std::string& subagent_run_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(subagent_run_id);
    return it != entries_.end() ? it->second : nullptr;
}

core::Result<void> SubagentRegistry::update_state(
    const std::string& subagent_run_id,
    SubagentState new_state
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entries_.find(subagent_run_id);
    if (it == entries_.end()) {
        return core::Result<void>::Err("Subagent not found: " + subagent_run_id);
    }
    
    it->second->state = new_state;
    
    // Update timestamps
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    if (new_state == SubagentState::Running && it->second->started_at == 0) {
        it->second->started_at = now;
    } else if (new_state == SubagentState::Completed || 
               new_state == SubagentState::Failed ||
               new_state == SubagentState::TimedOut ||
               new_state == SubagentState::Killed) {
        it->second->completed_at = now;
    }
    
    return core::Result<void>::Ok();
}

core::Result<void> SubagentRegistry::set_result(
    const std::string& subagent_run_id,
    const std::string& result_text,
    bool has_error,
    const std::string& error_message
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entries_.find(subagent_run_id);
    if (it == entries_.end()) {
        return core::Result<void>::Err("Subagent not found: " + subagent_run_id);
    }
    
    it->second->result_text = result_text;
    it->second->has_error = has_error;
    it->second->error_message = error_message;
    
    if (has_error) {
        it->second->state = SubagentState::Failed;
    } else {
        it->second->state = SubagentState::Completing;
    }
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    it->second->completed_at = now;
    
    return core::Result<void>::Ok();
}

core::Result<void> SubagentRegistry::mark_delivered(const std::string& subagent_run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entries_.find(subagent_run_id);
    if (it == entries_.end()) {
        return core::Result<void>::Err("Subagent not found: " + subagent_run_id);
    }
    
    it->second->result_delivered = true;
    if (it->second->state == SubagentState::Announcing) {
        it->second->state = SubagentState::Completed;
    }
    
    return core::Result<void>::Ok();
}

core::Result<void> SubagentRegistry::record_delivery_attempt(
    const std::string& subagent_run_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entries_.find(subagent_run_id);
    if (it == entries_.end()) {
        return core::Result<void>::Err("Subagent not found: " + subagent_run_id);
    }
    
    it->second->delivery_attempts++;
    it->second->last_delivery_attempt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    return core::Result<void>::Ok();
}

std::vector<std::shared_ptr<SubagentEntry>> SubagentRegistry::list_by_requester(
    const std::string& requester_run_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::shared_ptr<SubagentEntry>> result;
    for (const auto& pair : entries_) {
        if (pair.second->requester_run_id == requester_run_id) {
            result.push_back(pair.second);
        }
    }
    
    return result;
}

std::vector<std::shared_ptr<SubagentEntry>> SubagentRegistry::list_active() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::shared_ptr<SubagentEntry>> result;
    for (const auto& pair : entries_) {
        auto state = pair.second->state;
        if (state != SubagentState::Completed && 
            state != SubagentState::Failed &&
            state != SubagentState::Killed) {
            result.push_back(pair.second);
        }
    }
    
    return result;
}

SubagentDepthInfo SubagentRegistry::get_depth_info(const std::string& run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entries_.find(run_id);
    if (it != entries_.end()) {
        return it->second->depth_info;
    }
    
    // Return default (top-level)
    SubagentDepthInfo info;
    info.root_run_id = run_id;
    return info;
}

void SubagentRegistry::cleanup_old_entries(int max_age_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    std::vector<std::string> to_remove;
    for (const auto& pair : entries_) {
        const auto& entry = pair.second;
        
        // Only cleanup completed/failed/killed entries
        if (entry->state == SubagentState::Completed ||
            entry->state == SubagentState::Failed ||
            entry->state == SubagentState::Killed) {
            
            double age = (now - entry->spawned_at) / 1000.0;
            if (age > max_age_seconds) {
                to_remove.push_back(pair.first);
            }
        }
    }
    
    for (const auto& run_id : to_remove) {
        entries_.erase(run_id);
    }
    
    if (!to_remove.empty()) {
        std::cout << "[SubagentRegistry] Cleaned up " << to_remove.size() 
                  << " old entries" << std::endl;
    }
}

void SubagentRegistry::find_descendants_recursive(
    const std::string& run_id,
    std::vector<std::string>& descendants
) {
    for (const auto& pair : entries_) {
        if (pair.second->requester_run_id == run_id) {
            descendants.push_back(pair.first);
            // Recursively find children of this child
            find_descendants_recursive(pair.first, descendants);
        }
    }
}

core::Result<void> SubagentRegistry::kill_subagent_tree(
    const std::string& subagent_run_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entries_.find(subagent_run_id);
    if (it == entries_.end()) {
        return core::Result<void>::Err("Subagent not found: " + subagent_run_id);
    }
    
    // Find all descendants
    std::vector<std::string> descendants;
    find_descendants_recursive(subagent_run_id, descendants);
    
    // Kill the subagent itself
    it->second->state = SubagentState::Killed;
    
    // Kill all descendants
    for (const auto& desc_id : descendants) {
        auto desc_it = entries_.find(desc_id);
        if (desc_it != entries_.end()) {
            desc_it->second->state = SubagentState::Killed;
        }
    }
    
    std::cout << "[SubagentRegistry] Killed subagent tree: " 
              << subagent_run_id << " (" << descendants.size() 
              << " descendants)" << std::endl;
    
    return core::Result<void>::Ok();
}

// ============================================================================
// SubagentSpawner Implementation
// ============================================================================

SubagentSpawner::Ptr SubagentSpawner::create(
    std::shared_ptr<AgentManager> agent_mgr,
    std::shared_ptr<SubagentRegistry> registry
) {
    return std::make_shared<SubagentSpawner>(agent_mgr, registry);
}

SubagentSpawner::SubagentSpawner(
    std::shared_ptr<AgentManager> agent_mgr,
    std::shared_ptr<SubagentRegistry> registry
)
    : agent_manager_(agent_mgr)
    , registry_(registry)
{
}

core::Result<void> SubagentSpawner::validate_spawn_request(
    const std::string& requester_run_id,
    const SubagentSpawnOptions& options
) {
    // Check basic fields
    if (options.subagent_name.empty()) {
        return core::Result<void>::Err("Subagent name cannot be empty");
    }
    
    if (options.prompt.empty()) {
        return core::Result<void>::Err("Subagent prompt cannot be empty");
    }
    
    // Check depth
    auto depth_info = registry_->get_depth_info(requester_run_id);
    if (!depth_info.can_spawn_child()) {
        return core::Result<void>::Err(
            "Maximum subagent nesting depth reached: " + 
            std::to_string(depth_info.max_depth)
        );
    }
    
    return core::Result<void>::Ok();
}

AgentConfig SubagentSpawner::build_subagent_config(
    const std::string& requester_agent_id,
    const SubagentSpawnOptions& options,
    const SubagentDepthInfo& depth_info
) {
    // Get requester agent config as base
    auto requester_agent = agent_manager_->get_agent(requester_agent_id);
    AgentConfig config = requester_agent ? 
        requester_agent->get_config() : AgentConfig();
    
    // Override with subagent-specific settings
    config.id = options.subagent_name + "_" + std::to_string(depth_info.depth);
    config.name = options.subagent_name;
    
    // Apply option overrides
    if (!options.model.empty()) {
        config.model.model = options.model;
    }
    
    if (!options.thinking_level.empty()) {
        config.thinking_level = options.thinking_level;
    }
    
    if (options.timeout_seconds > 0) {
        config.timeout_seconds = options.timeout_seconds;
    }
    
    // Tool restrictions based on depth
    // TODO: AgentConfig doesn't have 'tools' field yet, commenting out for now
    /*
    if (depth_info.depth > 0) {
        // Subagents have restricted tool access
        config.tools.disabled.push_back("gateway");
        config.tools.disabled.push_back("cron");
        config.tools.disabled.push_back("deployment");
    }
    */
    
    return config;
}

void SubagentSpawner::setup_event_handlers(
    AgentPtr subagent,
    const std::string& subagent_run_id
) {
    // TODO: This requires Agent::on_event and AgentEvent which are not yet implemented
    // Commenting out for now
    /*
    // Register event handler to track lifecycle
    subagent->on_event([this, subagent_run_id](const AgentEvent& event) {
        if (event.stream == AgentEventStream::Lifecycle) {
            auto data = event.data;
            if (data.is_object() && data.contains("phase")) {
                int phase_int = static_cast<int>(data["phase"].as_number());
                auto phase = static_cast<AgentLifecyclePhase>(phase_int);
                
                switch (phase) {
                    case AgentLifecyclePhase::Start:
                        registry_->update_state(
                            subagent_run_id, 
                            SubagentState::Running
                        );
                        break;
                        
                    case AgentLifecyclePhase::End: {
                        // Extract result
                        std::string result_text;
                        bool has_error = false;
                        std::string error_msg;
                        
                        if (data.contains("response")) {
                            result_text = data["response"].as_string();
                        }
                        
                        if (data.contains("error")) {
                            has_error = true;
                            error_msg = data["error"].as_string();
                        }
                        
                        registry_->set_result(
                            subagent_run_id,
                            result_text,
                            has_error,
                            error_msg
                        );
                        
                        // Signal completion
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            completion_flags_[subagent_run_id] = true;
                        }
                        cv_.notify_all();
                        break;
                    }
                        
                    default:
                        break;
                }
            }
        }
    });
    */
}

core::Result<std::string> SubagentSpawner::spawn(
    const std::string& requester_run_id,
    const std::string& requester_agent_id,
    const SubagentSpawnOptions& options
) {
    // Validate request
    auto validate_result = validate_spawn_request(requester_run_id, options);
    if (validate_result.is_err()) {
        return core::Result<std::string>::Err(validate_result.error);
    }
    
    // Generate run ID (UUID-based)
    std::string subagent_run_id = "run_" + core::utils::generate_uuid();
    
    // TODO: Full implementation requires AgentManager and Agent which are not yet implemented
    // For now, return a stub implementation
    /*
    // Get depth info
    auto parent_depth = registry_->get_depth_info(requester_run_id);
    auto child_depth = parent_depth.child_depth(requester_run_id);
    
    // Build config
    auto subagent_config = build_subagent_config(
        requester_agent_id,
        options,
        child_depth
    );
    
    // Register agent (temporary)
    auto register_result = agent_manager_->register_agent(subagent_config);
    if (register_result.is_err()) {
        return core::Result<std::string>::Err(
            "Failed to register subagent: " + register_result.error
        );
    }
    
    // Get agent instance
    auto subagent = agent_manager_->get_agent(subagent_config.id);
    if (!subagent) {
        return core::Result<std::string>::Err("Failed to create subagent instance");
    }
    
    // Create registry entry
    SubagentEntry entry;
    entry.subagent_run_id = subagent_run_id;
    entry.requester_run_id = requester_run_id;
    entry.requester_agent_id = requester_agent_id;
    entry.subagent_name = options.subagent_name;
    entry.depth_info = child_depth;
    entry.state = SubagentState::Spawning;
    entry.description = options.description;
    entry.prompt = options.prompt;
    entry.timeout_seconds = options.timeout_seconds > 0 ? 
        options.timeout_seconds : 900;
    entry.spawned_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    // Register in registry
    auto reg_result = registry_->register_subagent(entry);
    if (reg_result.is_err()) {
        agent_manager_->unregister_agent(subagent_config.id);
        return core::Result<std::string>::Err(
            "Failed to register subagent in registry: " + reg_result.error
        );
    }
    
    // Setup event handlers
    setup_event_handlers(subagent, subagent_run_id);
    
    // Update state to listening
    registry_->update_state(subagent_run_id, SubagentState::Listening);
    
    // Generate a new run ID for execution
    std::string exec_run_id = "run_" + subagent_run_id.substr(4);  // Use same UUID
    
    // Start agent (async)
    std::map<std::string, std::string> run_options;
    run_options["session_key"] = subagent_run_id;
    
    std::thread([subagent, options, run_options]() {
        subagent->run(options.prompt, run_options);
    }).detach();
    
    std::cout << "[SubagentSpawner] Spawned subagent: " << subagent_run_id 
              << " (" << options.subagent_name << ", depth: " 
              << child_depth.depth << ")" << std::endl;
    
    // Wait for completion if requested
    if (options.wait_for_completion) {
        auto wait_result = wait_for_completion(
            subagent_run_id,
            options.timeout_seconds * 1000
        );
        if (wait_result.is_err()) {
            return core::Result<std::string>::Err(wait_result.error);
        }
    }
    */
    
    return core::Result<std::string>::Ok(subagent_run_id);
}

core::Result<std::string> SubagentSpawner::wait_for_completion(
    const std::string& subagent_run_id,
    int timeout_ms
) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    auto wait_condition = [this, &subagent_run_id]() {
        return completion_flags_.count(subagent_run_id) && 
               completion_flags_[subagent_run_id];
    };
    
    bool completed;
    if (timeout_ms > 0) {
        completed = cv_.wait_for(
            lock,
            std::chrono::milliseconds(timeout_ms),
            wait_condition
        );
    } else {
        cv_.wait(lock, wait_condition);
        completed = true;
    }
    
    if (!completed) {
        // Timeout
        registry_->update_state(subagent_run_id, SubagentState::TimedOut);
        return core::Result<std::string>::Err("Subagent execution timed out");
    }
    
    // Get result
    auto entry = registry_->get_entry(subagent_run_id);
    if (!entry) {
        return core::Result<std::string>::Err("Subagent entry not found");
    }
    
    if (entry->has_error) {
        return core::Result<std::string>::Err(entry->error_message);
    }
    
    return core::Result<std::string>::Ok(entry->result_text);
}

// ============================================================================
// SubagentAnnouncer Implementation
// ============================================================================

SubagentAnnouncer::Ptr SubagentAnnouncer::create(
    std::shared_ptr<SubagentRegistry> registry,
    std::shared_ptr<AgentManager> agent_mgr
) {
    return std::make_shared<SubagentAnnouncer>(registry, agent_mgr);
}

SubagentAnnouncer::SubagentAnnouncer(
    std::shared_ptr<SubagentRegistry> registry,
    std::shared_ptr<AgentManager> agent_mgr
)
    : registry_(registry)
    , agent_manager_(agent_mgr)
{
}

SubagentAnnouncer::~SubagentAnnouncer() {
    stop();
}

void SubagentAnnouncer::start() {
    if (running_.exchange(true)) {
        return; // Already running
    }
    
    worker_thread_ = std::make_unique<std::thread>([this]() {
        worker_loop();
    });
    
    std::cout << "[SubagentAnnouncer] Background delivery worker started" << std::endl;
}

void SubagentAnnouncer::stop() {
    if (!running_.exchange(false)) {
        return; // Already stopped
    }
    
    cv_.notify_all();
    
    if (worker_thread_ && worker_thread_->joinable()) {
        worker_thread_->join();
    }
    
    std::cout << "[SubagentAnnouncer] Background delivery worker stopped" << std::endl;
}

void SubagentAnnouncer::worker_loop() {
    while (running_) {
        try {
            // Get all entries with pending delivery
            std::vector<std::shared_ptr<SubagentEntry>> to_deliver;
            {
                auto entries = registry_->list_active();
                for (const auto& entry : entries) {
                    if (entry->state == SubagentState::Completing ||
                        entry->state == SubagentState::Announcing) {
                        if (!entry->result_delivered) {
                            to_deliver.push_back(entry);
                        }
                    }
                }
            }
            
            // Attempt delivery
            for (const auto& entry : to_deliver) {
                attempt_delivery(entry);
            }
            
            // Sleep for a bit
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(2));
            
        } catch (const std::exception& e) {
            std::cerr << "[SubagentAnnouncer] Worker error: " << e.what() << std::endl;
        }
    }
}

std::string SubagentAnnouncer::build_delivery_message(const SubagentEntry& entry) {
    std::ostringstream ss;
    
    ss << "📊 Subagent Task Completed\n\n";
    ss << "**Subagent**: " << entry.subagent_name << "\n";
    ss << "**Task**: " << entry.description << "\n";
    ss << "**Duration**: " << std::fixed << std::setprecision(1) 
       << entry.age_seconds() << "s\n";
    ss << "**Status**: " << (entry.has_error ? "❌ Failed" : "✅ Success") << "\n\n";
    
    if (entry.has_error) {
        ss << "**Error**:\n```\n" << entry.error_message << "\n```\n\n";
    }
    
    ss << "**Result**:\n" << entry.result_text << "\n";
    
    return ss.str();
}

core::Result<void> SubagentAnnouncer::attempt_delivery(
    std::shared_ptr<SubagentEntry> entry
) {
    // Check retry logic
    if (!should_retry(*entry)) {
        return core::Result<void>::Err("Max retries exceeded");
    }
    
    // Update state
    registry_->update_state(entry->subagent_run_id, SubagentState::Announcing);
    registry_->record_delivery_attempt(entry->subagent_run_id);
    
    // Build message
    std::string message = build_delivery_message(*entry);
    
    // Get requester agent
    // TODO: This requires AgentManager which is not yet implemented
    // For now, just mark as delivered as a stub
    /*
    auto requester_agent = agent_manager_->get_agent(entry->requester_agent_id);
    if (!requester_agent) {
        return core::Result<void>::Err("Requester agent not found");
    }
    
    // Get requester run context
    auto requester_ctx = requester_agent->get_run_context(entry->requester_run_id);
    if (!requester_ctx) {
        return core::Result<void>::Err("Requester run context not found");
    }
    
    // Deliver event to requester
    AgentEvent event;
    event.run_id = entry->requester_run_id;
    event.seq = requester_ctx->next_seq++;
    event.stream = AgentEventStream::Assistant;
    event.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    event.session_key = requester_ctx->session_key;
    event.data = core::JsonValue::object({
        {"type", "subagent_result"},
        {"subagent_run_id", entry->subagent_run_id},
        {"subagent_name", entry->subagent_name},
        {"description", entry->description},
        {"result", entry->result_text},
        {"has_error", entry->has_error},
        {"error_message", entry->error_message}
    });
    */
    
    // Mark as delivered
    registry_->mark_delivered(entry->subagent_run_id);
    
    std::cout << "[SubagentAnnouncer] Delivered result: " 
              << entry->subagent_run_id 
              << " -> " << entry->requester_run_id << std::endl;
    
    return core::Result<void>::Ok();
}

bool SubagentAnnouncer::should_retry(const SubagentEntry& entry) {
    const int MAX_RETRIES = 5;
    
    if (entry.delivery_attempts >= MAX_RETRIES) {
        return false;
    }
    
    // Check retry delay
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    int delay_ms = get_retry_delay_ms(entry.delivery_attempts);
    return (now - entry.last_delivery_attempt) >= delay_ms;
}

int SubagentAnnouncer::get_retry_delay_ms(int attempt) {
    // Exponential backoff: 1s, 2s, 4s, 8s, 16s
    return 1000 * (1 << attempt);
}

core::Result<void> SubagentAnnouncer::deliver_now(const std::string& subagent_run_id) {
    auto entry = registry_->get_entry(subagent_run_id);
    if (!entry) {
        return core::Result<void>::Err("Subagent not found");
    }
    
    return attempt_delivery(entry);
}

// ============================================================================
// SubagentManager Implementation
// ============================================================================

SubagentManager::Ptr SubagentManager::create(std::shared_ptr<AgentManager> agent_mgr) {
    return std::make_shared<SubagentManager>(agent_mgr);
}

SubagentManager::SubagentManager(std::shared_ptr<AgentManager> agent_mgr)
    : agent_manager_(agent_mgr)
{
    registry_ = SubagentRegistry::create();
    spawner_ = SubagentSpawner::create(agent_mgr, registry_);
    announcer_ = SubagentAnnouncer::create(registry_, agent_mgr);
    
    // Start background delivery
    announcer_->start();
}

SubagentManager::~SubagentManager() {
    announcer_->stop();
}

core::Result<std::string> SubagentManager::spawn(
    const std::string& requester_run_id,
    const std::string& requester_agent_id,
    const SubagentSpawnOptions& options
) {
    // Check if can spawn
    // TODO: This requires AgentManager which is not yet implemented
    // For now, skip the check as a stub
    /*
    auto requester_agent = agent_manager_->get_agent(requester_agent_id);
    if (!requester_agent) {
        return core::Result<std::string>::Err("Requester agent not found");
    }
    
    const auto& config = requester_agent->get_config();
    if (!can_spawn(requester_run_id, config.subagents)) {
        return core::Result<std::string>::Err(
            "Cannot spawn subagent: concurrent limit reached"
        );
    }
    */
    
    return spawner_->spawn(requester_run_id, requester_agent_id, options);
}

core::Result<SubagentEntry> SubagentManager::get_status(
    const std::string& subagent_run_id
) {
    auto entry = registry_->get_entry(subagent_run_id);
    if (!entry) {
        return core::Result<SubagentEntry>::Err("Subagent not found");
    }
    return core::Result<SubagentEntry>::Ok(*entry);
}

core::Result<void> SubagentManager::kill(const std::string& subagent_run_id) {
    return registry_->kill_subagent_tree(subagent_run_id);
}

std::vector<SubagentEntry> SubagentManager::list(const std::string& requester_run_id) {
    auto entries = registry_->list_by_requester(requester_run_id);
    
    std::vector<SubagentEntry> result;
    for (const auto& entry_ptr : entries) {
        result.push_back(*entry_ptr);
    }
    
    return result;
}

bool SubagentManager::can_spawn(
    const std::string& requester_run_id,
    const SubagentConfig& config
) {
    if (!config.enabled) {
        return false;
    }
    
    // Check concurrent limit
    auto active_subagents = registry_->list_by_requester(requester_run_id);
    int active_count = 0;
    for (const auto& entry : active_subagents) {
        if (entry->state != SubagentState::Completed &&
            entry->state != SubagentState::Failed &&
            entry->state != SubagentState::Killed) {
            active_count++;
        }
    }
    
    return active_count < config.max_concurrent;
}

} // namespace agents
} // namespace hunterclaw
