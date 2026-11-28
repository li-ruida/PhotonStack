#include "FrequencyStackCommand.hpp"
#include "OutputPath.hpp"
#include "photonstack/WorkingMemory.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/FrequencyCoadd.hpp"
#include "photonstack/ImageCodec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using photonstack::FrequencyCoadd;
using photonstack::NoiseCorrelation;
constexpr std::size_t maxPixels = 16 * 1024 * 1024;

template <class T> T number(const std::string& token) {
    T value{};
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(double(value)))
        throw std::invalid_argument("Invalid finite plan number: " + token);
    return value;
}

struct Identity {
    std::uintmax_t size;
    fs::file_time_type modified;
    explicit Identity(const fs::path& path) : size(fs::file_size(path)), modified(fs::last_write_time(path)) {
        if (!fs::is_regular_file(path))
            throw std::invalid_argument("Expected a regular file: " + path.string());
    }
    void check(const fs::path& path) const {
        const Identity now(path);
        if (now.size != size || now.modified != modified)
            throw std::runtime_error("Input changed during frequency stack: " + path.string());
    }
};

struct Model {
    double sky, variance, cx, cy;
    fs::path kernel;
    std::size_t kw, kh;
    std::vector<NoiseCorrelation> correlations;
};
struct Frame {
    fs::path path;
    double flux;
    std::array<Model, 3> models;
};

class Plan {
    std::ifstream stream;
    fs::path base;
    std::string token() {
        std::string result;
        if (!(stream >> result))
            throw std::invalid_argument("Unexpected end of frequency plan");
        return result;
    }
    void tag(const std::string& expected) {
        if (token() != expected)
            throw std::invalid_argument("Expected frequency plan field: " + expected);
    }
    template <class T> T read() { return number<T>(token()); }
    std::size_t bounded(std::size_t low, std::size_t high) {
        const auto value = read<std::size_t>();
        if (value < low || value > high)
            throw std::invalid_argument("Frequency plan integer outside supported range");
        return value;
    }
    fs::path path() {
        stream >> std::ws;
        if (stream.peek() != '"')
            throw std::invalid_argument("Plan paths require double quotes");
        std::string value;
        if (!(stream >> std::quoted(value)) || value.empty())
            throw std::invalid_argument("Invalid quoted plan path");
        const auto result = fs::canonical(base / fs::path(value));
        identities.try_emplace(result, result);
        return result;
    }

  public:
    std::size_t sw, sh, x, y, w, h, px, py, fw, fh;
    fs::path target;
    std::vector<Frame> frames;
    std::map<fs::path, Identity> identities;
    explicit Plan(const fs::path& source) : stream(source), base(fs::absolute(source).parent_path()) {
        const Identity planIdentity(source);
        if (!stream || planIdentity.size > 16 * 1024 * 1024)
            throw std::invalid_argument("Cannot read frequency plan, or plan exceeds 16 MiB");
        tag("PHOTONSTACK_FREQUENCY_PLAN");
        if (read<unsigned>() != 1)
            throw std::invalid_argument("Unsupported frequency plan version");
        tag("source-rgba-f32le");
        sw = bounded(1, 16384);
        sh = bounded(1, 16384);
        if (sw > maxPixels / sh)
            throw std::invalid_argument("Source grid exceeds 16 megapixels");
        tag("crop");
        x = bounded(0, sw - 1);
        y = bounded(0, sh - 1);
        w = bounded(1, sw - x);
        h = bounded(1, sh - y);
        tag("padding");
        px = bounded(0, 8192);
        py = bounded(0, 8192);
        fw = w + 2 * px;
        fh = h + 2 * py;
        if (fw > 16384 || fh > 16384 || fw > maxPixels / fh)
            throw std::invalid_argument("Padded grid exceeds supported bounds");
        tag("target-real-f64le");
        target = path();
        tag("frames");
        const auto count = bounded(1, 10000);
        std::set<fs::path> seen;
        for (std::size_t i = 0; i < count; ++i) {
            tag("frame");
            Frame frame;
            frame.path = path();
            frame.flux = read<double>();
            if (!seen.insert(frame.path).second)
                throw std::invalid_argument("Duplicate prepared frame path");
            if (frame.flux <= 0)
                throw std::invalid_argument("Frame flux must be positive");
            if (identities.at(frame.path).size != sw * sh * 4 * sizeof(float))
                throw std::invalid_argument("Prepared RGBA frame has incorrect byte count");
            for (std::size_t c = 0; c < 3; ++c) {
                tag("channel");
                if (read<unsigned>() != c)
                    throw std::invalid_argument("Expected channel order 0, 1, 2");
                auto& m = frame.models[c];
                m.sky = read<double>();
                m.variance = read<double>();
                m.cx = read<double>();
                m.cy = read<double>();
                m.kernel = path();
                m.kw = bounded(1, std::min(fw, std::size_t(129)));
                m.kh = bounded(1, std::min(fh, std::size_t(129)));
                if (!(m.kw % 2) || !(m.kh % 2) || m.variance <= 0 ||
                    std::max(std::abs(m.cx), std::abs(m.cy)) > double(std::min(m.kw, m.kh)) / 4)
                    throw std::invalid_argument("Invalid PSF dimensions, centroid or noise variance");
                if (identities.at(m.kernel).size != m.kw * m.kh * sizeof(double))
                    throw std::invalid_argument("PSF kernel has incorrect byte count");
                const auto n = bounded(0, 256);
                std::set<std::pair<int, int>> lags;
                for (std::size_t j = 0; j < n; ++j) {
                    tag("lag");
                    const int dx = read<int>(), dy = read<int>();
                    const double rho = read<double>();
                    if (dx < -64 || dx > 64 || dy < -64 || dy > 64 || (!dx && !dy) || std::abs(rho) > 1 ||
                        lags.contains({dx, dy}) || lags.contains({-dx, -dy}))
                        throw std::invalid_argument("Invalid or duplicate noise lag");
                    lags.insert({dx, dy});
                    m.correlations.push_back({dx, dy, rho});
                }
            }
            frames.push_back(std::move(frame));
        }
        tag("end");
        stream >> std::ws;
        if (!stream.eof())
            throw std::invalid_argument("Unexpected trailing frequency plan fields");
        planIdentity.check(source);
        identities.try_emplace(fs::canonical(source), source);
    }
    template <class T> std::vector<T> binary(const fs::path& file, std::size_t n) const {
        const auto& identity = identities.at(file);
        identity.check(file);
        if (identity.size != n * sizeof(T))
            throw std::invalid_argument("Incorrect binary byte count: " + file.string());
        std::ifstream in(file, std::ios::binary);
        std::vector<T> result(n);
        in.read(reinterpret_cast<char*>(result.data()), std::streamsize(n * sizeof(T)));
        if (!in || in.peek() != EOF)
            throw std::runtime_error("Cannot read complete binary input: " + file.string());
        identity.check(file);
        convertLittleEndian(result);
        return result;
    }

    std::vector<float> readCrop(const fs::path& file) const {
        const auto& identity = identities.at(file);
        identity.check(file);
        std::ifstream input(file, std::ios::binary);
        std::vector<float> result(w * h * 4);
        const auto readRows = [&](std::size_t row, std::size_t count) {
            input.seekg(std::streamoff(((row + y) * sw + x) * 4 * sizeof(float)));
            input.read(reinterpret_cast<char*>(result.data() + row * w * 4),
                       std::streamsize(count * w * 4 * sizeof(float)));
            if (!input) throw std::runtime_error("Cannot read prepared crop: " + file.string());
        };
        if (w == sw) readRows(0, h);
        else for (std::size_t row = 0; row < h; ++row) readRows(row, 1);
        identity.check(file);
        convertLittleEndian(result);
        for (std::size_t i = 3; i < result.size(); i += 4) {
            if (!std::isfinite(result[i]) || result[i] < .999F || result[i] > 1)
                throw std::invalid_argument("Prepared crop requires finite full coverage in every frame");
        }
        return result;
    }

  private:
    template <class T> static void convertLittleEndian(std::vector<T>& values) {
        if constexpr (std::endian::native != std::endian::little) {
            for (auto& value : values) {
                auto* bytes = reinterpret_cast<unsigned char*>(&value);
                std::reverse(bytes, bytes + sizeof(T));
            }
        }
    }
};

std::size_t reflect(long long i, std::size_t n) {
    if (n == 1)
        return 0;
    const auto period = 2 * static_cast<long long>(n - 1);
    i %= period;
    if (i < 0)
        i += period;
    return std::size_t(i < static_cast<long long>(n) ? i : period - i);
}

} // namespace

int runFrequencyStack(const std::vector<std::string>& args) {
    fs::path planPath, output;
    std::size_t threads = 0;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& key = args[i];
        if ((key != "--plan" && key != "--output" && key != "--threads") || !seen.insert(key).second ||
            i + 1 == args.size())
            throw std::invalid_argument("frequency-stack expects --plan, --output and optional --threads (once each)");
        const auto& value = args[++i];
        if (key == "--plan")
            planPath = value;
        else if (key == "--output")
            output = value;
        else
            threads = number<std::size_t>(value);
    }
    if (planPath.empty() || output.empty() || !photonstack::isFitsPath(output) || (!threads && seen.contains("--threads")) || threads > 64)
        throw std::invalid_argument("frequency-stack requires --plan, new FITS --output, and 1..64 threads");
    output = fs::absolute(output);
    if (fs::symlink_status(output).type() != fs::file_type::not_found)
        throw std::invalid_argument("Frequency output already exists; choose a new path");
    const Plan plan(planPath);
    const auto targetReal = plan.binary<double>(plan.target, plan.fh * (plan.fw / 2 + 1));
    if (!std::all_of(targetReal.begin(), targetReal.end(), [](double v) { return std::isfinite(v); }) ||
        std::abs(targetReal[0] - 1) > 1e-9)
        throw std::invalid_argument("Target response must be finite with unit DC");
    const std::vector<std::complex<double>> target(targetReal.begin(), targetReal.end());
    const auto stride = plan.fw / 2 + 1;
    for (std::size_t y = 0; y < plan.fh; ++y) {
        for (auto x : {std::size_t(0), plan.fw / 2}) {
            if (x != 0 && plan.fw % 2)
                continue;
            const double a = targetReal[y * stride + x];
            const double b = targetReal[((plan.fh - y) % plan.fh) * stride + x];
            if (std::abs(a - b) > 1e-10 * (1 + std::max(std::abs(a), std::abs(b))))
                throw std::invalid_argument("Target response violates real-image boundary symmetry");
        }
    }
    photonstack::ImageBuffer result;
    result.width = static_cast<std::uint32_t>(plan.w);
    result.height = static_cast<std::uint32_t>(plan.h);
    result.pixels.resize(plan.w * plan.h * 4, 1);
    std::array<std::size_t, 3> floored{};
    // Include the output, crop, target, FFT scratch and all three persistent
    // numerator/information spectra before admitting the one-read RGB path.
    const auto bins = plan.fh * stride;
    const auto parallelBytes = plan.w * plan.h * 32ULL + plan.fw * plan.fh * 48ULL + bins * 160ULL;
    const std::size_t channelsPerPass = parallelBytes <= photonstack::detail::workingMemoryBudget() ? 3 : 1;
    for (std::size_t first = 0; first < 3; first += channelsPerPass) {
        std::vector<FrequencyCoadd> accumulators;
        accumulators.reserve(channelsPerPass);
        for (std::size_t k = 0; k < channelsPerPass; ++k) accumulators.emplace_back(plan.fw, plan.fh, threads);
        for (std::size_t j = 0; j < plan.frames.size(); ++j) {
            const auto& frame = plan.frames[j];
            const auto raw = plan.readCrop(frame.path);
            for (std::size_t k = 0; k < channelsPerPass; ++k) {
                const auto c = first + k;
                auto& accumulator = accumulators[k];
                const auto& m = frame.models[c];
                const auto kernel = plan.binary<double>(m.kernel, m.kw * m.kh);
                const auto transfer = accumulator.psfTransfer(kernel, m.kw, m.kh, m.cx, m.cy);
                const auto noise = accumulator.noisePower(m.correlations);
                floored[c] += noise.flooredBins;
                std::vector<double> sample(plan.fw * plan.fh);
                for (std::size_t y = 0; y < plan.fh; ++y) {
                    const auto sy = reflect(static_cast<long long>(y) - static_cast<long long>(plan.py), plan.h);
                    for (std::size_t x = 0; x < plan.fw; ++x) {
                        const auto sx =
                            reflect(static_cast<long long>(x) - static_cast<long long>(plan.px), plan.w);
                        const auto offset = (sy * plan.w + sx) * 4;
                        sample[y * plan.fw + x] = double(raw[offset + c]) - m.sky;
                    }
                }
                accumulator.add(sample, transfer, frame.flux, m.variance, noise.values);
            }
            if ((j + 1) % 32 == 0 || j + 1 == plan.frames.size()) {
                const auto progress = (first + channelsPerPass * double(j + 1) / plan.frames.size()) / 3;
                std::cout << R"({"type":"progress","task":"stack","stage":"accumulating","command":"frequency-stack","channel":)"
                          << first << ",\"frames\":" << j + 1 << ",\"total\":" << plan.frames.size()
                          << ",\"progress\":" << .95 * progress << "}" << std::endl;
            }
        }
        for (std::size_t k = 0; k < channelsPerPass; ++k) {
            const auto c = first + k;
            auto& accumulator = accumulators[k];
            const auto image = accumulator.render(target);
            for (std::size_t y = 0; y < plan.h; ++y)
                for (std::size_t x = 0; x < plan.w; ++x) {
                    const double value = image[(y + plan.py) * plan.fw + x + plan.px];
                    if (std::abs(value) > std::numeric_limits<float>::max())
                        throw std::overflow_error("Frequency output exceeds scientific float32 FITS range");
                    result.pixels[(y * plan.w + x) * 4 + c] = static_cast<float>(value);
                }
        }
    }
    for (const auto& [path, identity] : plan.identities)
        identity.check(path);
    photonstack::cli::NewOutputFile publication(output);
    const auto written = photonstack::ImageCodec{}.write(result, publication.file());
    if (!written.ok)
        throw std::runtime_error(written.errorCode + ": " + written.message);
    publication.publish();
    std::cout << "{\"type\":\"progress\",\"task\":\"stack\",\"stage\":\"complete\",\"command\":\"frequency-stack\","
                 "\"progress\":1}"
              << std::endl;
    std::cout << "{\"type\":\"complete\",\"command\":\"frequency-stack\",\"fitsValues\":\"scientific\","
              << "\"clampOutput\":false,\"frames\":" << plan.frames.size() << ",\"width\":" << plan.w
              << ",\"height\":" << plan.h << ",\"noisePowerFlooredBins\":[" << floored[0] << ',' << floored[1] << ','
              << floored[2] << "]}" << std::endl;
    return 0;
}
