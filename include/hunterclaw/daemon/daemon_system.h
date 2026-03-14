#ifndef HUNTERCLAW_DAEMON_SYSTEM_H
#define HUNTERCLAW_DAEMON_SYSTEM_H

#include <string>
#include <optional>
#include <vector>
#include <unordered_map>
#include <functional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace daemon {

using json = nlohmann::json;

// ============================================================================
// 1. Service Types - Gateway service types and configuration
// ============================================================================

using GatewayServiceEnv = std::unordered_map<std::string, std::string>;

struct GatewayServiceInstallArgs {
    GatewayServiceEnv env;
    std::vector<std::string> program_arguments;
    std::optional<std::string> working_directory;
    std::optional<GatewayServiceEnv> environment;
    std::optional<std::string> description;
};

struct GatewayServiceManageArgs {
    GatewayServiceEnv env;
};

struct GatewayServiceControlArgs {
    std::optional<GatewayServiceEnv> env;
};

struct GatewayServiceRestartResult {
    enum class Outcome { Completed, Scheduled };
    Outcome outcome;
};

struct GatewayServiceCommandConfig {
    std::vector<std::string> program_arguments;
    std::optional<std::string> working_directory;
    std::unordered_map<std::string, std::string> environment;
    std::unordered_map<std::string, std::string> environment_value_sources; // "inline" or "file"
    std::optional<std::string> source_path;
};

// ============================================================================
// 2. Service Runtime - Runtime information
// ============================================================================

struct GatewayServiceRuntime {
    std::optional<int> pid;
    std::optional<std::string> status;
    std::optional<std::string> uptime;
    std::optional<std::string> last_exit_code;
    std::optional<std::string> executable_path;
    json extra_info;
};

// ============================================================================
// 3. Gateway Service Interface - Platform-agnostic service API
// ============================================================================

class GatewayService {
public:
    virtual ~GatewayService() = default;
    
    // Service metadata
    virtual std::string label() const = 0;
    virtual std::string loaded_text() const = 0;
    virtual std::string not_loaded_text() const = 0;
    
    // Service operations
    virtual bool install(const GatewayServiceInstallArgs& args) = 0;
    virtual bool uninstall(const GatewayServiceManageArgs& args) = 0;
    virtual bool stop(const GatewayServiceControlArgs& args) = 0;
    virtual GatewayServiceRestartResult restart(const GatewayServiceControlArgs& args) = 0;
    
    // Service status
    virtual bool is_loaded(const GatewayServiceEnv& env) = 0;
    virtual std::optional<GatewayServiceCommandConfig> read_command(const GatewayServiceEnv& env) = 0;
    virtual GatewayServiceRuntime read_runtime(const GatewayServiceEnv& env) = 0;
};

// ============================================================================
// 4. Systemd Service - Linux systemd service manager
// ============================================================================

class SystemdService : public GatewayService {
public:
    std::string label() const override { return "systemd"; }
    std::string loaded_text() const override { return "enabled"; }
    std::string not_loaded_text() const override { return "disabled"; }
    
    bool install(const GatewayServiceInstallArgs& args) override;
    bool uninstall(const GatewayServiceManageArgs& args) override;
    bool stop(const GatewayServiceControlArgs& args) override;
    GatewayServiceRestartResult restart(const GatewayServiceControlArgs& args) override;
    
    bool is_loaded(const GatewayServiceEnv& env) override;
    std::optional<GatewayServiceCommandConfig> read_command(const GatewayServiceEnv& env) override;
    GatewayServiceRuntime read_runtime(const GatewayServiceEnv& env) override;
    
private:
    std::string resolve_systemd_unit_path(const GatewayServiceEnv& env);
    std::string build_systemd_unit(const GatewayServiceInstallArgs& args);
    bool execute_systemctl(const std::vector<std::string>& args);
};

// ============================================================================
// 5. LaunchAgent Service - macOS launchd service manager
// ============================================================================

class LaunchAgentService : public GatewayService {
public:
    std::string label() const override { return "LaunchAgent"; }
    std::string loaded_text() const override { return "loaded"; }
    std::string not_loaded_text() const override { return "not loaded"; }
    
    bool install(const GatewayServiceInstallArgs& args) override;
    bool uninstall(const GatewayServiceManageArgs& args) override;
    bool stop(const GatewayServiceControlArgs& args) override;
    GatewayServiceRestartResult restart(const GatewayServiceControlArgs& args) override;
    
    bool is_loaded(const GatewayServiceEnv& env) override;
    std::optional<GatewayServiceCommandConfig> read_command(const GatewayServiceEnv& env) override;
    GatewayServiceRuntime read_runtime(const GatewayServiceEnv& env) override;
    
private:
    std::string resolve_launch_agent_plist_path(const GatewayServiceEnv& env);
    std::string build_launch_agent_plist(const GatewayServiceInstallArgs& args);
    bool execute_launchctl(const std::vector<std::string>& args);
};

// ============================================================================
// 6. Scheduled Task Service - Windows Task Scheduler
// ============================================================================

class ScheduledTaskService : public GatewayService {
public:
    std::string label() const override { return "Scheduled Task"; }
    std::string loaded_text() const override { return "registered"; }
    std::string not_loaded_text() const override { return "missing"; }
    
    bool install(const GatewayServiceInstallArgs& args) override;
    bool uninstall(const GatewayServiceManageArgs& args) override;
    bool stop(const GatewayServiceControlArgs& args) override;
    GatewayServiceRestartResult restart(const GatewayServiceControlArgs& args) override;
    
    bool is_loaded(const GatewayServiceEnv& env) override;
    std::optional<GatewayServiceCommandConfig> read_command(const GatewayServiceEnv& env) override;
    GatewayServiceRuntime read_runtime(const GatewayServiceEnv& env) override;
    
private:
    std::string resolve_task_name(const GatewayServiceEnv& env);
    bool execute_schtasks(const std::vector<std::string>& args);
};

// ============================================================================
// 7. Service Audit - Service health and diagnostics
// ============================================================================

struct ServiceAuditResult {
    bool healthy = true;
    std::vector<std::string> issues;
    std::vector<std::string> warnings;
    std::optional<GatewayServiceRuntime> runtime;
    std::optional<GatewayServiceCommandConfig> config;
};

class ServiceAudit {
public:
    // Audit service health
    static ServiceAuditResult audit_service(GatewayService& service,
                                           const GatewayServiceEnv& env);
    
    // Check if service is running
    static bool is_running(GatewayService& service, const GatewayServiceEnv& env);
    
    // Check if service is configured correctly
    static bool is_configured_correctly(GatewayService& service,
                                       const GatewayServiceEnv& env,
                                       const GatewayServiceCommandConfig& expected);
    
    // Get service status summary
    static std::string get_status_summary(const ServiceAuditResult& result);
};

// ============================================================================
// 8. Runtime Binary - Executable path resolution
// ============================================================================

class RuntimeBinary {
public:
    // Resolve runtime binary path
    static std::optional<std::string> resolve_runtime_binary(const GatewayServiceEnv& env);
    
    // Get Node.js binary path
    static std::optional<std::string> get_node_binary();
    
    // Get Bun binary path
    static std::optional<std::string> get_bun_binary();
    
    // Get Deno binary path
    static std::optional<std::string> get_deno_binary();
    
    // Detect runtime type
    enum class RuntimeType { Node, Bun, Deno, Unknown };
    static RuntimeType detect_runtime_type(const std::string& binary_path);
};

// ============================================================================
// 9. Program Args - Command-line argument handling
// ============================================================================

class ProgramArgs {
public:
    // Parse program arguments
    static std::vector<std::string> parse_program_args(const std::string& command);
    
    // Build program arguments
    static std::vector<std::string> build_program_args(const GatewayServiceEnv& env,
                                                       const std::optional<std::string>& profile);
    
    // Quote argument if needed
    static std::string quote_arg(const std::string& arg);
    
    // Join arguments into command string
    static std::string join_args(const std::vector<std::string>& args);
};

// ============================================================================
// 10. Service Env - Environment variable management
// ============================================================================

class ServiceEnv {
public:
    // Resolve service environment
    static GatewayServiceEnv resolve_service_env(const GatewayServiceEnv& base_env,
                                                 const std::optional<GatewayServiceEnv>& override_env);
    
    // Read environment from file
    static std::optional<GatewayServiceEnv> read_env_file(const std::string& path);
    
    // Write environment to file
    static bool write_env_file(const std::string& path, const GatewayServiceEnv& env);
    
    // Sanitize environment for service
    static GatewayServiceEnv sanitize_env(const GatewayServiceEnv& env);
};

// ============================================================================
// 11. Paths - Service-related path resolution
// ============================================================================

class ServicePaths {
public:
    // Resolve home directory
    static std::string resolve_home_dir(const GatewayServiceEnv& env);
    
    // Resolve config directory
    static std::string resolve_config_dir(const GatewayServiceEnv& env);
    
    // Resolve state directory
    static std::string resolve_state_dir(const GatewayServiceEnv& env);
    
    // Resolve log directory
    static std::string resolve_log_dir(const GatewayServiceEnv& env);
    
    // Convert to POSIX path
    static std::string to_posix_path(const std::string& path);
};

// ============================================================================
// 12. Constants - Service constants and defaults
// ============================================================================

class ServiceConstants {
public:
    // Service names
    static std::string default_service_name();
    static std::string resolve_service_name(const std::optional<std::string>& profile);
    
    // Service description
    static std::string default_service_description();
    static std::string resolve_service_description(const std::optional<std::string>& profile);
    
    // Default paths
    static std::string default_systemd_unit_dir();
    static std::string default_launchd_plist_dir();
    static std::string default_task_folder();
};

// ============================================================================
// Daemon System Manager
// ============================================================================

class DaemonSystem {
public:
    DaemonSystem();
    ~DaemonSystem();
    
    // Initialize daemon system
    bool initialize();
    void shutdown();
    
    // Get platform-specific service
    GatewayService* get_service();
    
    // Service operations (convenience wrappers)
    bool install_service(const GatewayServiceInstallArgs& args);
    bool uninstall_service(const GatewayServiceManageArgs& args);
    bool stop_service(const GatewayServiceControlArgs& args);
    GatewayServiceRestartResult restart_service(const GatewayServiceControlArgs& args);
    bool is_service_loaded(const GatewayServiceEnv& env);
    
    // Service status
    std::optional<GatewayServiceCommandConfig> read_service_command(const GatewayServiceEnv& env);
    GatewayServiceRuntime read_service_runtime(const GatewayServiceEnv& env);
    ServiceAuditResult audit_service(const GatewayServiceEnv& env);
    
    // Platform detection
    enum class Platform { Linux, Darwin, Windows, Unknown };
    static Platform detect_platform();
    static std::string platform_name(Platform platform);

private:
    std::unique_ptr<GatewayService> service_;
    bool initialized_ = false;
    
    void create_platform_service();
};

} // namespace daemon
} // namespace hunterclaw

#endif // HUNTERCLAW_DAEMON_SYSTEM_H
