#pragma once
#include <filesystem>
#include <string>
namespace photonstack {
// Streaming SHA-256 of the exact source bytes. Throws on read failure.
std::string fileSHA256(const std::filesystem::path& path);
} // namespace photonstack
