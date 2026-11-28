#include "../../../engine/src/RegisteredStackCache.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace photonstack;
namespace fs = std::filesystem;
int main() {
    using Clock = std::chrono::steady_clock;
    const auto dir = fs::temp_directory_path() /
                     ("photonstack-ram-probe-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } cleanup{dir};
    std::vector<ImageBuffer> images(24);
    for (std::size_t f = 0; f < images.size(); ++f) {
        auto& a = images[f];
        a.width = 512;
        a.height = 384;
        a.pixels.resize(a.sampleCount());
        for (std::size_t p = 0; p < a.pixelCount(); ++p) {
            for (unsigned c = 0; c < 3; ++c)
                a.pixels[p * 4 + c] = float((p * 17 + f * 23 + c * 31) % 991) * .01F;
            a.pixels[p * 4 + 3] = p % 113 == 0 ? 0.F : f % 3 == 0 ? .5F : 1.F;
        }
    }
    ImageBuffer baseline;
    for (unsigned trial = 0; trial < 3; ++trial)
        for (unsigned phase = 0; phase < 2; ++phase) {
            const bool memory = (phase + trial) % 2;
            const auto begin = Clock::now();
            std::vector<fs::path> paths;
            std::vector<detail::RegisteredFrame> frames;
            for (std::size_t f = 0; f < images.size(); ++f) {
                paths.push_back(dir / (std::to_string(trial) + "-" + std::to_string(phase) + "-" + std::to_string(f)));
                detail::RegisteredFrame frame;
                const auto stored = detail::writeRegisteredFrame(paths.back(), images[f], false, frame, memory);
                if (!stored.ok)
                    throw std::runtime_error(stored.message);
                frames.push_back(std::move(frame));
            }
            StackOptions o;
            o.method = StackMethod::SigmaClip;
            o.sigma.medianMad = true;
            auto output = detail::combineRegisteredFrames(paths, frames, o);
            const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            if (!output.ok)
                throw std::runtime_error(output.message);
            if (baseline.empty())
                baseline = output.image;
            if (output.image.pixels.size() != baseline.pixels.size() ||
                std::memcmp(output.image.pixels.data(), baseline.pixels.data(), baseline.pixels.size() * sizeof(float)))
                throw std::runtime_error("RAM/disk mismatch");
            std::uintmax_t disk = 0;
            for (const auto& p : paths)
                if (fs::exists(p))
                    disk += fs::file_size(p);
            std::cout << (memory ? "ram" : "disk") << "," << trial << "," << ms << "," << disk << ",bitwise_equal\n"
                      << std::flush;
        }
}
