// Synthetic parity/timing probe. Compile with -DBASELINE for the former FFT default.
#define main cpuRegressionMain
#include "../../../tests/engine/CPUParallelTests.cpp"
#undef main
#include "photonstack/ImageCodec.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    std::filesystem::create_directories(argv[1]);
    auto image = scene();
    if (std::getenv("PHOTONSTACK_CPU_PROBE_STARS_ONLY")) {
        auto tile = image;
        image.width *= 4;
        image.height *= 4;
        image.pixels.resize(image.sampleCount());
        for (unsigned y = 0; y < image.height; ++y)
            for (unsigned x = 0; x < image.width; ++x)
                for (unsigned c = 0; c < 4; ++c)
                    image.pixels[(std::size_t(y) * image.width + x) * 4 + c] =
                        tile.pixels[((y % tile.height) * tile.width + x % tile.width) * 4 + c];
    }
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        image.pixels[p * 4 + 3] = 1;
    auto run = [&](const char* name, auto compute) {
        for (int repeat = 0; repeat < 3; ++repeat) {
            const auto start = std::chrono::steady_clock::now();
            auto result = compute();
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (!result.ok)
                throw std::runtime_error(result.message);
            std::cout << name << "," << repeat << "," << ms << "\n" << std::flush;
            if (repeat == 2) {
                std::ofstream out(std::filesystem::path(argv[1]) / (std::string(name) + ".f32"), std::ios::binary);
                out.write(reinterpret_cast<const char*>(result.image.pixels.data()),
                          result.image.pixels.size() * sizeof(float));
            }
        }
    };
    run("stars", [&] {
        auto r = StarDetector().detect(image, {.maxStars = 1000, .countAllDetections = true});
        ImageReadResult out;
        out.ok = r.ok;
        out.message = r.message;
        out.image.width = r.stars.size();
        out.image.height = 1;
        out.image.channels = 6;
        for (const auto& s : r.stars)
            for (float f : {s.x, s.y, s.flux, s.peak, s.fwhm, s.eccentricity})
                out.image.pixels.push_back(f);
        return out;
    });
    if (std::getenv("PHOTONSTACK_CPU_PROBE_STARS_ONLY"))
        return 0;
    run("neutralize", [&] { return ColorAdjuster().neutralizeBackground(image); });
    run("saturation", [&] { return ColorAdjuster().adjustSaturation(image); });
    auto display = image;
    display.colorEncoding = ColorEncoding::SRGB;
    run("green", [&] {
        return ColorAdjuster().suppressGreenCast(display, {.averageNeutral = true, .preserveLightness = true});
    });
    run("coma", [&] { return ComaReducer().reduce(image); });
    run("develop", [&] { return AstroDevelop().apply(image, {.stellarBalance = false}); });
    run("develop_stars", [&] { return AstroDevelop().apply(image, {.stellarBalance = false, .starExposure = .7F}); });
    run("rl", [&] { return Deconvolution().richardsonLucy(image, {.iterations = 8, .clampOutput = false}); });
    std::vector<float> samples(image.pixelCount());
    for (std::size_t p = 0; p < samples.size(); ++p)
        samples[p] = image.pixels[p * 4];
    run("cfa", [&] {
        CfaDrizzleAccumulator a({.width = image.width, .height = image.height});
        a.add({.width = image.width,
               .height = image.height,
               .samples = samples,
               .transform = {1, .001, -.002, 1, .3, -.2}});
        ImageReadResult r;
        r.ok = true;
        r.image = a.image();
        return r;
    });
    for (int repeat = 0; repeat < 3; ++repeat) {
        const auto start = std::chrono::steady_clock::now();
#ifdef BASELINE
        FrequencyCoadd fft(768, 512, 1);
#else
        FrequencyCoadd fft(768, 512);
#endif
        std::vector<double> plane(samples.begin(), samples.end());
        auto transfer = fft.psfTransfer(std::vector<double>{1}, 1, 1);
        auto power = fft.noisePower({});
        fft.add(plane, transfer, 1, 2, power.values);
        auto out = fft.render(transfer);
        std::cout << "fft," << repeat << ","
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << "\n"
                  << std::flush;
        if (repeat == 2) {
            std::ofstream file(std::filesystem::path(argv[1]) / "fft.f64", std::ios::binary);
            file.write(reinterpret_cast<const char*>(out.data()), out.size() * sizeof(double));
        }
    }
}
