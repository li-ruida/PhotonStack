#include "SpectralCatalogCommand.hpp"
#include "SpectralData.hpp"
#include "photonstack/AstrometricValidation.hpp"
#include "photonstack/FitsCodec.hpp"
#include <iostream>
#include <numbers>

int runSpectralCatalog(const std::vector<std::string>& args) {
    using namespace photonstack;
    using namespace photonstack::cli;
    const bool field = args[0] == "spcc-field";
    const std::string input = field ? "--input" : "--catalog";
    std::map<std::string, std::string> values;
    for (std::size_t i = 1; i < args.size(); i += 2)
        if (i + 1 >= args.size() || (args[i] != input && args[i] != "--expected-sha") ||
            !values.emplace(args[i], args[i + 1]).second)
            throw std::invalid_argument("Invalid spectral field/catalog inspection option");
    if (!values.contains(input))
        throw std::invalid_argument("Specify " + input);
    const auto path = std::filesystem::canonical(values.at(input));
    const auto sha = fileSHA256(path);
    if (values.contains("--expected-sha") && values.at("--expected-sha") != sha)
        throw std::runtime_error("Spectral input changed");
    std::ostringstream out;
    out << std::setprecision(17) << "{\"type\":\"complete\",\"schema\":1,\"command\":" << channelJSON(args[0])
        << ",\"sha\":" << channelJSON(sha);
    if (field) {
        const auto metadata = FitsCodec().inspect(path);
        if (!metadata.ok || (metadata.metadata.channels != 3 && metadata.metadata.channels != 4))
            throw std::invalid_argument("Choose a solved linear RGB FITS master");
        const auto source = readLinearScienceImage(path);
        if (!source.sky)
            throw std::invalid_argument("A solved ICRS WCS is required to query Gaia");
        const auto& sky = *source.sky;
        const auto width = source.image.width, height = source.image.height;
        validateAstrometricGrid(sky, width, height);
        // A conservative bound around CRVAL, including all polynomial SIP terms
        // and half-pixel edges. Four corner samples alone are not sufficient.
        const auto& g = sky.grid;
        const double u = std::max(std::abs(.5 - g.x), std::abs(width + .5 - g.x));
        const double v = std::max(std::abs(.5 - g.y), std::abs(height + .5 - g.y));
        double ub = u, vb = v;
        for (const auto& t : sky.distortion) {
            const double bound = std::pow(u, t.i) * std::pow(v, t.j);
            ub += std::abs(t.a) * bound;
            vb += std::abs(t.b) * bound;
        }
        const double xi = std::abs(g.cd[0]) * ub + std::abs(g.cd[1]) * vb;
        const double eta = std::abs(g.cd[2]) * ub + std::abs(g.cd[3]) * vb;
        constexpr double radians = std::numbers::pi / 180;
        const double radius = std::atan(std::hypot(xi, eta) * radians) / radians;
        if (!std::isfinite(radius) || radius <= 0 || radius > 20)
            throw std::invalid_argument("Gaia query currently supports fields with a bounding radius up to 20 degrees");
        out << ",\"width\":" << width << ",\"height\":" << height << ",\"ra\":" << g.ra << ",\"dec\":" << g.dec
            << ",\"radius\":" << radius;
    } else {
        const auto catalog = readSpectralCatalog(path);
        out << ",\"count\":" << catalog.stars.size() << ",\"epoch\":" << catalog.epoch
            << ",\"name\":" << channelJSON(catalog.name);
    }
    out << '}';
    if (fileSHA256(path) != sha)
        throw std::runtime_error("Spectral input changed during inspection");
    std::cout << out.str() << '\n';
    return 0;
}
