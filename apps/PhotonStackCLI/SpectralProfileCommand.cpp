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
        << ",\"whiteLicense\":" << channelJSON(p.whiteLicense);
    narrowbandJSON(out, p.narrowband);
    out << "},\"bands\":[";
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

int runNarrowbandProfile(const std::vector<std::string>& args) {
    namespace fs = std::filesystem;
    const std::set<std::string> keys{"--profile", "--layout",     "--centers",     "--widths",
                                     "--name",    "--output-dir", "--expected-sha"};
    std::map<std::string, std::string> values;
    for (std::size_t i = 1; i < args.size(); i += 2)
        if (i + 1 >= args.size() || !keys.contains(args[i]) || !values.emplace(args[i], args[i + 1]).second)
            throw std::invalid_argument("Invalid narrowband profile option");
    for (const auto& key : keys)
        if (key != "--expected-sha" && !values.contains(key))
            throw std::invalid_argument("Specify " + key);
    const auto source = fs::canonical(values.at("--profile"));
    const auto sha = fileSHA256(source);
    if (values.contains("--expected-sha") && values.at("--expected-sha") != sha)
        throw std::runtime_error("Base spectral response changed");
    auto p = readSpectralProfile(source);
    if (p.narrowband)
        throw std::invalid_argument(
            "Choose a base response without this narrowband model; do not apply the filter twice");
    NarrowbandModel model;
    model.layout = values.at("--layout");
    model.baseSHA = sha;
    auto triplet = [&](const std::string& key) {
        auto text = values.at(key);
        if (std::count(text.begin(), text.end(), ',') != 2)
            throw std::invalid_argument("Specify three comma-separated wavelengths or widths");
        std::replace(text.begin(), text.end(), ',', ' ');
        std::istringstream in(text);
        std::array<double, 3> result{};
        for (auto& v : result)
            if (!(in >> v))
                throw std::invalid_argument("Invalid passband number");
        spectralEnd(in);
        return result;
    };
    model.centers = triplet("--centers");
    model.widths = triplet("--widths");
    validateNarrowband(model);
    if (model.layout == "extracted-hoo") {
        const auto& green = p.response[1];
        const auto& blue = p.response[2];
        if (green.size() != blue.size())
            throw std::invalid_argument("G/B must share optical components after their sensor QE curves");
        for (std::size_t i = 1; i < green.size(); ++i)
            if (!sameCurve(green[i], blue[i]))
                throw std::invalid_argument(
                    "G/B must share identical optical components; supply an effective response instead");
        const double low = std::max(green[0].wavelength.front(), blue[0].wavelength.front());
        const double high = std::min(green[0].wavelength.back(), blue[0].wavelength.back());
        if (!(low < high))
            throw std::invalid_argument("G/B sensor responses have no common support");
        std::set<double> knots{low, high};
        for (const auto* c : {&green[0], &blue[0]})
            for (double x : c->wavelength)
                if (x > low && x < high)
                    knots.insert(x);
        SpectralCurve combined;
        for (double x : knots) {
            combined.wavelength.push_back(x);
            combined.value.push_back((2 * at(green[0], x) + at(blue[0], x)) / 3);
        }
        auto effective = green;
        effective[0] = std::move(combined);
        p.response[1] = effective;
        p.response[2] = std::move(effective);
    }
    p.name = values.at("--name");
    if (p.name.find_first_not_of(' ') == std::string::npos)
        throw std::invalid_argument("Enter a nonempty profile name");
    p.mode = "narrowband";
    p.quality = "approximate";
    p.narrowband = model;
    p.provenance = "Nominal trapezoid passbands, edge fraction 1e-4; " + model.layout + "; base SHA256 " + sha + "; " +
                   p.provenance;
    spectralText(p.name);
    spectralText(p.provenance);
    for (unsigned c = 0; c < 3; ++c) {
        if (p.response[c].size() >= 8)
            throw std::invalid_argument("Base response has no free component slot for the narrowband filter");
        const auto window = narrowbandWindow(model, c);
        for (const auto& component : p.response[c])
            if (component.wavelength.front() > window.wavelength.front() ||
                component.wavelength.back() < window.wavelength.back())
                throw std::invalid_argument("Base response does not cover every requested passband");
        p.response[c].push_back(window);
        if (integratePhotonSpectrum(p.white, p.response[c]).value <= 0)
            throw std::invalid_argument("Narrowband white signal must be positive");
    }
    std::ostringstream text;
    text << std::setprecision(17) << "PhotonStack-SpectralProfile-2\n"
         << std::quoted(p.name) << ' ' << std::quoted(p.mode) << ' ' << std::quoted(p.quality) << ' '
         << std::quoted(p.provenance) << ' ' << std::quoted(p.license) << '\n'
         << std::quoted(model.layout) << ' ' << model.baseSHA;
    for (unsigned c = 0; c < 3; ++c)
        text << ' ' << model.centers[c] << ' ' << model.widths[c];
    text << '\n';
    auto curve = [&](const SpectralCurve& c) {
        text << c.value.size() << '\n';
        for (std::size_t i = 0; i < c.value.size(); ++i)
            text << c.wavelength[i] << ' ' << c.value[i] << '\n';
    };
    for (const auto& band : p.response) {
        text << band.size() << '\n';
        for (const auto& c : band)
            curve(c);
    }
    text << std::quoted(p.whiteName) << ' ' << std::quoted(p.whiteProvenance) << ' ' << std::quoted(p.whiteLicense)
         << '\n';
    curve(p.white.flux);
    const auto license = source.parent_path() / "response-source-license.txt";
    std::optional<std::string> licenseSHA;
    if (fs::exists(license)) {
        if (!fs::is_regular_file(license) || fs::is_symlink(license) || fs::file_size(license) > 1024 * 1024)
            throw std::invalid_argument("Invalid base response license");
        licenseSHA = fileSHA256(license);
    }
    const auto output = fs::absolute(values.at("--output-dir"));
    if (!fs::create_directory(output))
        throw std::invalid_argument("Choose a new profile output directory");
    try {
        fs::copy_file(source, output / "base-profile.txt");
        if (licenseSHA)
            fs::copy_file(license, output / "response-source-license.txt");
        if (fileSHA256(source) != sha || fileSHA256(output / "base-profile.txt") != sha ||
            (licenseSHA &&
             (fileSHA256(license) != *licenseSHA || fileSHA256(output / "response-source-license.txt") != *licenseSHA)))
            throw std::runtime_error("Base response or license changed during generation");
        {
            std::ofstream out(output / "profile.txt");
            out << text.str();
            out.close();
            if (!out)
                throw std::runtime_error("Failed to write narrowband profile");
        }
        (void)readSpectralProfile(output / "profile.txt");
        std::cout << "{\"type\":\"complete\",\"command\":\"spcc-narrowband-profile\",\"profile\":"
                  << channelJSON((output / "profile.txt").string())
                  << ",\"sha\":" << channelJSON(fileSHA256(output / "profile.txt")) << "}\n";
    } catch (...) {
        fs::remove_all(output);
        throw;
    }
    return 0;
}
