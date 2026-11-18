#pragma once
#include <filesystem>
#include <string>
#include <vector>
// CLI-only synchronous process scope. No shell, bounded wall time and log,
// cancellation terminates the whole child process group.
void runLocalProcess(const std::filesystem::path& executable, const std::vector<std::string>& args,
                     const std::filesystem::path& directory, unsigned timeoutSeconds);
