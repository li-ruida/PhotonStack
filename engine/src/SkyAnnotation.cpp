#include "photonstack/SkyAnnotation.hpp"
#include "photonstack/AstrometricValidation.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>
namespace photonstack {
namespace {
constexpr double rad = std::numbers::pi / 180;
std::vector<std::string> csvRow(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false, closed = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    quoted = false;
                    closed = true;
                }
            } else
                field += c;
        } else if (c == ';') {
            fields.push_back(field);
            field.clear();
            closed = false;
        } else if (c == '"' && field.empty() && !closed)
            quoted = true;
        else {
            if (closed)
                throw std::runtime_error("Invalid catalog CSV quoting");
            field += c;
        }
    }
    if (quoted)
        throw std::runtime_error("Unterminated catalog CSV field");
    fields.push_back(field);
    return fields;
}
double number(const std::string& s) {
    std::size_t used = 0;
    double v = std::stod(s, &used);
    if (used != s.size() || !std::isfinite(v))
        throw std::runtime_error("Invalid catalog number");
    return v;
}
double sexagesimal(const std::string& s, bool ra) {
    auto a = s.find(':'), b = s.find(':', a == s.npos ? 0 : a + 1);
    if (a == s.npos || b == s.npos)
        throw std::runtime_error("Invalid catalog coordinate");
    const double h = number(s.substr(0, a)), m = number(s.substr(a + 1, b - a - 1)), sec = number(s.substr(b + 1));
    if (m < 0 || m >= 60 || sec < 0 || sec > 60 || (ra && (h < 0 || h >= 24)) || (!ra && std::abs(h) > 90))
        throw std::runtime_error("Catalog coordinate outside range");
    const double result = (std::abs(h) + m / 60 + sec / 3600) * (ra ? 15 : 1) * (s.starts_with('-') ? -1 : 1);
    if ((ra && (result < 0 || result >= 360)) || (!ra && std::abs(result) > 90))
        throw std::runtime_error("Catalog declination outside range");
    return result;
}
std::string displayID(std::string id) {
    for (const std::string prefix : {"NGC", "IC", "M"})
        if (id.starts_with(prefix)) {
            auto rest = id.substr(prefix.size());
            if (rest.empty() || !std::isdigit(static_cast<unsigned char>(rest[0])))
                continue;
            const auto n = rest.find_first_not_of('0');
            if (n != std::string::npos)
                rest.erase(0, n);
            else
                rest = "0";
            return prefix + " " + rest;
        }
    return id;
}
std::string searchKey(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) || c == '-' || c == '_'; }),
            s.end());
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool inImage(const SkyPoint& p, unsigned w, unsigned h) {
    return std::isfinite(p.x) && std::isfinite(p.y) && p.x >= -.5 && p.y >= -.5 && p.x <= w - .5 && p.y <= h - .5;
}
SkyPoint project(const SolvedPanel& sky, const SkyPoint& p) {
    return SkyProjection(sky, {1, 1, p.x, p.y, {1, 0, 0, 1}}).toPanel(0, 0);
}
// Liang-Barsky clips only the actual segment. Never invent a closed edge along
// the image border, which could be mistaken for a nebula contour.
bool clip(SkyPoint& a, SkyPoint& b, unsigned w, unsigned h) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    double lo = 0, hi = 1;
    const std::array<double, 4> p{-dx, dx, -dy, dy}, q{a.x + .5, w - .5 - a.x, a.y + .5, h - .5 - a.y};
    for (unsigned i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0)
                return false;
        } else {
            const double t = q[i] / p[i];
            if (p[i] < 0)
                lo = std::max(lo, t);
            else
                hi = std::min(hi, t);
            if (lo > hi)
                return false;
        }
    }
    const SkyPoint start = a;
    a = {start.x + lo * dx, start.y + lo * dy};
    b = {start.x + hi * dx, start.y + hi * dy};
    return true;
}
bool contains(const std::vector<SkyPoint>& polygon, SkyPoint p) {
    if (polygon.size() < 4 ||
        std::hypot(polygon.front().x - polygon.back().x, polygon.front().y - polygon.back().y) > 1e-5)
        return false;
    bool inside = false;
    for (std::size_t i = 1; i < polygon.size(); ++i) {
        const auto a = polygon[i - 1], b = polygon[i];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}
double separation(SkyPoint a, SkyPoint b) {
    const double x = std::sin((a.y - b.y) * rad / 2), y = std::sin((a.x - b.x) * rad / 2);
    return 2 * std::asin(std::sqrt(std::clamp(x * x + std::cos(a.y * rad) * std::cos(b.y * rad) * y * y, 0., 1.))) /
           rad;
}
} // namespace
std::string deepSkyCategory(const std::string& t) {
    if (t == "G" || t == "GPair" || t == "GTrpl" || t == "GGroup")
        return "galaxies";
    if (t == "OCl" || t == "GCl" || t == "*Ass")
        return "clusters";
    if (t == "PN" || t == "HII" || t == "DrkN" || t == "EmN" || t == "Neb" || t == "RfN" || t == "SNR" || t == "Cl+N")
        return "nebulae";
    return "other";
}
std::vector<DeepSkyObject> readOpenNGC(const std::filesystem::path& directory) {
    std::vector<DeepSkyObject> result;
    std::map<std::string, std::size_t> byID;
    for (const auto* file : {"NGC.csv", "addendum.csv"}) {
        const auto path = directory / file;
        if (std::filesystem::file_size(path) > 16 * 1024 * 1024)
            throw std::runtime_error("Oversized object catalog");
        std::ifstream in(path);
        std::string line;
        if (!std::getline(in, line))
            throw std::runtime_error("Missing catalog header");
        if (line.ends_with('\r'))
            line.pop_back();
        const auto columns = csvRow(line);
        std::map<std::string, std::size_t> keys;
        for (std::size_t i = 0; i < columns.size(); ++i)
            if (!keys.emplace(columns[i], i).second)
                throw std::runtime_error("Duplicate catalog column");
        for (const auto* key : {"Name", "Type", "RA", "Dec", "MajAx", "MinAx", "PosAng", "V-Mag", "B-Mag", "M", "NGC",
                                "IC", "Identifiers", "Common names"})
            if (!keys.contains(key))
                throw std::runtime_error("Missing catalog column: " + std::string(key));
        std::size_t rows = 0;
        while (std::getline(in, line)) {
            if (line.ends_with('\r'))
                line.pop_back();
            if (line.empty())
                continue;
            if (line.size() > 65536 || ++rows > 50000)
                throw std::runtime_error("Oversized catalog row count or text");
            const auto fields = csvRow(line);
            if (fields.size() != columns.size())
                throw std::runtime_error("Inconsistent catalog columns");
            auto value = [&](const std::string& k) -> const std::string& { return fields[keys.at(k)]; };
            const auto& type = value("Type");
            if (type == "NonEx" || type == "Dup" || type == "*" || type == "**" || type == "Nova" ||
                value("RA").empty() || value("Dec").empty())
                continue;
            DeepSkyObject o;
            o.id = value("Name");
            if (o.id.empty() || o.id.size() > 64)
                throw std::runtime_error("Invalid catalog identifier");
            o.type = type;
            o.label = displayID(o.id);
            o.commonNames = value("Common names");
            o.aliases = value("Identifiers");
            if (!value("M").empty()) {
                const auto m = displayID("M" + value("M"));
                o.label = m + " / " + o.label;
                o.aliases += "," + m;
            }
            for (const auto* key : {"NGC", "IC"})
                if (!value(key).empty())
                    o.aliases += "," + std::string(key) + value(key);
            o.ra = sexagesimal(value("RA"), true);
            o.dec = sexagesimal(value("Dec"), false);
            if (!value("MajAx").empty())
                o.majorArcmin = number(value("MajAx"));
            if (!value("MinAx").empty())
                o.minorArcmin = number(value("MinAx"));
            if (o.majorArcmin < 0 || o.majorArcmin > 1800 || o.minorArcmin < 0 || o.minorArcmin > 1800)
                throw std::runtime_error("Invalid catalog diameter");
            if (!value("PosAng").empty()) {
                const double angle = number(value("PosAng"));
                if (angle < 0 || angle >= 360)
                    o.invalidPositionAngle = true;
                else
                    o.positionAngle = std::fmod(angle, 180.);
            }
            for (const auto* key : {"V-Mag", "B-Mag"})
                if (!value(key).empty()) {
                    o.magnitudeBand = std::string(key).substr(0, 1);
                    o.magnitude = number(value(key));
                    if (*o.magnitude < -30 || *o.magnitude > 60)
                        throw std::runtime_error("Invalid catalog magnitude");
                    break;
                }
            if (!byID.emplace(o.id, result.size()).second)
                throw std::runtime_error("Duplicate catalog identifier");
            result.push_back(std::move(o));
        }
        if (!in.eof())
            throw std::runtime_error("Cannot read object catalog");
    }
    const auto path = directory / "outlines.txt";
    if (std::filesystem::file_size(path) > 16 * 1024 * 1024)
        throw std::runtime_error("Oversized catalog outlines");
    std::ifstream in(path);
    std::string magic, id;
    if (!(in >> magic) || magic != "PhotonStack-Outlines-1")
        throw std::runtime_error("Invalid outline catalog");
    std::size_t total = 0, n = 0;
    while (in >> id) {
        if (!(in >> n) || n < 3 || n > 50000 || total + n > 500000)
            throw std::runtime_error("Invalid catalog contour size");
        total += n;
        std::vector<SkyPoint> points(n);
        for (auto& p : points)
            if (!(in >> p.x >> p.y) || !std::isfinite(p.x) || !std::isfinite(p.y) || p.x < 0 || p.x >= 360 ||
                std::abs(p.y) > 90)
                throw std::runtime_error("Invalid contour coordinate");
        if (auto at = byID.find(id); at != byID.end())
            result[at->second].outlines.push_back(std::move(points));
        // Some published outline IDs cover a broader complex than one NGC entry.
        // Unmapped IDs are not silently assigned to a nearby target.
    }
    if (!in.eof())
        throw std::runtime_error("Truncated outline catalog");
    return result;
}
std::vector<SkyPoint> celestialEllipse(const DeepSkyObject& o, unsigned samples) {
    if (samples < 16 || samples > 4096)
        throw std::invalid_argument("Invalid ellipse sampling");
    if (o.majorArcmin <= 0 || o.minorArcmin <= 0 ||
        (!o.positionAngle && std::abs(o.majorArcmin - o.minorArcmin) > 1e-9))
        return {};
    const double a = o.ra * rad, d = o.dec * rad, pa = o.positionAngle.value_or(0) * rad;
    std::vector<SkyPoint> result;
    result.reserve(samples + 1);
    for (unsigned i = 0; i <= samples; ++i) {
        const double t = 2 * std::numbers::pi * i / samples, u = o.majorArcmin / 120 * std::cos(t),
                     v = o.minorArcmin / 120 * std::sin(t);
        const double e = u * std::sin(pa) + v * std::cos(pa), n = u * std::cos(pa) - v * std::sin(pa),
                     rho = std::hypot(e, n) * rad, f = rho > 0 ? std::sin(rho) / rho : 1;
        const double x =
            std::cos(rho) * std::cos(d) * std::cos(a) + f * rad * (-e * std::sin(a) - n * std::sin(d) * std::cos(a));
        const double y =
            std::cos(rho) * std::cos(d) * std::sin(a) + f * rad * (e * std::cos(a) - n * std::sin(d) * std::sin(a));
        const double z = std::cos(rho) * std::sin(d) + f * rad * n * std::cos(d);
        double ra = std::atan2(y, x) / rad;
        if (ra < 0)
            ra += 360;
        result.push_back({ra, std::atan2(z, std::hypot(x, y)) / rad});
    }
    return result;
}
SkyAnnotationResult annotateSky(const SolvedPanel& sky, unsigned width, unsigned height,
                                const std::vector<DeepSkyObject>& catalog, const AnnotationOptions& options) {
    if (!std::set<std::string>{"all", "galaxies", "nebulae", "clusters", "other"}.contains(options.category) ||
        !options.maxObjects || options.maxObjects > 1000 || !std::isfinite(options.limitingMagnitude) ||
        options.limitingMagnitude < -30 || options.limitingMagnitude > 60 || options.query.size() > 256)
        throw std::invalid_argument("Invalid annotation filters");
    validateAstrometricGrid(sky, width, height);
    // Bound the complete pixel rectangle, including the half-pixel edges.
    // Absolute polynomial terms give a conservative SIP bound; sampling a few
    // corners could otherwise omit an object near a curved image boundary.
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
    const double radius = std::atan(std::hypot(xi, eta) * rad) / rad;
    const SkyPoint middle{g.ra, g.dec};
    const auto query = searchKey(options.query);
    SkyAnnotationResult result;
    for (const auto& object : catalog) {
        if (options.category != "all" && deepSkyCategory(object.type) != options.category)
            continue;
        if (object.magnitude ? *object.magnitude > options.limitingMagnitude : !options.includeUnknownMagnitude)
            continue;
        if (!query.empty() &&
            searchKey(object.id + " " + object.label + " " + object.commonNames + " " + object.aliases).find(query) ==
                std::string::npos)
            continue;
        double extent = std::max(object.majorArcmin, object.minorArcmin) / 120;
        for (const auto& shape : object.outlines)
            for (const auto& point : shape)
                extent = std::max(extent, separation({object.ra, object.dec}, point));
        if (separation(middle, {object.ra, object.dec}) > radius + extent + .01)
            continue;
        SkyAnnotationObject found;
        found.object = object;
        found.center = project(sky, {object.ra, object.dec});
        found.centerVisible = inImage(found.center, width, height);
        bool visible = found.centerVisible;
        auto outlines = object.outlines;
        if (!outlines.empty())
            found.outlineKind = "published-contour";
        else if (auto ellipse = celestialEllipse(object); !ellipse.empty()) {
            found.outlineKind = "catalog-ellipse";
            outlines.push_back(std::move(ellipse));
        }
        for (const auto& world : outlines) {
            std::vector<SkyPoint> pixels;
            pixels.reserve(world.size());
            bool finite = true;
            for (const auto& point : world) {
                const auto p = project(sky, point);
                pixels.push_back(p);
                finite &= std::isfinite(p.x) && std::isfinite(p.y);
            }
            if (finite)
                visible |= contains(pixels, {(width - 1.) / 2, (height - 1.) / 2});
            std::vector<SkyPoint> stroke;
            for (std::size_t i = 1; i < pixels.size(); ++i) {
                auto a = pixels[i - 1], b = pixels[i];
                if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y) ||
                    !clip(a, b, width, height)) {
                    if (stroke.size() > 1)
                        found.paths.push_back(std::move(stroke));
                    stroke.clear();
                    continue;
                }
                visible = true;
                if (stroke.empty())
                    stroke.push_back(a);
                else if (std::hypot(stroke.back().x - a.x, stroke.back().y - a.y) > 1e-6) {
                    found.paths.push_back(std::move(stroke));
                    stroke = {a};
                }
                stroke.push_back(b);
            }
            if (stroke.size() > 1)
                found.paths.push_back(std::move(stroke));
        }
        if (visible && std::isfinite(found.center.x) && std::isfinite(found.center.y))
            result.objects.push_back(std::move(found));
    }
    std::stable_sort(result.objects.begin(), result.objects.end(), [](const auto& a, const auto& b) {
        const bool am = a.object.label.starts_with("M "), bm = b.object.label.starts_with("M ");
        if (am != bm)
            return am;
        if (a.object.magnitude.has_value() != b.object.magnitude.has_value())
            return a.object.magnitude.has_value();
        if (a.object.magnitude && a.object.magnitude != b.object.magnitude)
            return *a.object.magnitude < *b.object.magnitude;
        return a.object.id < b.object.id;
    });
    result.matched = result.objects.size();
    if (result.objects.size() > options.maxObjects)
        result.objects.resize(options.maxObjects);
    return result;
}
} // namespace photonstack
