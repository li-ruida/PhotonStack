#include "SkyAnnotationCommand.hpp"
#include "ChannelRecipe.hpp"
#include "photonstack/AstroDevelop.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/ImageResizer.hpp"
#include "photonstack/SkyAnnotation.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
namespace {
using namespace photonstack;
using namespace photonstack::cli;
namespace fs = std::filesystem;
const std::set<std::string> catalogFiles{
    "NGC.csv",        "addendum.csv", "outlines.txt",  "REVISION",           "LICENSE.txt",
    "NOTICE.txt",     "AUTHORS",      "NGC_guide.txt", "UPSTREAM-README.md", "outlines-methodology.txt",
    "provenance.json"};
const std::set<std::string> products{"preview.png", "annotated.svg", "overlay.svg", "report.json"};
struct Recipe {
    fs::path source;
    std::string sha;
    AnnotationOptions options;
    unsigned previewWidth = 1600;
    bool outlines = true, labels = true;
    std::map<std::string, std::string> catalog, products;
};
std::string bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(in), {}};
}
void save(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.close();
    if (!out)
        throw std::runtime_error("Cannot write " + path.string());
}
std::map<std::string, std::string> hashes(const fs::path& dir, const std::set<std::string>& names) {
    std::map<std::string, std::string> out;
    for (const auto& name : names)
        out[name] = fileSHA256(dir / name);
    return out;
}
void verify(const fs::path& dir, const std::map<std::string, std::string>& files) {
    for (const auto& [name, sha] : files)
        if (fileSHA256(dir / name) != sha)
            throw std::runtime_error("Annotation dependency or product changed: " + name);
}
void verifySource(const Recipe& r) {
    if (!r.source.is_absolute() || !channelSHA(r.sha) || fileSHA256(r.source) != r.sha)
        throw std::runtime_error("Annotation source changed");
}
void validate(const Recipe& r) {
    const auto& o = r.options;
    if (r.previewWidth < 320 || r.previewWidth > 4096 || o.maxObjects < 1 || o.maxObjects > 1000 ||
        !std::isfinite(o.limitingMagnitude) || o.limitingMagnitude < -30 || o.limitingMagnitude > 60 ||
        o.query.size() > 256 ||
        !std::set<std::string>{"all", "nebulae", "galaxies", "clusters", "other"}.contains(o.category))
        throw std::invalid_argument("Invalid annotation settings");
}
void writeRecipe(const fs::path& path, const Recipe& r) {
    std::ostringstream out;
    out << std::setprecision(17) << "PhotonStack-SkyAnnotation-1\n"
        << std::quoted(r.source.string()) << ' ' << r.sha << '\n'
        << std::quoted(r.options.category) << ' ' << std::quoted(r.options.query) << ' ' << r.options.maxObjects << ' '
        << r.options.limitingMagnitude << ' ' << r.options.includeUnknownMagnitude << ' ' << r.previewWidth << ' '
        << r.outlines << ' ' << r.labels << '\n';
    for (const auto& [n, s] : r.catalog)
        out << "catalog " << n << ' ' << s << '\n';
    for (const auto& [n, s] : r.products)
        out << "product " << n << ' ' << s << '\n';
    save(path, out.str());
}
Recipe readRecipe(const fs::path& path) {
    if (fs::file_size(path) > 65536)
        throw std::runtime_error("Oversized annotation recipe");
    std::ifstream in(path);
    Recipe r;
    std::string magic, source;
    if (!(in >> magic) || magic != "PhotonStack-SkyAnnotation-1" ||
        !(in >> std::quoted(source) >> r.sha >> std::quoted(r.options.category) >> std::quoted(r.options.query) >>
          r.options.maxObjects >> r.options.limitingMagnitude >> r.options.includeUnknownMagnitude >> r.previewWidth >>
          r.outlines >> r.labels))
        throw std::runtime_error("Invalid annotation recipe");
    r.source = source;
    std::string kind, name, sha;
    while (in >> kind) {
        if (!(in >> name >> sha) || !channelSHA(sha))
            throw std::runtime_error("Invalid annotation digest");
        if (kind == "catalog" && catalogFiles.contains(name) && r.catalog.emplace(name, sha).second)
            continue;
        if (kind == "product" && products.contains(name) && r.products.emplace(name, sha).second)
            continue;
        throw std::runtime_error("Invalid annotation file record");
    }
    if (!in.eof() || r.catalog.size() != catalogFiles.size() || r.products.size() != products.size())
        throw std::runtime_error("Incomplete annotation recipe");
    validate(r);
    verifySource(r);
    verify(path.parent_path() / "catalog", r.catalog);
    verify(path.parent_path(), r.products);
    return r;
}
fs::path defaultCatalog() {
#ifdef __APPLE__
    std::uint32_t n = 0;
    _NSGetExecutablePath(nullptr, &n);
    std::vector<char> path(n);
    if (_NSGetExecutablePath(path.data(), &n) == 0) {
        auto dir = fs::weakly_canonical(path.data()).parent_path().parent_path() / "Resources/OpenNGC";
        if (fs::is_directory(dir))
            return dir;
    }
#endif
#ifdef PHOTONSTACK_CATALOG_DIR
    return PHOTONSTACK_CATALOG_DIR;
#else
    throw std::runtime_error("Specify --catalog-dir pointing to OpenNGC data");
#endif
}
std::string xml(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            if (c >= 32 || c == '\n' || c == '\t')
                out += c;
        }
    }
    return out;
}
std::string base64(const std::string& data) {
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        unsigned v = static_cast<unsigned char>(data[i]) << 16;
        if (i + 1 < data.size())
            v |= static_cast<unsigned char>(data[i + 1]) << 8;
        if (i + 2 < data.size())
            v |= static_cast<unsigned char>(data[i + 2]);
        out += alphabet[v >> 18];
        out += alphabet[(v >> 12) & 63];
        out += i + 1 < data.size() ? alphabet[(v >> 6) & 63] : '=';
        out += i + 2 < data.size() ? alphabet[v & 63] : '=';
    }
    return out;
}
struct Label {
    double x = 0, y = 0, width = 0, anchorX = 0, anchorY = 0;
    bool visible = false;
};
std::vector<Label> layout(const SkyAnnotationResult& result, unsigned width, unsigned height, unsigned pw,
                          unsigned ph) {
    std::vector<Label> labels;
    if (pw < 24 || ph < 24)
        return std::vector<Label>(result.objects.size());
    const double sx = double(pw) / width, sy = double(ph) / height;
    for (const auto& o : result.objects) {
        Label l;
        l.width = std::min(double(pw) - 8, 8. * o.object.label.size() + 10);
        l.anchorX = std::clamp((o.center.x + .5) * sx, 4., pw - 4.);
        l.anchorY = std::clamp((o.center.y + .5) * sy, 4., ph - 4.);
        if (!o.centerVisible && !o.paths.empty()) {
            const auto& p = o.paths.front()[o.paths.front().size() / 2];
            l.anchorX = (p.x + .5) * sx;
            l.anchorY = (p.y + .5) * sy;
        }
        for (double dy : {-24., 8., -48., 32., -72., 56.}) {
            l.x = std::clamp(l.anchorX + 8, 4., pw - l.width - 4);
            l.y = std::clamp(l.anchorY + dy, 4., ph - 24.);
            bool overlap = false;
            for (const auto& other : labels)
                if (other.visible && l.x < other.x + other.width + 3 && l.x + l.width + 3 > other.x &&
                    l.y < other.y + 23 && l.y + 23 > other.y) {
                    overlap = true;
                    break;
                }
            if (!overlap) {
                l.visible = true;
                break;
            }
        }
        labels.push_back(l);
    }
    return labels;
}
std::string color(const DeepSkyObject& o) {
    const auto type = deepSkyCategory(o.type);
    return type == "galaxies" ? "#ffcd78" : type == "clusters" ? "#9dbdff" : "#64e4d5";
}
std::string svg(const Recipe& r, const SkyAnnotationResult& result, const std::vector<Label>& labels, unsigned w,
                unsigned h, unsigned pw, unsigned ph, const std::string& png, const std::string& revision) {
    const double sx = double(pw) / w, sy = double(ph) / h;
    std::ostringstream out;
    out << std::setprecision(10) << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << pw << "\" height=\""
        << ph + 40 << "\" viewBox=\"0 0 " << pw << ' ' << ph + 40
        << "\">\n<title>PhotonStack deep-sky annotations</title>\n";
    out << "<desc>Catalog: OpenNGC, Mattia Verga and contributors. https://github.com/mattiaverga/OpenNGC/tree/"
        << xml(revision)
        << " . Data license: CC BY-SA 4.0 https://creativecommons.org/licenses/by-sa/4.0/ . Catalog shapes projected "
           "and clipped by PhotonStack; approximate boundaries.</desc>\n";
    if (!png.empty())
        out << "<rect width=\"100%\" height=\"100%\" fill=\"#080c10\"/><image width=\"" << pw << "\" height=\"" << ph
            << "\" href=\"data:image/png;base64," << base64(png) << "\"/>\n";
    out << "<g fill=\"none\" stroke-width=\"1.2\">\n";
    for (std::size_t i = 0; i < result.objects.size(); ++i) {
        const auto& o = result.objects[i];
        const auto& l = labels[i];
        out << "<g stroke=\"" << color(o.object) << "\"><title>" << xml(o.object.label + " — " + o.outlineKind)
            << "</title>";
        if (r.outlines)
            for (const auto& path : o.paths) {
                out << "<path d=\"";
                for (std::size_t j = 0; j < path.size(); ++j)
                    out << (j ? " L" : "M") << (path[j].x + .5) * sx << ' ' << (path[j].y + .5) * sy;
                out << "\"/>";
            }
        if (o.centerVisible)
            out << "<circle cx=\"" << (o.center.x + .5) * sx << "\" cy=\"" << (o.center.y + .5) * sy << "\" r=\"4\"/>";
        if (r.labels && l.visible)
            out << "<path opacity=\"0.6\" d=\"M" << l.anchorX << ' ' << l.anchorY << " L" << l.x << ' ' << l.y + 10
                << "\"/>";
        out << "</g>\n";
    }
    out << "</g>\n<g font-family=\"monospace\" font-size=\"13\">";
    if (r.labels)
        for (std::size_t i = 0; i < labels.size(); ++i)
            if (labels[i].visible) {
                const auto& l = labels[i];
                out << "<rect x=\"" << l.x << "\" y=\"" << l.y << "\" width=\"" << l.width
                    << "\" height=\"20\" rx=\"3\" fill=\"#080c10\" fill-opacity=\"0.85\"/><text x=\"" << l.x + 5
                    << "\" y=\"" << l.y + 14 << "\" fill=\"" << color(result.objects[i].object) << "\">"
                    << xml(result.objects[i].object.label) << "</text>";
            }
    out << "</g><rect y=\"" << ph << "\" width=\"" << pw << "\" height=\"40\" fill=\"#080c10\"/><text x=\"8\" y=\""
        << ph + 16
        << "\" font-family=\"sans-serif\" font-size=\"11\" fill=\"#d0d4d8\">OpenNGC · CC BY-SA 4.0</text><text x=\"8\" "
           "y=\""
        << ph + 31
        << "\" font-family=\"sans-serif\" font-size=\"11\" fill=\"#a0a4a8\">Approximate catalog "
           "outlines</text></svg>\n";
    return out.str();
}
std::string report(const Recipe& r, const SkyAnnotationResult& result, const std::vector<Label>& labels, unsigned w,
                   unsigned h, unsigned pw, unsigned ph, const std::string& revision, bool linear) {
    std::ostringstream out;
    out << std::setprecision(17) << std::boolalpha
        << "{\"type\":\"complete\",\"command\":\"sky-annotate\",\"source\":" << channelJSON(r.source.string())
        << ",\"sourceSHA\":" << channelJSON(r.sha) << ",\"width\":" << w << ",\"height\":" << h
        << ",\"previewWidth\":" << pw << ",\"previewHeight\":" << ph << ",\"requestedPreviewWidth\":" << r.previewWidth
        << ",\"linearDisplayPreview\":" << linear << ",\"catalogRevision\":" << channelJSON(revision)
        << ",\"catalogFrame\":\"published-J2000\",\"projectionFrame\":\"ICRS\",\"approximateCatalogGeometry\":true,"
           "\"matched\":"
        << result.matched << ",\"category\":" << channelJSON(r.options.category)
        << ",\"query\":" << channelJSON(r.options.query) << ",\"maxObjects\":" << r.options.maxObjects
        << ",\"limitingMagnitude\":" << r.options.limitingMagnitude
        << ",\"includeUnknownMagnitude\":" << r.options.includeUnknownMagnitude << ",\"showOutlines\":" << r.outlines
        << ",\"showLabels\":" << r.labels << ",\"objects\":[";
    for (std::size_t i = 0; i < result.objects.size(); ++i) {
        if (i)
            out << ',';
        const auto& f = result.objects[i];
        const auto& o = f.object;
        const auto& l = labels[i];
        out << "{\"id\":" << channelJSON(o.id) << ",\"label\":" << channelJSON(o.label)
            << ",\"type\":" << channelJSON(o.type) << ",\"category\":" << channelJSON(deepSkyCategory(o.type))
            << ",\"commonNames\":" << channelJSON(o.commonNames) << ",\"ra\":" << o.ra << ",\"dec\":" << o.dec
            << ",\"majorArcmin\":" << o.majorArcmin << ",\"minorArcmin\":" << o.minorArcmin << ",\"positionAngle\":";
        if (o.positionAngle)
            out << *o.positionAngle;
        else
            out << "null";
        out << ",\"invalidPositionAngle\":" << o.invalidPositionAngle
            << ",\"magnitudeBand\":" << channelJSON(o.magnitudeBand) << ",\"magnitude\":";
        if (o.magnitude)
            out << *o.magnitude;
        else
            out << "null";
        out << ",\"x\":" << f.center.x << ",\"y\":" << f.center.y << ",\"centerVisible\":" << f.centerVisible
            << ",\"outlineKind\":" << channelJSON(f.outlineKind) << ",\"labelX\":" << l.x << ",\"labelY\":" << l.y
            << ",\"labelWidth\":" << l.width << ",\"anchorX\":" << l.anchorX << ",\"anchorY\":" << l.anchorY
            << ",\"labelVisible\":" << l.visible << ",\"paths\":[";
        for (std::size_t j = 0; j < f.paths.size(); ++j) {
            if (j)
                out << ',';
            out << '[';
            for (std::size_t k = 0; k < f.paths[j].size(); ++k) {
                if (k)
                    out << ',';
                out << '[' << f.paths[j][k].x << ',' << f.paths[j][k].y << ']';
            }
            out << ']';
        }
        out << "]}";
    }
    out << "]}\n";
    return out.str();
}
} // namespace
int runSkyAnnotation(const std::vector<std::string>& args) {
    Recipe r;
    fs::path output, recipe, catalog;
    std::set<std::string> seen;
    const bool fresh = args[0] == "sky-annotate";
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto key = args[i];
        if (!seen.insert(key).second || i + 1 == args.size())
            throw std::invalid_argument("Duplicate or incomplete annotation option");
        const auto value = args[++i];
        if (key == "--output-dir")
            output = fs::absolute(value);
        else if (!fresh && key == "--recipe")
            recipe = fs::absolute(value);
        else if (fresh && key == "--input")
            r.source = fs::weakly_canonical(fs::absolute(value));
        else if (fresh && key == "--catalog-dir")
            catalog = fs::weakly_canonical(fs::absolute(value));
        else if (fresh && key == "--category")
            r.options.category = value;
        else if (fresh && key == "--query")
            r.options.query = value;
        else if (fresh && (key == "--include-unknown" || key == "--outlines" || key == "--labels")) {
            if (value != "0" && value != "1")
                throw std::invalid_argument("Boolean options require 0 or 1");
            if (key == "--include-unknown")
                r.options.includeUnknownMagnitude = value == "1";
            else if (key == "--outlines")
                r.outlines = value == "1";
            else
                r.labels = value == "1";
        } else if (fresh && (key == "--max-objects" || key == "--limiting-magnitude" || key == "--preview-width")) {
            std::size_t used = 0;
            double v = std::stod(value, &used);
            if (used != value.size() || !std::isfinite(v))
                throw std::invalid_argument("Invalid numeric option");
            if (key == "--limiting-magnitude")
                r.options.limitingMagnitude = v;
            else {
                if (v < 1 || v > 4096 || v != std::floor(v))
                    throw std::invalid_argument("Invalid integer option");
                if (key == "--max-objects")
                    r.options.maxObjects = unsigned(v);
                else
                    r.previewWidth = unsigned(v);
            }
        } else
            throw std::invalid_argument("Unknown annotation option: " + key);
    }
    if (!fresh) {
        if (recipe.empty())
            throw std::invalid_argument("Specify --recipe");
        r = readRecipe(recipe);
        catalog = recipe.parent_path() / "catalog";
        if (args[0] == "sky-annotation-inspect") {
            if (!output.empty())
                throw std::invalid_argument("Inspect does not accept output");
            std::cout << bytes(recipe.parent_path() / "report.json");
            return 0;
        }
    } else {
        if (r.source.empty())
            throw std::invalid_argument("Specify --input");
        if (catalog.empty())
            catalog = defaultCatalog();
        r.sha = fileSHA256(r.source);
        r.catalog = hashes(catalog, catalogFiles);
    }
    validate(r);
    verifySource(r);
    if (output.empty() || fs::exists(output))
        throw std::invalid_argument("Choose a new annotation directory");
    if (!fs::create_directory(output))
        throw std::runtime_error("Cannot create annotation directory");
    try {
        fs::create_directory(output / "catalog");
        for (const auto& name : catalogFiles)
            fs::copy_file(catalog / name, output / "catalog" / name);
        verify(output / "catalog", r.catalog);
        if (args[0] == "sky-annotation-export") {
            for (const auto& name : products)
                fs::copy_file(recipe.parent_path() / name, output / name);
            verify(output, r.products);
            verifySource(r);
            writeRecipe(output / "recipe.txt", r);
            std::cout << bytes(output / "report.json");
            return 0;
        }
        std::string revision = bytes(output / "catalog/REVISION");
        while (!revision.empty() && std::isspace(static_cast<unsigned char>(revision.back())))
            revision.pop_back();
        if (revision.size() != 40 || revision.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("Invalid catalog revision");
        const auto objects = readOpenNGC(output / "catalog");
        auto input = readDevelopableFits(r.source);
        if (!input.sky)
            throw std::invalid_argument("Annotation requires a solved FITS master with WCS");
        const auto w = input.image.width, h = input.image.height;
        const auto result = annotateSky(*input.sky, w, h, objects, r.options);
        const bool linear = input.image.colorEncoding == ColorEncoding::Linear;
        if (linear) {
            AstroDevelopOptions options;
            options.stellarBalance = false;
            auto developed = AstroDevelop().apply(input.image, options);
            if (!developed.ok)
                throw std::runtime_error(developed.message);
            input.image = std::move(developed.image);
        }
        // Bound both dimensions. Layout and image share the actual rounded resize.
        const auto limit = std::min(1., double(r.previewWidth) / std::max(w, h));
        const auto target = std::max(1u, unsigned(std::round(w * limit)));
        if (target < w) {
            auto resized = ImageResizer().resizeToWidth(input.image, target);
            if (!resized.ok)
                throw std::runtime_error(resized.message);
            input.image = std::move(resized.image);
        }
        const auto pw = input.image.width, ph = input.image.height;
        const auto labels = layout(result, w, h, pw, ph);
        ImageWriteOptions options;
        options.bitDepth = ImageWriteBitDepth::Eight;
        auto saved = ImageCodec().write(input.image, output / "preview.png", options);
        if (!saved.ok)
            throw std::runtime_error(saved.message);
        save(output / "annotated.svg", svg(r, result, labels, w, h, pw, ph, bytes(output / "preview.png"), revision));
        save(output / "overlay.svg", svg(r, result, labels, w, h, pw, ph, "", revision));
        save(output / "report.json", report(r, result, labels, w, h, pw, ph, revision, linear));
        verifySource(r);
        r.products = hashes(output, products);
        writeRecipe(output / "recipe.txt", r);
        std::cout << bytes(output / "report.json");
    } catch (...) {
        std::error_code error;
        fs::remove_all(output, error);
        throw;
    }
    return 0;
}
