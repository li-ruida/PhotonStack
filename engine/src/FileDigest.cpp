#include "photonstack/FileDigest.hpp"
#include <array>
#include <fstream>
#include <stdexcept>
#ifdef __APPLE__
#define COMMON_DIGEST_FOR_OPENSSL
#include <CommonCrypto/CommonDigest.h>
#else
#include <memory>
#include <openssl/evp.h>
#endif
namespace photonstack {
std::string fileSHA256(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot hash source: " + path.string());
    std::array<unsigned char, 32> digest{};
    std::array<char, 65536> bytes{};
#ifdef __APPLE__
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
#else
    const auto context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Cannot initialize SHA-256");
#endif
    while (file) {
        file.read(bytes.data(), bytes.size());
        const auto count = file.gcount();
#ifdef __APPLE__
        CC_SHA256_Update(&context, bytes.data(), static_cast<CC_LONG>(count));
#else
        if (EVP_DigestUpdate(context.get(), bytes.data(), static_cast<std::size_t>(count)) != 1)
            throw std::runtime_error("Cannot update SHA-256");
#endif
    }
    if (!file.eof() || file.bad())
        throw std::runtime_error("Cannot read source for SHA-256: " + path.string());
#ifdef __APPLE__
    CC_SHA256_Final(digest.data(), &context);
#else
    unsigned size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 || size != digest.size())
        throw std::runtime_error("Cannot finalize SHA-256");
#endif
    std::string text;
    constexpr char hex[] = "0123456789abcdef";
    for (auto v : digest) {
        text += hex[v >> 4];
        text += hex[v & 15];
    }
    return text;
}
} // namespace photonstack
