#pragma once

#include <filesystem>
#include <string>

namespace protoscope::config::embedded {

// 返回程序所在目录。
// Windows: exe 所在目录
// Linux: /proc/self/exe 所在目录
// macOS: 可执行文件所在目录
std::filesystem::path executableDirectory();

// 确保协议工作区存在。
// rootDir 通常传入：executableDirectory() / "protocols"
// 行为：
// - rootDir 不存在：创建并释放所有内嵌 protocols 文件
// - rootDir 已存在且是目录：补齐缺失的内嵌文件，不覆盖已有文件
// - rootDir 已存在但不是目录：返回 false
bool ensureProtocolWorkspace(const std::filesystem::path& rootDir, std::string& error);

// 确保单个默认协议目录里有 main.lua。
// 这个用于兼容原来的 ConfigStore::ensureDefaultProtocolScript(protocolDir, error)。
bool ensureDefaultProtocolScript(const std::filesystem::path& protocolDir, std::string& error);

bool extractResourceToFile(const char* resourcePath, const std::filesystem::path& outputPath, std::string& error);

} // namespace protoscope::config::embedded
