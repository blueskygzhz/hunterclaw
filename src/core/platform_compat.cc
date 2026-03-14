#include "hunterclaw/core/platform_compat.h"
#include <algorithm>
#include <sstream>

#ifdef _WIN32
  #include <windows.h>
  #define PLATFORM_WINDOWS
#elif defined(__APPLE__)
  #define PLATFORM_MACOS
#elif defined(__linux__)
  #define PLATFORM_LINUX
#else
  #define PLATFORM_UNKNOWN
#endif

namespace hunterclaw {

Platform PlatformCompat::GetCurrentPlatform() {
#ifdef PLATFORM_WINDOWS
  return Platform::Windows;
#elif defined(PLATFORM_MACOS)
  return Platform::MacOS;
#elif defined(PLATFORM_LINUX)
  return Platform::Linux;
#else
  return Platform::Unknown;
#endif
}

ShellType PlatformCompat::GetDefaultShell(Platform platform) {
  if (platform == Platform::Unknown) {
    platform = GetCurrentPlatform();
  }
  
  if (platform == Platform::Windows) {
    return ShellType::Cmd;
  } else {
    return ShellType::Bash;
  }
}

bool PlatformCompat::IsWindows(Platform platform) {
  if (platform == Platform::Unknown) {
    platform = GetCurrentPlatform();
  }
  return platform == Platform::Windows;
}

NormalizedCommand PlatformCompat::NormalizeCommand(
    const std::string& program,
    const std::vector<std::string>& args,
    const NormalizeOptions& options) {
  NormalizedCommand result;
  result.program = program;
  result.args = args;
  
  // 确定目标平台
  Platform platform = options.platform.value_or(GetCurrentPlatform());
  ShellType shell = options.shell.value_or(GetDefaultShell(platform));
  
  result.shell = shell;
  
  // 处理npm/npx命令
  if (options.handle_npm) {
    if (program == "npm" || program == "npx") {
      result.program = NormalizeNpmCommand(program, platform);
    }
  }
  
  // 检查是否需要Shell包装
  if (options.auto_shell_wrap && NeedsShell(result.program)) {
    result.needs_shell = true;
    
    // 在Windows上自动包装Shell命令
    if (IsWindows(platform)) {
      auto wrapped = WrapInShell(result.program, result.args, shell);
      result.program = wrapped.program;
      result.args = wrapped.args;
      result.needs_shell = false;  // 已经包装了
    }
  }
  
  return result;
}

std::string PlatformCompat::NormalizeNpmCommand(
    const std::string& program,
    Platform platform) {
  if (platform == Platform::Unknown) {
    platform = GetCurrentPlatform();
  }
  
  // Windows: npm -> npm.cmd, npx -> npx.cmd
  if (IsWindows(platform)) {
    if (program == "npm") {
      return "npm.cmd";
    } else if (program == "npx") {
      return "npx.cmd";
    }
  }
  
  return program;
}

NormalizedCommand PlatformCompat::WrapInShell(
    const std::string& program,
    const std::vector<std::string>& args,
    ShellType shell) {
  if (shell == ShellType::Unknown) {
    shell = GetDefaultShell();
  }
  
  NormalizedCommand result;
  result.shell = shell;
  result.needs_shell = false;  // 已经包装
  
  // 构建完整命令行
  std::ostringstream cmd;
  cmd << program;
  for (const auto& arg : args) {
    cmd << " " << EscapeShellArg(arg, shell);
  }
  
  std::string full_cmd = cmd.str();
  
  // 根据Shell类型包装
  if (shell == ShellType::Cmd) {
    // Windows cmd.exe
    result.program = "cmd.exe";
    result.args = {"/c", full_cmd};
  } else if (shell == ShellType::PowerShell) {
    // Windows PowerShell
    result.program = "powershell.exe";
    result.args = {"-Command", full_cmd};
  } else if (shell == ShellType::Bash) {
    // Unix bash
    result.program = "/bin/bash";
    result.args = {"-c", full_cmd};
  } else if (shell == ShellType::Sh) {
    // Unix sh
    result.program = "/bin/sh";
    result.args = {"-c", full_cmd};
  } else {
    // 默认不包装
    result.program = program;
    result.args = args;
  }
  
  return result;
}

std::string PlatformCompat::EscapeShellArg(
    const std::string& arg,
    ShellType shell) {
  if (shell == ShellType::Unknown) {
    shell = GetDefaultShell();
  }
  
  // 如果参数不包含特殊字符,直接返回
  bool needs_escape = false;
  for (char c : arg) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '\'' ||
        c == '$' || c == '\\' || c == '&' || c == '|' || c == ';' ||
        c == '<' || c == '>' || c == '(' || c == ')' || c == '`') {
      needs_escape = true;
      break;
    }
  }
  
  if (!needs_escape) {
    return arg;
  }
  
  // Windows cmd.exe: 双引号包裹,内部双引号转义为 \"
  if (shell == ShellType::Cmd) {
    std::string result = "\"";
    for (char c : arg) {
      if (c == '"') {
        result += "\\\"";
      } else {
        result += c;
      }
    }
    result += "\"";
    return result;
  }
  
  // Unix shells: 单引号包裹,内部单引号转义为 '\''
  std::string result = "'";
  for (char c : arg) {
    if (c == '\'') {
      result += "'\\''";
    } else {
      result += c;
    }
  }
  result += "'";
  return result;
}

bool PlatformCompat::NeedsShell(const std::string& program) {
  // 检查是否为Shell内置命令或包含Shell特殊字符
  
  // Shell内置命令列表(部分)
  static const std::vector<std::string> shell_builtins = {
    "cd", "echo", "set", "export", "source", ".", "alias",
    "unalias", "pwd", "pushd", "popd", "dirs", "jobs", "fg", "bg"
  };
  
  for (const auto& builtin : shell_builtins) {
    if (program == builtin) {
      return true;
    }
  }
  
  // 检查是否包含Shell特殊字符(管道,重定向等)
  for (char c : program) {
    if (c == '|' || c == '&' || c == ';' || c == '<' || c == '>' ||
        c == '(' || c == ')' || c == '$' || c == '`') {
      return true;
    }
  }
  
  return false;
}

std::string PlatformCompat::PlatformToString(Platform platform) {
  switch (platform) {
    case Platform::Windows: return "Windows";
    case Platform::Linux: return "Linux";
    case Platform::MacOS: return "MacOS";
    default: return "Unknown";
  }
}

std::string PlatformCompat::ShellTypeToString(ShellType shell) {
  switch (shell) {
    case ShellType::Cmd: return "cmd.exe";
    case ShellType::PowerShell: return "powershell.exe";
    case ShellType::Bash: return "bash";
    case ShellType::Sh: return "sh";
    default: return "unknown";
  }
}

}  // namespace hunterclaw
