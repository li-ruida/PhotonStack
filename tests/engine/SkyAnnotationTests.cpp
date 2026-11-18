#include "photonstack/AstrometricValidation.hpp"
#include "photonstack/SkyAnnotation.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace photonstack;
namespace fs = std::filesystem;
void require(bool ok, const char* reason) {
    if (!ok)
        throw std::runtime_error(reason);
}
template <class F> void rejects(F f, const char* reason) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, reason);
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "catalog argument required");
        const auto catalog = readOpenNGC(argv[1]);
        require(catalog.size() > 12000, "catalog incomplete");
        const auto find = [&](std::string id) -> const DeepSkyObject& {
            for (const auto& o : catalog)
                if (o.id == id)
                    return o;
            throw std::runtime_error("Missing catalog object");
        };
        require(find("IC2105").positionAngle == 179 && !find("NGC6857").positionAngle &&
                    find("NGC6857").invalidPositionAngle,
                "invalid published PA not recorded");
        const auto& m31 = find("NGC0224");
        require(m31.label == "M 31 / NGC 224" && std::abs(m31.ra - 10.6847916667) < 1e-8 &&
                    std::abs(m31.dec - 41.2690555556) < 1e-8,
                "M31 coordinate units or aliases");
        require(!find("NGC6992").outlines.empty() && !find("NGC6960").outlines.empty(), "Veil contours missing");
        DeepSkyObject o;
        o.id = "fixture";
        o.label = "fixture";
        o.type = "G";
        o.ra = 359.9;
        o.dec = 30;
        o.majorArcmin = 60;
        o.minorArcmin = 30;
        o.positionAngle = 0;
        auto ellipse = celestialEllipse(o);
        require(std::abs(ellipse[0].y - 30.5) < 1e-10 && std::abs(ellipse[0].x - 359.9) < 1e-10,
                "PA north or arcmin diameter incorrect");
        o.positionAngle.reset();
        require(celestialEllipse(o).empty(), "unknown PA guessed");
        o.minorArcmin = 60;
        require(!celestialEllipse(o).empty(), "circle needs no PA");
        SolvedPanel sky{{101, 101, 359.9, 30, {-.01, 0, 0, .01}}, {{2, 0, 1e-5, 0}}};
        auto annotation = annotateSky(sky, 200, 200, {o});
        require(annotation.objects.size() == 1 && annotation.objects[0].centerVisible, "RA wrap object omitted");
        require(std::hypot(annotation.objects[0].center.x - 100, annotation.objects[0].center.y - 100) < 1e-8,
                "reference pixel convention");
        const auto beyond = pixelToSky(sky, 240, 100);
        o.ra = beyond.x;
        o.dec = beyond.y;
        o.majorArcmin = o.minorArcmin = 100;
        annotation = annotateSky(sky, 200, 200, {o});
        require(annotation.objects.size() == 1 && !annotation.objects[0].centerVisible &&
                    !annotation.objects[0].paths.empty(),
                "cropped object dropped");
        for (const auto& path : annotation.objects[0].paths)
            for (const auto& p : path)
                require(p.x >= -.50000001 && p.x <= 199.50000001 && p.y >= -.50000001 && p.y <= 199.50000001,
                        "contour not clipped");
        // A single open segment crossing the field must remain an open segment.
        auto left = pixelToSky(sky, -40, 100), right = pixelToSky(sky, 240, 100);
        o.outlines = {{left, right}};
        annotation = annotateSky(sky, 200, 200, {o});
        require(annotation.objects[0].paths.size() == 1 && annotation.objects[0].paths[0].size() == 2,
                "invented contour closure");
        o.ra = 359.9;
        o.dec = 30;
        o.outlines.clear();
        o.majorArcmin = o.minorArcmin = 300;
        annotation = annotateSky(sky, 200, 200, {o});
        require(annotation.objects.size() == 1, "field inside object omitted");
        // Include a point within the outer half-pixel at a wide-field corner.
        SolvedPanel wide{{11, 11, 20, 10, {1, 0, 0, 1}}, {}};
        auto edge = o;
        auto edgeSky = pixelToSky(wide, 19.49, 19.49);
        edge.ra = edgeSky.x;
        edge.dec = edgeSky.y;
        edge.majorArcmin = edge.minorArcmin = 0;
        require(annotateSky(wide, 20, 20, {edge}).objects.size() == 1, "half-pixel field edge omitted");
        auto a = o, b = o;
        a.id = "a";
        a.label = "M 1";
        a.magnitude = 12;
        b.id = "b";
        b.magnitude = 1;
        AnnotationOptions options;
        options.maxObjects = 1;
        annotation = annotateSky(sky, 200, 200, {b, a}, options);
        require(annotation.matched == 2 && annotation.objects[0].object.id == "a",
                "limit or stable ordering incorrect");
        options.category = "nebulae";
        require(annotateSky(sky, 200, 200, {o}, options).objects.empty(), "category filter ignored");
        options.category = "all";
        options.includeUnknownMagnitude = false;
        require(annotateSky(sky, 200, 200, {o}, options).objects.empty(), "unknown magnitude filter ignored");
        options.maxObjects = 0;
        rejects([&] { annotateSky(sky, 200, 200, {o}, options); }, "invalid limit accepted");
        const auto root =
            fs::temp_directory_path() /
            ("annotation-catalog-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directory(root);
        const std::string header =
            "Name;Type;RA;Dec;MajAx;MinAx;PosAng;V-Mag;B-Mag;M;NGC;IC;Identifiers;Common names\n";
        std::ofstream(root / "addendum.csv") << header;
        std::ofstream(root / "outlines.txt") << "PhotonStack-Outlines-1\n";
        std::ofstream(root / "NGC.csv") << header
                                        << "NGC0001;G;00:00:01;-00:30:00;1;1;;12;;;;;\"A;B\";\"quoted \"\"name\"\"\"\n";
        auto small = readOpenNGC(root);
        require(small.size() == 1 && small[0].dec == -.5 && small[0].commonNames == "quoted \"name\"",
                "negative zero or CSV quoting");
        std::ofstream(root / "NGC.csv") << header << "NGC0001;G;00:00:01;+90:01:00;1;1;;12;;;;;a;b\n";
        rejects([&] { readOpenNGC(root); }, "invalid declination accepted");
        fs::remove_all(root);
        std::cout << "Catalog parsing, spherical geometry, clipping and filters passed: " << catalog.size()
                  << " objects\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
