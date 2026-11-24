#pragma once
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <vector>
#include <unistd.h>
#ifdef __linux__
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace photonstack::cli {
class NewProductDirectory {
    std::filesystem::path destination_;
public:
    std::filesystem::path stage;
    explicit NewProductDirectory(const std::filesystem::path& destination): destination_(std::filesystem::absolute(destination).lexically_normal()) {
        if (std::filesystem::exists(destination_) || std::filesystem::is_symlink(destination_) ||
            !std::filesystem::is_directory(destination_.parent_path())) throw std::invalid_argument("Choose a new directory with an existing parent");
        const auto pattern = (destination_.parent_path() / ".photonstack-product-XXXXXX").string();
        std::vector<char> text(pattern.begin(), pattern.end()); text.push_back(0);
        if (!mkdtemp(text.data())) throw std::runtime_error("Cannot create product staging directory");
        stage = text.data();
    }
    NewProductDirectory(const NewProductDirectory&) = delete;
    ~NewProductDirectory() { std::error_code error; std::filesystem::remove_all(stage, error); }
    void publish() {
#ifdef __APPLE__
        const auto status = renamex_np(stage.c_str(), destination_.c_str(), RENAME_EXCL);
#elif defined(__linux__)
        const auto status = syscall(SYS_renameat2, AT_FDCWD, stage.c_str(), AT_FDCWD, destination_.c_str(), RENAME_NOREPLACE);
#else
        throw std::runtime_error("Exclusive publication is unavailable on this platform");
#endif
#if defined(__APPLE__) || defined(__linux__)
        if (status) throw std::system_error(errno, std::generic_category(), "Cannot publish product directory");
#endif
    }
};
}
