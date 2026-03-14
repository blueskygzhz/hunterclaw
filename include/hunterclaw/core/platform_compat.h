#pragma once

#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {

/**
 * 平台信息
 */
enum class Platform {
  Windows,
  Linux,
  MacOS,
  Unknown
};

/**
 * Shell 类型
 */
enum class ShellType {
  Cmd,        // Windows cmd.exe
  PowerShell, // Windows PowerShell
  Bash,       // Unix bash
  Sh,         // Unix sh
  Unknown
};

/**
 * 命令规范化选项
 */
struct NormalizeOptions {
  // 目标平台(空表示使用当前平台)
  std::optional<Platform> platform;
  
  // 目标Shell(空表示使用默认Shell)
  std::optional<ShellType> shell;
  
  // 是否处理npm/npx命令
  bool handle_npm = true;
  
  // 是否自动添加Shell包装
  bool auto_shell_wrap = true;
};

/**
 * 规范化后的命令
 */
struct NormalizedCommand {
  // 规范化后的程序路径
  std::string program;
  
  // 规范化后的参数列表
  std::vector<std::string> args;
  
  // 是否需要Shell包装
  bool needs_shell = false;
  
  // 使用的Shell类型
  ShellType shell = ShellType::Unknown;
};

/**
 * PlatformCompat
 * 
 * 功能对齐 OpenClaw process/platform-compat.ts:
 * - Windows 命令规范化(cmd.exe /c)
 * - npm/npx 跨平台处理(.cmd/.bat后缀)
 * - Shell 命令解析
 * 
 * OpenClaw 对应:
 * - normalizeCommand(cmd, args): {program, args}
 * - isWindows(): boolean
 * - wrapInShell(cmd): string[]
 */
class PlatformCompat {
 public:
  /**
   * 获取当前平台
   */
  static Platform GetCurrentPlatform();

  /**
   * 获取默认Shell
   */
  static ShellType GetDefaultShell(Platform platform = Platform::Unknown);

  /**
   * 检查是否为Windows平台
   */
  static bool IsWindows(Platform platform = Platform::Unknown);

  /**
   * 规范化命令
   * @param program 程序路径
   * @param args 参数列表
   * @param options 规范化选项
   * @return 规范化后的命令
   */
  static NormalizedCommand NormalizeCommand(
      const std::string& program,
      const std::vector<std::string>& args,
      const NormalizeOptions& options = {});

  /**
   * 规范化npm/npx命令
   * @param program 程序名(npm/npx)
   * @param platform 目标平台
   * @return 规范化后的程序路径
   */
  static std::string NormalizeNpmCommand(
      const std::string& program,
      Platform platform = Platform::Unknown);

  /**
   * 将命令包装到Shell中
   * @param program 程序路径
   * @param args 参数列表
   * @param shell Shell类型
   * @return 包装后的命令(program + args)
   */
  static NormalizedCommand WrapInShell(
      const std::string& program,
      const std::vector<std::string>& args,
      ShellType shell = ShellType::Unknown);

  /**
   * 转义Shell参数
   * @param arg 参数
   * @param shell Shell类型
   * @return 转义后的参数
   */
  static std::string EscapeShellArg(
      const std::string& arg,
      ShellType shell = ShellType::Unknown);

  /**
   * 检查命令是否需要Shell
   * @param program 程序路径
   * @return 是否需要Shell
   */
  static bool NeedsShell(const std::string& program);

  /**
   * 平台名称转字符串
   */
  static std::string PlatformToString(Platform platform);

  /**
   * Shell类型转字符串
   */
  static std::string ShellTypeToString(ShellType shell);
};

}  // namespace hunterclaw
