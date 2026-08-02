#include "photonstack/RawCodec.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include "photonstack/ImageInspector.hpp"

namespace photonstack {
namespace {

std::string lowercaseExtension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

InspectResult inspectError(std::string code, std::string message, const std::filesystem::path& path) {
    InspectResult result;
    result.ok = false;
    result.metadata.path = path.string();
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

} // namespace

bool isRawPath(const std::filesystem::path& path) {
    const auto extension = lowercaseExtension(path);
    return extension == ".dng" || extension == ".arw" || extension == ".cr2" || extension == ".cr3" ||
           extension == ".nef" || extension == ".nrw" || extension == ".raf" || extension == ".rw2" ||
           extension == ".orf" || extension == ".pef" || extension == ".srw";
}

InspectResult RawCodec::inspect(const std::filesystem::path& path) const {
    if (!isRawPath(path)) {
        return inspectError("InputFormatUnsupported", "Input path does not look like a supported RAW file", path);
    }

    const ImageInspector inspector;
    auto result = inspector.inspect(path);
    if (result.ok) {
        result.metadata.format = ImageFormat::RAW;
    }
    return result;
}

ImageReadResult RawCodec::read(const std::filesystem::path& path) const {
    return read(path, {});
}

ImageReadResult RawCodec::read(const std::filesystem::path& path, const RawDecodeOptions& options) const {
    const ImageCodec codec;
    return codec.read(path, {.raw = options});
}

} // namespace photonstack
