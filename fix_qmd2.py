#!/usr/bin/env python3

# Read qmd_manager.cc
with open('src/memory/qmd_manager.cc', 'r') as f:
    content = f.read()

# Fix params.reason -> params->reason, params.force -> params->force
content = content.replace('params.reason', 'params->reason')
content = content.replace('params.force', 'params->force')

# Fix remaining QmdCommandOptions{5000}
content = content.replace(
    'run_qmd({"query", "test", "--limit", "1"}, QmdCommandOptions{5000});',
    '''QmdCommandOptions opts{5000, {}};
        run_qmd({"query", "test", "--limit", "1"}, &opts);'''
)

# Fix run_qmd signature - const QmdCommandOptions& opts -> const QmdCommandOptions* opts
content = content.replace(
    'QmdMemoryManager::QmdCommandResult QmdMemoryManager::run_qmd(\n    const std::vector<std::string>& args,\n    const QmdCommandOptions& opts\n)',
    'QmdMemoryManager::QmdCommandResult QmdMemoryManager::run_qmd(\n    const std::vector<std::string>& args,\n    const QmdCommandOptions* opts\n)'
)

# Fix opts reference to pointer inside run_qmd
lines = content.split('\n')
new_lines = []
in_run_qmd = False
for i, line in enumerate(lines):
    if 'QmdMemoryManager::run_qmd(' in line:
        in_run_qmd = True
    elif in_run_qmd and line.strip().startswith('}') and 'run_qmd' in '\n'.join(lines[max(0,i-50):i]):
        in_run_qmd = False
    
    if in_run_qmd:
        if 'opts.timeout_ms' in line:
            line = line.replace('opts.timeout_ms', 'opts->timeout_ms')
        elif 'opts.env' in line:
            line = line.replace('opts.env', 'opts->env')
        elif 'if (opts.timeout_ms > 0)' in line:
            line = line.replace('if (opts.timeout_ms', 'if (opts && opts->timeout_ms')
    
    new_lines.append(line)

content = '\n'.join(new_lines)

# Write back
with open('src/memory/qmd_manager.cc', 'w') as f:
    f.write(content)

print("Fixed qmd_manager.cc")
