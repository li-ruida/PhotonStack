#include "photonstack/MasterFrameBuilder.hpp"

#include <string>

#include "photonstack/Stacker.hpp"

namespace photonstack {
namespace {

MasterFrameResult masterError(std::string code, std::string message) {
    MasterFrameResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

MasterFrameResult fromStackResult(StackResult stackResult) {
    if (stackResult.ok && stackResult.image.colorEncoding != ColorEncoding::Linear) {
        return masterError(
            "ImageColorEncodingMismatch",
            "Calibration master frames must be built from linear inputs"
        );
    }
    MasterFrameResult result;
    result.ok = stackResult.ok;
    result.image = std::move(stackResult.image);
    result.errorCode = std::move(stackResult.errorCode);
    result.message = std::move(stackResult.message);
    return result;
}

} // namespace

MasterFrameResult MasterFrameBuilder::build(const std::vector<std::filesystem::path>& inputs,
                                            const MasterFrameOptions& options) const {
    if (inputs.empty()) {
        return masterError("InputMissing", "At least one calibration frame is required");
    }
    if (!isValidRawDecodeOptions(options.raw) || !options.raw.linearOutput) {
        return masterError("ArgumentInvalid", "Master frame RAW decoding must use valid linear options");
    }

    StackOptions stackOptions;
    stackOptions.raw = options.raw;
    switch (options.method) {
    case MasterFrameMethod::Average:
        stackOptions.method = StackMethod::Average;
        break;
    case MasterFrameMethod::Median:
        stackOptions.method = StackMethod::Median;
        break;
    default:
        return masterError("MethodUnsupported", "Unsupported master frame method");
    }
    return fromStackResult(Stacker().stack(inputs, stackOptions));
}

} // namespace photonstack
