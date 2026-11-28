#pragma once
#include <filesystem>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
namespace photonstack {
struct FileDigestSnapshot {
    std::string sha, prefix;
    std::uint64_t size = 0;
};
// Hash each byte once, including FITS padding and extensions. Tiles may be
// revisited. The final digest also checks the open object and its pathname.
class DigestingFileReader {
  public:
    explicit DigestingFileReader(const std::filesystem::path& path, bool followSymlinks = false);
    ~DigestingFileReader();
    std::uint64_t size() const;
    void readAt(std::uint64_t offset, std::span<unsigned char> bytes);
    std::string finish();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Capture header bytes and the digest from the same open regular file.
FileDigestSnapshot digestRegularFile(const std::filesystem::path& path, std::size_t prefixBytes);
std::string bytesSHA256(std::span<const unsigned char> bytes);
// Streaming SHA-256 of the exact source bytes. Throws on read failure.
std::string fileSHA256(const std::filesystem::path& path);
} // namespace photonstack
