#include "photonstack/SensorPatternBuilder.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace photonstack {
namespace {
void ensure(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
float median(std::vector<float>& a) {
    const auto n = a.size();
    std::nth_element(a.begin(), a.begin() + n / 2, a.end());
    return n % 2 ? a[n / 2] : float((double(a[n / 2]) + *std::max_element(a.begin(), a.begin() + n / 2)) / 2);
}
struct Sample {
    float value = 0, weight = 0;
};
struct Residual {
    std::vector<Sample> samples;
    std::array<float, 4> sigma{};
};
double smooth(double x) {
    x = std::clamp(x, 0., 1.);
    return x * x * (3 - 2 * x);
}
float location(const std::vector<Sample>& sample, double scale) {
    double total = 0, center = 0, lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (const auto& s : sample)
        if (s.weight > 0) {
            total += s.weight;
            center += s.weight * s.value;
            lo = std::min(lo, double(s.value));
            hi = std::max(hi, double(s.value));
        }
    if (total <= 0)
        return 0;
    center /= total;
    // Strictly convex pseudo-Huber objective. Bracketed Newton prevents a
    // distant contaminant from taking a step outside the unique root interval.
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        double gradient = 0, hessian = 0;
        for (const auto& s : sample)
            if (s.weight > 0) {
                const double z = (center - s.value) / scale, inv = 1 / std::sqrt(1 + z * z);
                gradient += s.weight * (center - s.value) * inv;
                hessian += s.weight * inv * inv * inv;
            }
        if (std::abs(gradient) <= 1.e-7 * scale * total)
            break;
        if (gradient > 0)
            hi = center;
        else
            lo = center;
        const double step = hessian > 0 ? center - gradient / hessian : (lo + hi) * .5;
        const double next = step > lo && step < hi ? step : (lo + hi) * .5;
        if (std::abs(next - center) <= 1.e-7 * scale) {
            center = next;
            break;
        }
        center = next;
    }
    return float(center);
}
int reflect(int x, int n) {
    const int period = 2 * (n - 1);
    x %= period;
    if (x < 0)
        x += period;
    return x < n ? x : period - x;
}
Residual residual(const ImageBuffer& raw) {
    const unsigned w = raw.width / 2, h = raw.height / 2;
    Residual result;
    result.samples.resize(raw.pixelCount());
    std::vector<float> a(w * h), tmp(w * h), hp(w * h), confidence(w * h), expanded(w * h);
    constexpr double kernel[] = {1. / 16, 4. / 16, 6. / 16, 4. / 16, 1. / 16};
    for (std::size_t p = 0; p < raw.pixelCount(); ++p)
        ensure(raw.pixels[p * 4 + 3] == 1 && std::isfinite(raw.pixels[p * 4]), "Incomplete or nonfinite sensor frame");
    for (unsigned c = 0; c < 4; ++c) {
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x)
                a[y * w + x] = raw.pixels[((2 * y + c / 2) * raw.width + 2 * x + c % 2) * 4];
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                double sum = 0;
                for (int k = -2; k <= 2; ++k)
                    sum += kernel[k + 2] * a[y * w + reflect(int(x) + k, w)];
                tmp[y * w + x] = float(sum);
            }
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                double sum = 0;
                for (int k = -2; k <= 2; ++k)
                    sum += kernel[k + 2] * tmp[reflect(int(y) + k, h) * w + x];
                hp[y * w + x] = float(double(a[y * w + x]) - sum);
            }
        std::vector<float> samples;
        const auto stride = std::max<std::size_t>(1, hp.size() / 200000);
        for (std::size_t p = 0; p < hp.size(); p += stride)
            samples.push_back(hp[p]);
        const double center = median(samples);
        for (auto& v : samples)
            v = float(std::abs(v - center));
        const double sigma = std::max(1.e-6, 1.4826 * median(samples));
        result.sigma[c] = float(sigma);
        for (std::size_t p = 0; p < hp.size(); ++p)
            confidence[p] = float(1 - smooth((std::abs(hp[p] - center) / sigma - 4) / 2));
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                float weight = 1;
                for (int k = -3; k <= 3; ++k)
                    weight = std::min(weight, confidence[y * w + std::clamp(int(x) + k, 0, int(w) - 1)]);
                expanded[y * w + x] = weight;
            }
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                float weight = 1;
                for (int k = -3; k <= 3; ++k)
                    weight = std::min(weight, expanded[std::clamp(int(y) + k, 0, int(h) - 1) * w + x]);
                result.samples[(2 * y + c / 2) * raw.width + 2 * x + c % 2] = {hp[y * w + x], weight};
            }
    }
    return result;
}
struct Cache {
    std::filesystem::path path;
    ~Cache() {
        if (!path.empty()) {
            std::error_code e;
            std::filesystem::remove_all(path, e);
        }
    }
};
void identity(const SensorPatternSource& s) {
    ensure(std::filesystem::file_size(s.path) == s.bytes &&
               std::filesystem::last_write_time(s.path).time_since_epoch().count() == s.modifiedTicks,
           "Sensor input changed during estimation");
}
} // namespace
SensorPatternResult SensorPatternBuilder::build(const std::vector<std::filesystem::path>& inputs,
                                                const SensorPatternOptions& o) const {
    SensorPatternResult result;
    Cache cache;
    try {
        ensure(o.minimumGroupSamples >= 3 && inputs.size() >= 3 * o.minimumGroupSamples && inputs.size() <= 192,
               "Require three training folds with sufficient samples (maximum 192 frames)");
        ensure(o.border >= 12 && o.sensorPositions.size() == inputs.size() && std::isfinite(o.minimumMotion) &&
                   o.minimumMotion >= 8 && std::isfinite(o.minimumCorrelation) && o.minimumCorrelation >= 0 &&
                   o.minimumCorrelation <= 1,
               "Invalid sensor pattern options or missing measured field positions");
        for (const auto& p : o.sensorPositions)
            ensure(std::isfinite(p[0]) && std::isfinite(p[1]), "Nonfinite field position");
        // Check movement within every group before touching the residual cache.
        for (unsigned fold = 0; fold < 3; ++fold) {
            std::vector<float> distances;
            for (std::size_t i = fold; i < inputs.size(); i += 3)
                for (std::size_t j = i + 3; j < inputs.size(); j += 3)
                    distances.push_back(float(std::hypot(o.sensorPositions[i][0] - o.sensorPositions[j][0],
                                                         o.sensorPositions[i][1] - o.sensorPositions[j][1])));
            std::sort(distances.begin(), distances.end());
            ensure(!distances.empty() && distances[distances.size() / 4] >= o.minimumMotion,
                   "Insufficient field movement within a training fold");
        }
        std::set<std::filesystem::path> unique;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto path = std::filesystem::canonical(inputs[i]);
            ensure(unique.insert(path).second, "Duplicate sensor input");
            for (const auto& prior : result.sources)
                ensure(!std::filesystem::equivalent(path, prior.path), "Aliased sensor input");
            auto sensor = FitsCodec().inspectSensor(path);
            ensure(sensor.ok, sensor.message.c_str());
            if (i == 0)
                result.sensor = sensor;
            else
                ensure(sensor.sameAcquisition(result.sensor), "Sensor acquisition settings do not match");
            result.sources.push_back(
                {path, std::filesystem::file_size(path),
                 static_cast<std::int64_t>(std::filesystem::last_write_time(path).time_since_epoch().count()),
                 unsigned(i % 3)});
        }
        const auto w = result.sensor.width, h = result.sensor.height;
        ensure(w % 2 == 0 && h % 2 == 0 && w >= 64 && h >= 64 && o.border < w / 2 && o.border < h / 2,
               "Sensor pattern requires even dimensions and a supported interior");
        const auto count = std::size_t(w) * h;
        const auto parent =
            o.temporaryDirectory.empty() ? std::filesystem::temp_directory_path() : o.temporaryDirectory;
        ensure(std::filesystem::is_directory(parent), "Sensor cache parent must exist");
        const auto available = std::filesystem::space(parent).available;
        constexpr auto reserve = 64ULL * 1024 * 1024;
        ensure(count <= (std::numeric_limits<std::uintmax_t>::max() / sizeof(Sample)) / inputs.size() &&
                   available >= reserve && count * sizeof(Sample) * inputs.size() <= available - reserve,
               "Insufficient sensor cache storage");
        static std::atomic<unsigned> serial = 0;
        const auto candidate = parent / ("photonstack-sensor-" +
                                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                         "-" + std::to_string(serial++));
        ensure(std::filesystem::create_directory(candidate), "Cannot create sensor cache");
        cache.path = candidate;
        const auto emit = [&](double p) {
            if (o.progress)
                o.progress(p);
        };
        emit(0);
        std::vector<std::array<float, 4>> noiseScales(inputs.size());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            identity(result.sources[i]);
            FitsDecodeOptions decode;
            decode.mode = FitsDecodeMode::Scientific;
            decode.debayer = false;
            const auto image = FitsCodec().read(result.sources[i].path, decode);
            ensure(image.ok, image.message.c_str());
            ensure(image.image.width == w && image.image.height == h && image.image.channels == 4,
                   "Unexpected sensor decode geometry");
            const auto values = residual(image.image);
            std::ofstream stream(cache.path / (std::to_string(i) + ".bin"), std::ios::binary);
            noiseScales[i] = values.sigma;
            stream.write(reinterpret_cast<const char*>(values.samples.data()),
                         std::streamsize(values.samples.size() * sizeof(Sample)));
            stream.close();
            ensure(bool(stream), "Cannot write sensor residual cache");
            identity(result.sources[i]);
            emit(.6 * (i + 1) / inputs.size());
        }
        std::array<std::vector<float>, 3> centers;
        std::array<std::vector<float>, 3> counts;
        std::array<std::array<double, 4>, 3> scales;
        for (unsigned f = 0; f < 3; ++f)
            for (unsigned c = 0; c < 4; ++c) {
                std::vector<float> v;
                for (std::size_t i = f; i < inputs.size(); i += 3)
                    v.push_back(noiseScales[i][c]);
                scales[f][c] = std::max(1.e-6, 1.5 * median(v));
            }
        for (unsigned f = 0; f < 3; ++f) {
            centers[f].resize(count);
            counts[f].resize(count);
        }
        std::vector<std::ifstream> streams;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            streams.emplace_back(cache.path / (std::to_string(i) + ".bin"), std::ios::binary);
            ensure(bool(streams.back()), "Cannot read sensor cache");
        }
        const auto tileSize = std::max<std::size_t>(1, (32ULL * 1024 * 1024) / (sizeof(Sample) * inputs.size()));
        for (std::size_t start = 0; start < count; start += tileSize) {
            const auto n = std::min(tileSize, count - start);
            std::vector<Sample> tile(n * inputs.size()), sample;
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                streams[i].read(reinterpret_cast<char*>(tile.data() + i * n), std::streamsize(n * sizeof(Sample)));
                ensure(streams[i].gcount() == std::streamsize(n * sizeof(Sample)), "Truncated sensor cache");
            }
            for (std::size_t p = 0; p < n; ++p)
                for (unsigned f = 0; f < 3; ++f) {
                    sample.clear();
                    double weight = 0;
                    for (std::size_t i = f; i < inputs.size(); i += 3) {
                        const auto& v = tile[i * n + p];
                        if (v.weight > 0) {
                            sample.push_back(v);
                            weight += v.weight;
                        }
                    }
                    counts[f][start + p] = float(weight);
                    const unsigned channel = 2 * ((start + p) / w % 2) + (start + p) % w % 2;
                    centers[f][start + p] = location(sample, scales[f][channel]);
                }
            emit(.6 + .2 * (start + n) / count);
        }
        streams.clear();
        for (unsigned excluded = 0; excluded < 3; ++excluded) {
            auto& model = result.models[excluded];
            model.excludedFold = excluded;
            for (std::size_t i = 0; i < inputs.size(); ++i)
                if (i % 3 != excluded)
                    model.trainingIndices.push_back(i);
            for (auto* image : {&model.correction, &model.validity}) {
                image->width = w;
                image->height = h;
                image->channels = 1;
                image->format = PixelFormat::Float32Gray;
                image->pixels.assign(count, 0);
            }
            const unsigned a = (excluded + 1) % 3, b = (excluded + 2) % 3;
            for (unsigned c = 0; c < 4; ++c) {
                struct Pixel {
                    std::size_t index;
                    double weight;
                };
                std::vector<Pixel> pixels;
                double n = 0, sa = 0, sb = 0, aa = 0, bb = 0, ab = 0;
                for (unsigned y = o.border; y < h - o.border; ++y)
                    for (unsigned x = o.border; x < w - o.border; ++x) {
                        if (2 * (y % 2) + x % 2 != c)
                            continue;
                        const auto p = std::size_t(y) * w + x;
                        const double weight =
                            smooth(std::min(counts[a][p], counts[b][p]) - (o.minimumGroupSamples - 1.));
                        if (weight <= 0)
                            continue;
                        pixels.push_back({p, weight});
                        const double av = centers[a][p], bv = centers[b][p];
                        n += weight;
                        sa += weight * av;
                        sb += weight * bv;
                        aa += weight * av * av;
                        bb += weight * bv * bv;
                        ab += weight * av * bv;
                    }
                if (n < 128)
                    continue;
                const double ac = sa / n, bc = sb / n, va = std::max(0., aa - sa * sa / n),
                             vb = std::max(0., bb - sb * sb / n), cov = ab - sa * sb / n;
                const double corr = va > 0 && vb > 0 ? std::clamp(cov / std::sqrt(va * vb), -1., 1.) : 0;
                const double power = .25 * (va + vb + 2 * cov);
                const double retention =
                    corr >= o.minimumCorrelation && power > 0 ? std::clamp(cov / power, 0., 1.) : 0;
                model.retention[c] = retention;
                model.correlation[c] = corr;
                for (const auto& p : pixels) {
                    model.correction.pixels[p.index] =
                        float(p.weight * retention * .5 * (centers[a][p.index] - ac + centers[b][p.index] - bc));
                    model.validity.pixels[p.index] = 1;
                }
            }
            emit(.8 + .2 * (excluded + 1) / 3);
        }
        for (const auto& source : result.sources)
            identity(source);
        result.ok = true;
    } catch (const std::exception& e) {
        result.ok = false;
        result.errorCode = "SensorPatternFailed";
        result.message = e.what();
        result.models = {}; // Never expose partially constructed correction fields.
    }
    return result;
}
} // namespace photonstack
