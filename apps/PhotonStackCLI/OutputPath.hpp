#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#ifdef __APPLE__
#include <stdio.h>
#endif

namespace photonstack::cli {

inline void requireNewOutputPath(const std::filesystem::path& path) {
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    if (path.empty() || std::filesystem::symlink_status(path).type() != std::filesystem::file_type::not_found ||
        !std::filesystem::is_directory(parent)) {
        throw std::invalid_argument("Choose a new output path with an existing parent directory: " + path.string());
    }
}

// The encoder can reopen its own file and select a codec by extension. Only
// the completed file is published, with exclusive destination semantics.
class NewOutputFile {
  public:
    explicit NewOutputFile(const std::filesystem::path& destination) : destination_(destination) {
        requireNewOutputPath(destination_);
        auto pattern = (destination.parent_path() / ".photonstack-output-XXXXXX").string();
        if (!mkdtemp(pattern.data()))
            throw std::system_error(errno, std::generic_category(), "Cannot create output staging directory");
        directory_ = pattern;
    }
    NewOutputFile(const NewOutputFile&) = delete;
    NewOutputFile& operator=(const NewOutputFile&) = delete;
    ~NewOutputFile() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
    std::filesystem::path file() const { return directory_ / ("result" + destination_.extension().string()); }
    void publish() const {
#ifdef __APPLE__
        if (renamex_np(file().c_str(), destination_.c_str(), RENAME_EXCL) != 0)
            throw std::system_error(errno, std::generic_category(), "Cannot publish new output file");
#else
        std::filesystem::create_hard_link(file(), destination_);
#endif
    }

  private:
    std::filesystem::path destination_;
    std::filesystem::path directory_;
};
} // namespace photonstack::cli
