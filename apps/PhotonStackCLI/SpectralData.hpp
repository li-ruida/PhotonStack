#pragma once
#include <charconv>
#include <cstdlib>
#include "ChannelRecipe.hpp"
#include "SpectralNarrowband.hpp"
#include "photonstack/SpectralCalibrator.hpp"
#include <cmath>
namespace photonstack::cli {
struct SpectralProfile {
    std::string name, mode, quality, provenance, license;
    std::string whiteName, whiteProvenance, whiteLicense;
    std::array<SpectralBand, 3> response;
    SampledSpectrum white;
    std::optional<NarrowbandModel> narrowband;
};
struct SpectralCatalog {
    std::string name, provenance, license;
    double epoch = 0;
    std::vector<SpectralReferenceStar> stars;
};
inline void spectralText(const std::string& text, std::size_t limit = 4096) {
    if (text.empty() || text.size() > limit)
        throw std::invalid_argument("Empty or oversized spectral metadata");
    for (unsigned char c : text)
        if (c < 32 || c == 127)
            throw std::invalid_argument("Control characters in spectral metadata");
}
inline void spectralID(const std::string& id) {
    if (id.empty() || id.size() > 128 ||
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-") != std::string::npos)
        throw std::invalid_argument("Invalid spectral source ID");
}
inline void spectralEnd(std::istream& in) {
    std::string extra;
    if (!in || in >> extra || !in.eof())
        throw std::invalid_argument("Incomplete or trailing spectral records");
}
inline SpectralCurve spectralCurve(std::istream& in, std::size_t& total) {
    unsigned n = 0;
    in >> n;
    if (n < 2 || n > 100000 || total + n > 200000)
        throw std::invalid_argument("Invalid spectral response sample count");
    total += n;
    SpectralCurve c;
    c.wavelength.resize(n);
    c.value.resize(n);
    for (unsigned i = 0; i < n; ++i)
        if (!(in >> c.wavelength[i] >> c.value[i]))
            throw std::invalid_argument("Invalid spectral response sample");
    return c;
}
inline SpectralProfile readSpectralProfile(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 16 * 1024 * 1024)
        throw std::invalid_argument("Spectral profile is too large");
    std::ifstream in(path);
    std::string magic;
    SpectralProfile p;
    if (!(in >> magic) || (magic != "PhotonStack-SpectralProfile-1" && magic != "PhotonStack-SpectralProfile-2") ||
        !(in >> std::quoted(p.name) >> std::quoted(p.mode) >> std::quoted(p.quality) >> std::quoted(p.provenance) >>
          std::quoted(p.license)))
        throw std::invalid_argument("Invalid spectral profile header");
    for (const auto& s : {p.name, p.mode, p.quality, p.provenance, p.license})
        spectralText(s);
    if (magic == "PhotonStack-SpectralProfile-2") {
        NarrowbandModel m;
        if (p.mode != "narrowband" || p.quality != "approximate" || !(in >> std::quoted(m.layout) >> m.baseSHA))
            throw std::invalid_argument("Version 2 requires an explicitly approximate narrowband response");
        for (unsigned c = 0; c < 3; ++c)
            if (!(in >> m.centers[c] >> m.widths[c]))
                throw std::invalid_argument("Missing narrowband passband");
        validateNarrowband(m);
        p.narrowband = m;
    } else if (p.mode != "broadband" ||
               !std::set<std::string>{"measured", "approximate", "synthetic"}.contains(p.quality))
        throw std::invalid_argument("Select a broadband profile with measured, approximate or synthetic provenance");
    std::size_t total = 0;
    for (auto& band : p.response) {
        unsigned n = 0;
        in >> n;
        if (n < 1 || n > 8)
            throw std::invalid_argument("Invalid spectral response component count");
        for (unsigned j = 0; j < n; ++j)
            band.push_back(spectralCurve(in, total));
    }
    if (!(in >> std::quoted(p.whiteName) >> std::quoted(p.whiteProvenance) >> std::quoted(p.whiteLicense)))
        throw std::invalid_argument("Missing white-reference provenance");
    for (const auto& s : {p.whiteName, p.whiteProvenance, p.whiteLicense})
        spectralText(s);
    p.white.flux = spectralCurve(in, total);
    spectralEnd(in);
    if (p.narrowband) {
        for (unsigned c = 0; c < 3; ++c) {
            const auto expected = narrowbandWindow(*p.narrowband, c);
            if (p.response[c].size() < 2 || !sameCurve(p.response[c].back(), expected))
                throw std::invalid_argument("Narrowband transmission does not match its declared model");
            for (std::size_t i = 0; i + 1 < p.response[c].size(); ++i)
                if (p.response[c][i].wavelength.front() > expected.wavelength.front() ||
                    p.response[c][i].wavelength.back() < expected.wavelength.back())
                    throw std::invalid_argument("Base response does not cover the complete narrowband filter");
        }
        if (p.narrowband->layout == "extracted-hoo" && !sameBand(p.response[1], p.response[2]))
            throw std::invalid_argument("Extracted HOO requires identical effective OIII responses in G and B");
    }
    for (const auto& band : p.response)
        if (integratePhotonSpectrum(p.white, band).value <= 0)
            throw std::invalid_argument("White reference must give positive signal in each channel");
    return p;
}
template<class Number>
bool spectralNumber(std::istream& in, std::string& token, Number& value) {
    if (!(in >> token)) return false;
    const auto* first = token.data() + (token[0] == '+');
    const auto* last = token.data() + token.size();
    if constexpr (requires { std::from_chars(first, last, value, std::chars_format::general); }) {
        const auto parsed = std::from_chars(first, last, value, std::chars_format::general);
        return parsed.ec == std::errc{} && parsed.ptr == last;
    } else {
        char* end = nullptr;
        value = std::strtod(token.c_str(), &end);
        return end != token.c_str() && end == last;
    }
}

inline SpectralCatalog readSpectralCatalog(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 256 * 1024 * 1024)
        throw std::invalid_argument("Spectral catalog is too large");
    std::ifstream in(path);
    std::string magic;
    SpectralCatalog catalog;
    unsigned count = 0;
    if (!(in >> magic) || magic != "PhotonStack-SpectralCatalog-1" ||
        !(in >> std::quoted(catalog.name) >> catalog.epoch >> std::quoted(catalog.provenance) >>
          std::quoted(catalog.license) >> count) ||
        count < 1 || count > 10000 || !std::isfinite(catalog.epoch) || catalog.epoch < 1800 || catalog.epoch > 2200)
        throw std::invalid_argument("Invalid spectral catalog header or observation epoch");
    for (const auto& s : {catalog.name, catalog.provenance, catalog.license})
        spectralText(s);
    std::set<std::string> ids;
    std::string token;
    std::size_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        SpectralReferenceStar s;
        unsigned n = 0;
        if (!(in >> std::quoted(s.id) >> s.ra >> s.dec >> n) || n < 2 || n > 100000 || total + n > 4000000 ||
            !std::isfinite(s.ra) || !std::isfinite(s.dec) || s.ra < 0 || s.ra >= 360 || s.dec < -90 || s.dec > 90)
            throw std::invalid_argument("Invalid spectral reference record");
        spectralID(s.id);
        if (!ids.insert(s.id).second)
            throw std::invalid_argument("Duplicate spectral reference ID");
        total += n;
        auto& f = s.spectrum;
        f.flux.wavelength.resize(n);
        f.flux.value.resize(n);
        f.error.resize(n);
        for (unsigned j = 0; j < n; ++j)
            if (!spectralNumber(in, token, f.flux.wavelength[j]) || !spectralNumber(in, token, f.flux.value[j]) ||
                !spectralNumber(in, token, f.error[j]) ||
                !std::isfinite(f.flux.wavelength[j]) || f.flux.wavelength[j] <= 0 || f.flux.wavelength[j] > 1e6 ||
                !std::isfinite(f.flux.value[j]) || !std::isfinite(f.error[j]) || f.error[j] < 0 ||
                (j && f.flux.wavelength[j] <= f.flux.wavelength[j - 1]))
                throw std::invalid_argument("Invalid spectral reference sample");
        catalog.stars.push_back(std::move(s));
    }
    spectralEnd(in);
    return catalog;
}
} // namespace photonstack::cli
