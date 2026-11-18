#include "SpectralProfileCommand.hpp"
#include "SpectralData.hpp"
#include <algorithm>
#include <iostream>

namespace {
using namespace photonstack;
using namespace photonstack::cli;
using Point = std::pair<double, double>;
double at(const SpectralCurve& c, double x) {
    const auto end = std::upper_bound(c.wavelength.begin(), c.wavelength.end(), x);
    const auto i = std::clamp<std::size_t>(end - c.wavelength.begin(), 1, c.value.size() - 1) - 1;
    const double t = (x - c.wavelength[i]) / (c.wavelength[i + 1] - c.wavelength[i]);
    return c.value[i] * (1 - t) + c.value[i + 1] * t;
}
// Display only: retain first/last/min/max samples in each wavelength bin.
// Integration below always uses the original component curves, not this plot.
void plot(std::ostream& out, const std::vector<Point>& points) {
    std::set<std::size_t> keep;
    const double low = points.front().first, span = points.back().first - low;
    std::size_t begin = 0;
    while (begin < points.size()) {
        const auto bin = [&](std::size_t i) { return std::min(511, int((points[i].first - low) / span * 512)); };
        std::size_t end = begin + 1, minimum = begin, maximum = begin;
        while (end < points.size() && bin(end) == bin(begin)) {
            if (points[end].second < points[minimum].second)
                minimum = end;
            if (points[end].second > points[maximum].second)
                maximum = end;
            ++end;
        }
        keep.insert(begin);
        keep.insert(end - 1);
        keep.insert(minimum);
        keep.insert(maximum);
        begin = end;
    }
    out << '[';
    bool first = true;
    for (auto i : keep) {
        if (!first)
            out << ',';
        first = false;
        out << '[' << points[i].first << ',' << points[i].second << ']';
    }
    out << ']';
}
} // namespace

int runSpectralProfileInspection(const std::vector<std::string>& args) {
    std::map<std::string, std::string> values;
    for (std::size_t i = 1; i < args.size(); i += 2)
        if (i + 1 >= args.size() || (args[i] != "--profile" && args[i] != "--expected-sha") ||
            !values.emplace(args[i], args[i + 1]).second)
            throw std::invalid_argument("Use spcc-profile-inspect --profile profile.txt [--expected-sha sha256]");
    if (!values.contains("--profile"))
        throw std::invalid_argument("Choose a spectral profile");
    const auto path = std::filesystem::canonical(values.at("--profile"));
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 16 * 1024 * 1024)
        throw std::invalid_argument("Invalid spectral profile file");
    const auto sha = fileSHA256(path);
    if (values.contains("--expected-sha") && values.at("--expected-sha") != sha)
        throw std::runtime_error("Spectral profile changed");
    const auto p = readSpectralProfile(path);
    std::ostringstream out;
    out << std::setprecision(17)
        << "{\"type\":\"complete\",\"command\":\"spcc-profile-inspect\",\"schema\":1,\"profile\":{"
        << "\"name\":" << channelJSON(p.name) << ",\"mode\":" << channelJSON(p.mode)
        << ",\"quality\":" << channelJSON(p.quality) << ",\"provenance\":" << channelJSON(p.provenance)
        << ",\"license\":" << channelJSON(p.license) << ",\"sha\":" << channelJSON(sha)
        << ",\"whiteName\":" << channelJSON(p.whiteName) << ",\"whiteProvenance\":" << channelJSON(p.whiteProvenance)
        << ",\"whiteLicense\":" << channelJSON(p.whiteLicense) << "},\"bands\":[";
    for (unsigned c = 0; c < 3; ++c) {
        const auto& band = p.response[c];
        double low = 0, high = 1e6;
        for (const auto& component : band) {
            low = std::max(low, component.wavelength.front());
            high = std::min(high, component.wavelength.back());
        }
        std::set<double> nodes{low, high};
        for (const auto& component : band)
            for (double x : component.wavelength)
                if (x > low && x < high)
                    nodes.insert(x);
        for (unsigned i = 1; i < 2048; ++i)
            nodes.insert(low + (high - low) * i / 2048);
        const std::vector<double> base(nodes.begin(), nodes.end());
        for (std::size_t i = 1; i < base.size(); ++i)
            nodes.insert((base[i - 1] + base[i]) / 2);
        std::vector<Point> points;
        for (double x : nodes) {
            double y = 1;
            for (const auto& component : band)
                y *= at(component, x);
            points.emplace_back(x, y);
        }
        const auto flux = integratePhotonSpectrum(p.white, band);
        if (c)
            out << ',';
        out << "{\"components\":" << band.size() << ",\"support\":[" << low << ',' << high
            << "],\"meanWavelength\":" << flux.meanWavelength << ",\"rmsBandwidth\":" << flux.rmsBandwidth
            << ",\"whiteFlux\":" << flux.value << ",\"samples\":";
        plot(out, points);
        out << '}';
    }
    std::vector<Point> white;
    double normalization = 0;
    for (double y : p.white.flux.value)
        normalization = std::max(normalization, std::abs(y));
    for (std::size_t i = 0; i < p.white.flux.value.size(); ++i)
        white.emplace_back(p.white.flux.wavelength[i], p.white.flux.value[i] / normalization);
    out << "],\"whiteNormalization\":" << normalization << ",\"whiteSamples\":";
    plot(out, white);
    out << ",\"plotMethod\":\"sampled-minmax-512\"}";
    if (fileSHA256(path) != sha)
        throw std::runtime_error("Spectral profile changed during inspection");
    std::cout << out.str() << '\n';
    return 0;
}
