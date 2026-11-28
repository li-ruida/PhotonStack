#include "photonstack/FileDigest.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#define COMMON_DIGEST_FOR_OPENSSL
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif

namespace photonstack {
namespace {
class SHA256 {
#ifdef __APPLE__
    CC_SHA256_CTX ctx;
#else
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
#endif
  public:
    SHA256() {
#ifdef __APPLE__
        CC_SHA256_Init(&ctx);
#else
        if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) throw std::runtime_error("Cannot initialize SHA-256");
#endif
    }
    void update(std::span<const unsigned char> bytes) {
#ifdef __APPLE__
        while (!bytes.empty()) {
            const auto n = std::min<std::size_t>(bytes.size(), UINT32_MAX);
            CC_SHA256_Update(&ctx, bytes.data(), static_cast<CC_LONG>(n));
            bytes = bytes.subspan(n);
        }
#else
        if (EVP_DigestUpdate(ctx.get(), bytes.data(), bytes.size()) != 1) throw std::runtime_error("Cannot update SHA-256");
#endif
    }
    std::string finish() {
        std::array<unsigned char, 32> digest{};
#ifdef __APPLE__
        CC_SHA256_Final(digest.data(), &ctx);
#else
        unsigned size = 0;
        if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &size) != 1 || size != digest.size())
            throw std::runtime_error("Cannot finalize SHA-256");
#endif
        std::string text;
        constexpr char hex[] = "0123456789abcdef";
        for (const auto v : digest) { text += hex[v >> 4]; text += hex[v & 15]; }
        return text;
    }
};
bool same(const struct stat& a, const struct stat& b) {
    if (a.st_dev != b.st_dev || a.st_ino != b.st_ino || a.st_size != b.st_size) return false;
#ifdef __APPLE__
    return a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
           a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
#else
    return a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
#endif
}
} // namespace

struct DigestingFileReader::Impl {
    std::filesystem::path path;
    bool follow;
    SHA256 hash;
    int fd = -1;
    struct stat before{};
    std::uint64_t hashed = 0;
    std::string digest;
    Impl(const std::filesystem::path& path, bool follow) : path(path), follow(follow) {}
    ~Impl() { if (fd >= 0) close(fd); }

    void readExact(std::uint64_t offset, std::span<unsigned char> bytes) const {
        while (!bytes.empty()) {
            const auto n = pread(fd, bytes.data(), std::min<std::size_t>(bytes.size(), 4U << 20), offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("Cannot read source for SHA-256: " + path.string());
            offset += static_cast<std::size_t>(n);
            bytes = bytes.subspan(static_cast<std::size_t>(n));
        }
    }
    void hashThrough(std::uint64_t end) {
        if (hashed >= end) return;
        const auto capacity = std::min<std::uint64_t>(end - hashed, 4U << 20);
        auto buffer = std::make_unique_for_overwrite<unsigned char[]>(capacity);
        while (hashed < end) {
            std::span bytes(buffer.get(), static_cast<std::size_t>(std::min(capacity, end - hashed)));
            readExact(hashed, bytes);
            hash.update(bytes);
            hashed += bytes.size();
        }
    }
};

DigestingFileReader::DigestingFileReader(const std::filesystem::path& path, bool followSymlinks)
    : impl_(std::make_unique<Impl>(path, followSymlinks)) {
    auto& reader = *impl_;
    reader.fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | (followSymlinks ? 0 : O_NOFOLLOW));
    if (reader.fd < 0 || fstat(reader.fd, &reader.before) != 0 || !S_ISREG(reader.before.st_mode) ||
        reader.before.st_size < 0)
        throw std::runtime_error("Cannot open regular source for SHA-256: " + path.string());
}
DigestingFileReader::~DigestingFileReader() = default;
std::uint64_t DigestingFileReader::size() const { return static_cast<std::uint64_t>(impl_->before.st_size); }
void DigestingFileReader::readAt(std::uint64_t offset, std::span<unsigned char> bytes) {
    auto& reader = *impl_;
    if (offset > size() || bytes.size() > size() - offset) throw std::runtime_error("File read exceeds source size");
    reader.hashThrough(offset);
    reader.readExact(offset, bytes);
    if (const auto end = offset + bytes.size(); end > reader.hashed) {
        reader.hash.update(bytes.subspan(static_cast<std::size_t>(reader.hashed - offset)));
        reader.hashed = end;
    }
}
std::string DigestingFileReader::finish() {
    auto& reader = *impl_;
    reader.hashThrough(size());
    struct stat after{};
    struct stat named{};
    if (fstat(reader.fd, &after) != 0 ||
        (reader.follow ? stat(reader.path.c_str(), &named) : lstat(reader.path.c_str(), &named)) != 0 ||
        !same(reader.before, after) || !same(reader.before, named))
        throw std::runtime_error("Source changed during SHA-256: " + reader.path.string());
    if (reader.digest.empty()) reader.digest = reader.hash.finish();
    return reader.digest;
}

std::string fileSHA256(const std::filesystem::path& path) { return DigestingFileReader(path, true).finish(); }
FileDigestSnapshot digestRegularFile(const std::filesystem::path& path, std::size_t prefixBytes) {
    DigestingFileReader reader(path);
    FileDigestSnapshot out;
    out.size = reader.size();
    out.prefix.resize(std::min<std::uint64_t>(out.size, prefixBytes));
    reader.readAt(0, {reinterpret_cast<unsigned char*>(out.prefix.data()), out.prefix.size()});
    out.sha = reader.finish();
    return out;
}
std::string bytesSHA256(std::span<const unsigned char> bytes) {
    SHA256 hash;
    hash.update(bytes);
    return hash.finish();
}
} // namespace photonstack
