#pragma once

#include "photonstack/ImageCodec.hpp"
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace photonstack::detail {
struct GuardAperture {
    float x, y;
    double radius;
    std::array<double, 3> sky, noise;
};

// Request-local, bounded replay for one input. A zero limit disables capture.
class GuardReplay {
  public:
    explicit GuardReplay(std::size_t maxApertures) : limit_(maxApertures) {}
    GuardReplay(GuardReplay&& other) noexcept
        : limit_(other.limit_), identity_(std::exchange(other.identity_, std::nullopt)),
          apertures_(std::move(other.apertures_)), use_(std::exchange(other.use_, Use::unused)) {}
    bool ready() const { return identity_.has_value(); }
    std::size_t apertureCount() const { return apertures_.size(); }
    bool replayedLastRead() const { return use_ == Use::replayed; }

  private:
    struct Identity {
        std::array<unsigned char, 32> mosaicSHA256;
        std::uint32_t width, height;
        std::array<int, 4> colors;
        std::array<float, 3> gains;
        bool maskNonFinite;
        bool operator==(const Identity&) const = default;
    };
    enum class Use { unused, captured, replayed };
    std::size_t limit_;
    std::optional<Identity> identity_;
    std::vector<GuardAperture> apertures_;
    Use use_ = Use::unused;
    void discard();
    bool append(const GuardAperture& aperture);
    friend ImageReadResult readAnalysisFits(const std::filesystem::path&, const FitsDecodeOptions&, GuardReplay&);
    friend bool guardDemosaicStars(ImageBuffer&, std::span<const float>, const std::array<int, 4>&,
                                   const std::array<float, 3>&, GuardReplay*, bool);
};

ImageReadResult readAnalysisFits(const std::filesystem::path& path, const FitsDecodeOptions& options, GuardReplay& replay);

// Native CFA samples and the integrated RGB of each repaired aperture are
// preserved. False requests a fresh decode after an unsuccessful replay.
bool guardDemosaicStars(ImageBuffer& image, std::span<const float> mosaic,
                        const std::array<int, 4>& colors, const std::array<float, 3>& gains,
                        GuardReplay* replay = nullptr, bool maskNonFinite = false);
}
