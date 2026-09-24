#include "FrequencyStackCommand.hpp"
#include "photonstack/ImageCodec.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class T> void binary(const fs::path& path, std::vector<T> values) {
    if constexpr (std::endian::native != std::endian::little)
        for (auto& value : values) {
            auto* bytes = reinterpret_cast<char*>(&value);
            std::reverse(bytes, bytes + sizeof(T));
        }
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(T));
    require(bool(f), "fixture write");
}
std::string contents(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
int main() {
    const auto root =
        fs::temp_directory_path() / ("photonstack frequency tests " + std::to_string(std::random_device{}()));
    fs::create_directory(root);
    try {
        const auto plan = root / "plan.psfreq", output = root / "result.fits";
        const std::string first = "first frame.f32", second = "second frame.f32", kernel = "kernel.f64";
        std::array<std::vector<float>, 2> frames;
        for (std::size_t f = 0; f < 2; ++f) {
            frames[f].resize(6 * 5 * 4, 1);
            for (int y = 0; y < 5; ++y)
                for (int x = 0; x < 6; ++x) {
                    const std::array<float, 3> scene{float(x + 10 * y - 80), float(120000 + x),
                                                     float(-600 + 3 * x - y)};
                    for (int c = 0; c < 3; ++c)
                        frames[f][(y * 6 + x) * 4 + c] = float(f + 1) * scene[c] + float(100 * (f + 1)) + float(4 * f);
                }
            binary(root / (f ? second : first), frames[f]);
        }
        binary(root / kernel, std::vector<double>{1});
        const std::vector<double> target(5 * 5, 1);
        binary(root / "target.f64", target);
        std::ostringstream text;
        text << "PHOTONSTACK_FREQUENCY_PLAN 1\nsource-rgba-f32le 6 5\ncrop 1 1 4 3\npadding 2 1\n"
             << "target-real-f64le \"target.f64\"\nframes 2\n";
        for (int f = 0; f < 2; ++f) {
            text << "frame " << std::quoted(f ? second : first) << ' ' << f + 1 << '\n';
            for (int c = 0; c < 3; ++c)
                text << "channel " << c << ' ' << 100 * (f + 1) << ' ' << (f ? 4 : 1) << " 0 0 " << std::quoted(kernel)
                     << " 1 1 0\n";
        }
        text << "end\n";
        const auto good = text.str();
        auto writePlan = [&](const std::string& value) {
            std::ofstream out(plan);
            out << value;
        };
        auto run = [&] {
            return runFrequencyStack({"--plan", plan.string(), "--output", output.string(), "--threads", "2"});
        };
        auto fail = [&] {
            bool failed = false;
            try {
                run();
            } catch (const std::exception&) {
                failed = true;
            }
            require(failed, "invalid input must fail");
            require(!fs::exists(output), "failed run must not publish output");
        };
        writePlan(good);
        require(run() == 0, "valid command");
        photonstack::ImageReadOptions options;
        options.fits.mode = photonstack::FitsDecodeMode::Scientific;
        const auto decoded = photonstack::ImageCodec{}.read(output, options);
        require(decoded.ok && decoded.image.width == 4 && decoded.image.height == 3, "scientific FITS dimensions");
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 4; ++x) {
                // Analytic coadd: equal flux^2/variance weights. Second frame adds 4/2 units,
                // so the final value is scene + 1, with no clipping, gain or RGB conversion.
                const auto offset = ((y + 1) * 6 + x + 1) * 4;
                for (int c = 0; c < 3; ++c)
                    require(std::abs(decoded.image.pixels[(y * 4 + x) * 4 + c] - (frames[0][offset + c] - 99)) < 1e-4,
                            "unequal flux/variance and sky retain analytic RGB values");
            }
        const auto original = contents(output);
        bool existingFailed = false;
        try {
            run();
        } catch (const std::exception&) {
            existingFailed = true;
        }
        require(existingFailed && contents(output) == original, "existing output untouched");
        fs::remove(output);
        // Sky offsets are float64 model parameters even though cached pixels and
        // output FITS are float32. A float32 parser would erase this residual.
        auto precisionPlan = good;
        for (int f = 0; f < 2; ++f) {
            auto constant = frames[f];
            for (std::size_t p = 0; p < constant.size(); p += 4)
                for (std::size_t c = 0; c < 3; ++c)
                    constant[p + c] = float(100 * (f + 1));
            binary(root / (f ? second : first), constant);
            for (int c = 0; c < 3; ++c) {
                const auto oldField = "channel " + std::to_string(c) + " " + std::to_string(100 * (f + 1));
                const auto newField = oldField + (f ? ".000002" : ".000001");
                precisionPlan.replace(precisionPlan.find(oldField), oldField.size(), newField);
            }
        }
        writePlan(precisionPlan);
        require(run() == 0, "double precision background model");
        const auto precise = photonstack::ImageCodec{}.read(output, options);
        require(precise.ok, "precision FITS read");
        for (std::size_t p = 0; p < precise.image.pixels.size(); p += 4)
            for (std::size_t c = 0; c < 3; ++c)
                require(std::abs(double(precise.image.pixels[p + c]) + 1e-6) < 1e-12,
                        "sub-float32 sky residual survives");
        fs::remove(output);
        binary(root / first, frames[0]);
        binary(root / second, frames[1]);
        writePlan(good);
        // Dangling links also count as existing outputs, and never redirect writes.
        fs::create_symlink(root / "missing.fits", output);
        bool linkFailed = false;
        try {
            run();
        } catch (const std::exception&) {
            linkFailed = true;
        }
        require(linkFailed && fs::is_symlink(output) && !fs::exists(root / "missing.fits"), "symlink output protected");
        fs::remove(output);
        auto mutate = [&](const std::string& from, const std::string& to) {
            auto changed = good;
            const auto at = changed.find(from);
            require(at != std::string::npos, "mutation target");
            changed.replace(at, from.size(), to);
            writePlan(changed);
            fail();
        };
        mutate("PLAN 1", "PLAN 2");
        mutate("6 5", "9999999999999999999999 5");
        mutate("crop 1 1 4 3", "crop 4 1 4 3");
        mutate("padding 2 1", "padding -1 1");
        mutate("channel 0 100 1", "channel 0 100 0");
        mutate("channel 0 100 1", "channel 0 nan 1");
        mutate("channel 0", "channel 2");
        mutate("1 1 0\n", "1 1 2\nlag 1 0 0.2\nlag -1 0 0.2\n");
        mutate("second frame.f32", "first frame.f32");
        mutate("end\n", "end\nextra\n");
        writePlan(good);
        binary(root / kernel, std::vector<double>{-1});
        fail();
        binary(root / kernel, std::vector<double>{1});
        auto brokenTarget = target;
        brokenTarget[0] = 2;
        binary(root / "target.f64", brokenTarget);
        fail();
        brokenTarget = target;
        brokenTarget[5] = 2; // real-spectrum x=0 boundary mismatch
        binary(root / "target.f64", brokenTarget);
        fail();
        binary(root / "target.f64", target);
        for (const float bad : {0.5F, std::numeric_limits<float>::quiet_NaN()}) {
            auto broken = frames[0];
            broken[(1 * 6 + 1) * 4 + 3] = bad;
            binary(root / first, broken);
            fail();
        }
        auto broken = frames[0];
        broken[(1 * 6 + 1) * 4 + 1] = std::numeric_limits<float>::infinity();
        binary(root / first, broken);
        fail();
        binary(root / first, frames[0]);
        binary(root / first, std::vector<float>{1});
        fail();
        for (const auto& item : fs::directory_iterator(root))
            require(!item.path().filename().string().starts_with(".photonstack-frequency-"),
                    "no abandoned staging directories");
        fs::remove_all(root);
        std::cout << "Frequency command integration checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << " (fixtures: " << root << ")\n";
        return 1;
    }
}
