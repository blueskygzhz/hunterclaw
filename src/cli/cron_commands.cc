#include "hunterclaw/cli/cron_commands.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/core/json.h"
#include <iostream>
#include <iomanip>

namespace hunterclaw {
namespace cli {

// ============================================================================
// CronListCommand
// ============================================================================

CronListCommand::CronListCommand(std::shared_ptr<cron::CronManager> manager)
    : manager_(manager) {}

void CronListCommand::configure(CLI::App& app) {
    app.add_flag("--json", show_json_, "Output as JSON");
    app.add_flag("--all,-a", show_disabled_, "Show disabled jobs");
}

core::Result<int> CronListCommand::execute(const CommandContext& ctx) {
    auto jobs_result = manager_->list_jobs();
    if (!jobs_result.success) {
        std::cerr << "Error: " << jobs_result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    auto jobs = jobs_result.value;
    
    // Filter disabled if needed
    if (!show_disabled_) {
        jobs.erase(
            std::remove_if(jobs.begin(), jobs.end(),
                [](const auto& job) { return !job.enabled; }),
            jobs.end()
        );
    }
    
    if (show_json_) {
        // JSON output
        core::JsonArray json_jobs;
        for (const auto& job : jobs) {
            core::JsonObject obj;
            obj["name"] = core::JsonValue(job.name);
            obj["schedule"] = core::JsonValue(job.schedule);
            obj["command"] = core::JsonValue(job.command);
            obj["enabled"] = core::JsonValue(job.enabled);
            obj["next_run"] = core::JsonValue(job.next_run);
            json_jobs.push_back(core::JsonValue(obj));
        }
        std::cout << core::JsonWriter::write(core::JsonValue(json_jobs)) << std::endl;
    } else {
        // Table output
        if (jobs.empty()) {
            std::cout << "No cron jobs found." << std::endl;
            return core::Result<int>::ok(0);
        }
        
        std::cout << std::left
                  << std::setw(20) << "NAME"
                  << std::setw(15) << "SCHEDULE"
                  << std::setw(30) << "COMMAND"
                  << std::setw(10) << "ENABLED"
                  << "NEXT RUN"
                  << std::endl;
        std::cout << std::string(90, '-') << std::endl;
        
        for (const auto& job : jobs) {
            std::cout << std::left
                      << std::setw(20) << job.name
                      << std::setw(15) << job.schedule
                      << std::setw(30) << (job.command.length() > 27 
                            ? job.command.substr(0, 27) + "..." 
                            : job.command)
                      << std::setw(10) << (job.enabled ? "yes" : "no")
                      << job.next_run
                      << std::endl;
        }
    }
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// CronAddCommand
// ============================================================================

CronAddCommand::CronAddCommand(std::shared_ptr<cron::CronManager> manager)
    : manager_(manager) {}

void CronAddCommand::configure(CLI::App& app) {
    app.add_option("name", job_name_, "Job name")
        ->required();
    app.add_option("schedule", schedule_, "Cron schedule (e.g. '0 9 * * *')")
        ->required();
    app.add_option("command", command_, "Command to execute")
        ->required();
    app.add_option("--description,-d", description_, "Job description");
}

core::Result<int> CronAddCommand::execute(const CommandContext& ctx) {
    cron::CronJob job;
    job.name = job_name_;
    job.schedule = schedule_;
    job.command = command_;
    job.description = description_;
    job.enabled = true;
    
    auto result = manager_->add_job(job);
    if (!result.success) {
        std::cerr << "Error: " << result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    std::cout << "✓ Cron job '" << job_name_ << "' added successfully" << std::endl;
    std::cout << "  Schedule: " << schedule_ << std::endl;
    std::cout << "  Command:  " << command_ << std::endl;
    
    // Show next run time
    auto next_run = manager_->get_next_run_time(job_name_);
    if (next_run.success) {
        std::cout << "  Next run: " << next_run.value << std::endl;
    }
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// CronRemoveCommand
// ============================================================================

CronRemoveCommand::CronRemoveCommand(std::shared_ptr<cron::CronManager> manager)
    : manager_(manager) {}

void CronRemoveCommand::configure(CLI::App& app) {
    app.add_option("name", job_name_, "Job name")
        ->required();
    app.add_flag("--force,-f", force_, "Force removal without confirmation");
}

core::Result<int> CronRemoveCommand::execute(const CommandContext& ctx) {
    // Confirm removal
    if (!force_) {
        std::cout << "Remove cron job '" << job_name_ << "'? [y/N] ";
        std::string response;
        std::getline(std::cin, response);
        
        if (response != "y" && response != "Y") {
            std::cout << "Cancelled." << std::endl;
            return core::Result<int>::ok(0);
        }
    }
    
    auto result = manager_->remove_job(job_name_);
    if (!result.success) {
        std::cerr << "Error: " << result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    std::cout << "✓ Cron job '" << job_name_ << "' removed" << std::endl;
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// CronInfoCommand
// ============================================================================

CronInfoCommand::CronInfoCommand(std::shared_ptr<cron::CronManager> manager)
    : manager_(manager) {}

void CronInfoCommand::configure(CLI::App& app) {
    app.add_option("name", job_name_, "Job name")
        ->required();
    app.add_flag("--json", show_json_, "Output as JSON");
}

core::Result<int> CronInfoCommand::execute(const CommandContext& ctx) {
    auto job_result = manager_->get_job(job_name_);
    if (!job_result.success) {
        std::cerr << "Error: " << job_result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    auto& job = job_result.value;
    
    if (show_json_) {
        // JSON output
        core::JsonObject obj;
        obj["name"] = core::JsonValue(job.name);
        obj["schedule"] = core::JsonValue(job.schedule);
        obj["command"] = core::JsonValue(job.command);
        obj["description"] = core::JsonValue(job.description);
        obj["enabled"] = core::JsonValue(job.enabled);
        obj["created_at"] = core::JsonValue(job.created_at);
        obj["last_run"] = core::JsonValue(job.last_run);
        obj["next_run"] = core::JsonValue(job.next_run);
        obj["run_count"] = core::JsonValue(static_cast<int64_t>(job.run_count));
        
        std::cout << core::JsonWriter::write(core::JsonValue(obj), true) << std::endl;
    } else {
        // Human-readable output
        std::cout << "Cron Job: " << job.name << std::endl;
        std::cout << "  Schedule:    " << job.schedule << std::endl;
        std::cout << "  Command:     " << job.command << std::endl;
        std::cout << "  Description: " << job.description << std::endl;
        std::cout << "  Enabled:     " << (job.enabled ? "yes" : "no") << std::endl;
        std::cout << "  Created:     " << job.created_at << std::endl;
        std::cout << "  Last run:    " << job.last_run << std::endl;
        std::cout << "  Next run:    " << job.next_run << std::endl;
        std::cout << "  Run count:   " << job.run_count << std::endl;
    }
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// CronEnableCommand
// ============================================================================

CronEnableCommand::CronEnableCommand(std::shared_ptr<cron::CronManager> manager)
    : manager_(manager) {}

void CronEnableCommand::configure(CLI::App& app) {
    app.add_option("name", job_name_, "Job name")
        ->required();
}

core::Result<int> CronEnableCommand::execute(const CommandContext& ctx) {
    auto result = manager_->enable_job(job_name_);
    if (!result.success) {
        std::cerr << "Error: " << result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    std::cout << "✓ Cron job '" << job_name_ << "' enabled" << std::endl;
    
    // Show next run time
    auto next_run = manager_->get_next_run_time(job_name_);
    if (next_run.success) {
        std::cout << "  Next run: " << next_run.value << std::endl;
    }
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// CronDisableCommand
// ============================================================================

CronDisableCommand::CronDisableCommand(std::shared_ptr<cron::CronManager> manager)
    : manager_(manager) {}

void CronDisableCommand::configure(CLI::App& app) {
    app.add_option("name", job_name_, "Job name")
        ->required();
}

core::Result<int> CronDisableCommand::execute(const CommandContext& ctx) {
    auto result = manager_->disable_job(job_name_);
    if (!result.success) {
        std::cerr << "Error: " << result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    std::cout << "✓ Cron job '" << job_name_ << "' disabled" << std::endl;
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// CronRunCommand
// ============================================================================

CronRunCommand::CronRunCommand(std::shared_ptr<cron::CronManager> manager)
    : manager_(manager) {}

void CronRunCommand::configure(CLI::App& app) {
    app.add_option("name", job_name_, "Job name")
        ->required();
}

core::Result<int> CronRunCommand::execute(const CommandContext& ctx) {
    std::cout << "Running cron job '" << job_name_ << "'..." << std::endl;
    
    auto result = manager_->run_job_now(job_name_);
    if (!result.success) {
        std::cerr << "Error: " << result.error_message << std::endl;
        return core::Result<int>::ok(1);
    }
    
    std::cout << "✓ Job executed successfully" << std::endl;
    
    return core::Result<int>::ok(0);
}

// ============================================================================
// Registration
// ============================================================================

void register_cron_commands(
    CLI::App& app,
    std::shared_ptr<cron::CronManager> manager
) {
    // Register all cron commands
    auto cron_list = std::make_shared<CronListCommand>(manager);
    auto cron_add = std::make_shared<CronAddCommand>(manager);
    auto cron_remove = std::make_shared<CronRemoveCommand>(manager);
    auto cron_info = std::make_shared<CronInfoCommand>(manager);
    auto cron_enable = std::make_shared<CronEnableCommand>(manager);
    auto cron_disable = std::make_shared<CronDisableCommand>(manager);
    auto cron_run = std::make_shared<CronRunCommand>(manager);
    
    // Configure subcommands
    auto list_cmd = app.add_subcommand("cron:list", cron_list->description());
    cron_list->configure(*list_cmd);
    list_cmd->callback([cron_list]() {
        CommandContext ctx;
        cron_list->execute(ctx);
    });
    
    auto add_cmd = app.add_subcommand("cron:add", cron_add->description());
    cron_add->configure(*add_cmd);
    add_cmd->callback([cron_add]() {
        CommandContext ctx;
        cron_add->execute(ctx);
    });
    
    auto remove_cmd = app.add_subcommand("cron:remove", cron_remove->description());
    cron_remove->configure(*remove_cmd);
    remove_cmd->callback([cron_remove]() {
        CommandContext ctx;
        cron_remove->execute(ctx);
    });
    
    auto info_cmd = app.add_subcommand("cron:info", cron_info->description());
    cron_info->configure(*info_cmd);
    info_cmd->callback([cron_info]() {
        CommandContext ctx;
        cron_info->execute(ctx);
    });
    
    auto enable_cmd = app.add_subcommand("cron:enable", cron_enable->description());
    cron_enable->configure(*enable_cmd);
    enable_cmd->callback([cron_enable]() {
        CommandContext ctx;
        cron_enable->execute(ctx);
    });
    
    auto disable_cmd = app.add_subcommand("cron:disable", cron_disable->description());
    cron_disable->configure(*disable_cmd);
    disable_cmd->callback([cron_disable]() {
        CommandContext ctx;
        cron_disable->execute(ctx);
    });
    
    auto run_cmd = app.add_subcommand("cron:run", cron_run->description());
    cron_run->configure(*run_cmd);
    run_cmd->callback([cron_run]() {
        CommandContext ctx;
        cron_run->execute(ctx);
    });
}

} // namespace cli
} // namespace hunterclaw
