#include "photonstack/PsfHomogenizer.hpp"
#include <cmath>
#include <limits>
#include <source_location>
#include <stdexcept>
using namespace photonstack;
void require(bool ok, const std::source_location where = std::source_location::current()) {
    if (!ok)
        throw std::runtime_error("PSF check at line " + std::to_string(where.line()));
}
int main() {
    ImageBuffer image;
    image.width = image.height = 49;
    image.channels = 4;
    image.pixels.resize(image.sampleCount());
    const PsfHomogenizer processor;
    PsfHomogenizeOptions options{1.2, 0.9, 0.03, 0.7};
    for (int y = 0; y < 49; ++y)
        for (int x = 0; x < 49; ++x) {
            const double u = x - 24.2, v = y - 23.7;
            const double star = 1000 * std::exp(-0.5 * (0.9 * u * u + 1.2 * v * v - 0.06 * u * v) / (1.08 - .0009));
            const auto offset = (y * 49 + x) * 4;
            image.pixels[offset] = static_cast<float>(star);
            image.pixels[offset + 1] = static_cast<float>(star * 2);
            image.pixels[offset + 2] = static_cast<float>(star - 100);
            image.pixels[offset + 3] = 1;
        }
    const auto corrected = processor.apply(image, options);
    require(corrected.ok);
    auto moments = [](const ImageBuffer& im) {
        std::array<double, 6> m{};
        for (int y = 0; y < 49; ++y)
            for (int x = 0; x < 49; ++x) {
                const double value = im.pixels[(y * 49 + x) * 4];
                m[0] += value;
                m[1] += value * x;
                m[2] += value * y;
                m[3] += value * x * x;
                m[4] += value * y * y;
                m[5] += value * x * y;
            }
        for (int i = 1; i < 6; ++i)
            m[i] /= m[0];
        m[3] -= m[1] * m[1];
        m[4] -= m[2] * m[2];
        m[5] -= m[1] * m[2];
        return m;
    };
    const auto before = moments(image), after = moments(corrected.image);
    const double target = std::sqrt(1.08 - .0009);
    require(std::abs(after[0] / before[0] - 1) < 1e-6);
    require(std::abs(after[1] - before[1]) < 1e-5 && std::abs(after[2] - before[2]) < 1e-5);
    require(std::abs(after[3] - before[3] - .7 * (target - 1.2)) < 1e-5);
    require(std::abs(after[4] - before[4] - .7 * (target - .9)) < 1e-5);
    require(std::abs(after[5] - before[5] + .7 * .03) < 1e-5);
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        require(std::abs(corrected.image.pixels[p * 4 + 1] - 2 * corrected.image.pixels[p * 4]) < 1e-4);
        require(std::abs(corrected.image.pixels[p * 4 + 2] - (corrected.image.pixels[p * 4] - 100)) < 1e-4);
        require(corrected.image.pixels[p * 4 + 3] == 1);
    }
    // No clipping of negative scientific values or signed kernel lobes.
    require(corrected.image.pixels[2] == -100);
    options.strength = 0;
    require(processor.apply(image, options).image.pixels == image.pixels);
    options.strength = .7;
    // Mask holes/partial coverage keep every neighboring covered pixel unchanged.
    const auto center = (24 * 49 + 24) * 4;
    image.pixels[center + 3] = 0;
    image.pixels[center] = std::numeric_limits<float>::quiet_NaN();
    const auto masked = processor.apply(image, options);
    require(masked.ok && masked.image.pixels[center] == 0 && masked.image.pixels[center + 3] == 0);
    require(masked.image.pixels[center + 4] == image.pixels[center + 4]);
    image.pixels[center] = 100;
    image.pixels[center + 3] = .5;
    require(processor.apply(image, options).image.pixels[center + 4] == image.pixels[center + 4]);
    image.pixels[center] = std::numeric_limits<float>::infinity();
    require(!processor.apply(image, options).ok);
    image.pixels[center] = 100;
    for (auto bad :
         {PsfHomogenizeOptions{1, 1, 2, .7}, PsfHomogenizeOptions{4, 1, 0, 1}, PsfHomogenizeOptions{1, 1, 0, -.1},
          PsfHomogenizeOptions{1, 1, 0, 1.1}, PsfHomogenizeOptions{1, 1, std::numeric_limits<double>::quiet_NaN(), .7}})
        require(!processor.apply(image, bad).ok);
}
