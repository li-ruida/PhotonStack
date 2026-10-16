#include "../../engine/src/ChromaticBeaconDetector.hpp"
#include "photonstack/ArtifactTrailRemover.hpp"
#include "photonstack/ImageCodec.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

using namespace photonstack;
void require(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
ImageBuffer sky() {
    ImageBuffer im;
    im.width = 960;
    im.height = 640;
    im.channels = 4;
    im.pixels.resize(im.sampleCount());
    for (unsigned y = 0; y < im.height; ++y)
        for (unsigned x = 0; x < im.width; ++x) {
            auto p = (y * im.width + x) * 4;
            const float b = .025F + .12F * y / im.height;
            im.pixels[p] = b;
            im.pixels[p + 1] = b * 1.2F;
            im.pixels[p + 2] = b * 1.5F;
            im.pixels[p + 3] = 1;
            if (y > 565)
                for (int c = 0; c < 3; ++c)
                    im.pixels[p + c] += (x % 30 < 16 ? .65F : .08F);
        }
    return im;
}
void spot(ImageBuffer& im, double x, double y, std::array<double, 3> color, double sigma = .8) {
    for (int iy = int(y) - 4; iy <= int(y) + 4; ++iy)
        for (int ix = int(x) - 4; ix <= int(x) + 4; ++ix) {
            if (ix < 0 || iy < 0 || ix >= int(im.width) || iy >= int(im.height))
                continue;
            const double g = std::exp(-((ix - x) * (ix - x) + (iy - y) * (iy - y)) / (2 * sigma * sigma));
            for (int c = 0; c < 3; ++c)
                im.pixels[(iy * im.width + ix) * 4 + c] += float(g * color[c]);
        }
}
void stars(ImageBuffer& im) {
    for (int i = 0; i < 70; ++i) {
        const double r = 90 + (i * 73) % 470, a = i * 2.399;
        for (int k = 0; k < 22; ++k) {
            const double theta = a + k * .0015;
            spot(im, 480 + r * std::cos(theta), 100 + r * std::sin(theta), {.007, .008, .01}, .7);
        }
    }
}
void plane(ImageBuffer& im, bool vertical = false, bool curved = true) {
    for (int i = 0; i < 28; ++i) {
        // Missing intervals model interruptions between exposure groups.
        if (i >= 12 && i <= 15)
            continue;
        const double t = 30 + i * 30, bend = curved ? 9 * std::pow((t - 440) / 440, 2) : 0;
        const double x = vertical ? 200 + t * .17 : t;
        const double y = vertical ? 30 + t * .56 : 160 + t * .24 + bend;
        spot(im, x, y, {.047, .004, .002});
        spot(im, x + (vertical ? 1 : 12), y + (vertical ? 8 : 1), {.008, .035, .014});
    }
}
// Optional, private-fixture acceptance: fixed human-reviewed regions, never
// detector-produced coordinates. No image bytes are embedded in the repo.
int reviewRegions(const char* sourcePath, const char* resultPath, const char* regionPath) {
    const auto source = ImageCodec().read(sourcePath), result = ImageCodec().read(resultPath);
    require(source.ok && result.ok, "review inputs decode");
    const auto& a = source.image;
    const auto& b = result.image;
    require(a.width == b.width && a.height == b.height && a.channels == b.channels && a.channels >= 3,
            "review images have matching native dimensions and channels");
    std::ifstream regions(regionPath);
    require(bool(regions), "review region manifest exists");
    bool allPassed = true, first = true;
    unsigned count = 0;
    std::cout << std::setprecision(9) << "{\"regions\":[";
    for (std::string line; std::getline(regions, line);) {
        if (line.empty() || line.front() == '#')
            continue;
        std::istringstream row(line);
        std::string mode, name;
        int x, y, w, h;
        require(bool(row >> mode >> name >> x >> y >> w >> h), "review manifest row parses");
        require(name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos,
                "review region names are safe JSON labels");
        require(x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= int(a.width) && y + h <= int(a.height),
                "review region is inside image");
        std::size_t changed = 0;
        double delta = 0, sourcePeak = 0, resultPeak = 0;
        std::array<double, 3> background{};
        if (mode == "lamp") {
            require(y >= 16 && y + h + 16 < int(a.height), "lamp has background margins");
            for (unsigned c = 0; c < 3; ++c) {
                std::vector<float> values;
                for (int px = x; px < x + w; ++px)
                    for (int dy : {-16, -12, h + 12, h + 16})
                        values.push_back(a.pixels[(std::size_t(y + dy) * a.width + px) * a.channels + c]);
                std::sort(values.begin(), values.end());
                background[c] = (values[values.size() / 2] + values[(values.size() - 1) / 2]) * .5;
            }
        }
        for (int py = y; py < y + h; ++py)
            for (int px = x; px < x + w; ++px)
                for (unsigned c = 0; c < 3; ++c) {
                    const auto p = (std::size_t(py) * a.width + px) * a.channels + c;
                    changed += a.pixels[p] != b.pixels[p];
                    delta = std::max(delta, double(std::abs(a.pixels[p] - b.pixels[p])));
                    sourcePeak = std::max(sourcePeak, a.pixels[p] - background[c]);
                    resultPeak = std::max(resultPeak, b.pixels[p] - background[c]);
                }
        bool passed = false;
        if (mode == "exact")
            passed = changed == 0;
        else if (mode == "lamp")
            passed = sourcePeak >= .003 && resultPeak <= std::max(.0025, sourcePeak * .15);
        else
            require(false, "known review mode");
        if (!first)
            std::cout << ',';
        first = false;
        std::cout << "{\"name\":\"" << name << "\",\"mode\":\"" << mode << "\",\"changedSamples\":" << changed
                  << ",\"maxDelta\":" << delta << ",\"sourcePeak\":" << sourcePeak << ",\"resultPeak\":" << resultPeak
                  << ",\"passed\":" << (passed ? "true" : "false") << '}';
        allPassed = allPassed && passed;
        ++count;
    }
    require(count > 0, "review manifest cannot be empty");
    std::cout << "],\"passed\":" << (allPassed ? "true" : "false") << "}\n";
    return allPassed ? 0 : 1;
}
int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--regions")
        return reviewRegions(argv[2], argv[3], argv[4]);
    if (argc == 3) {
        const auto a = ImageCodec().read(argv[1]);
        const auto b = ImageCodec().read(argv[2]);
        require(a.ok && b.ok, "read both pixel-equivalence fixtures");
        require(a.image.width == b.image.width && a.image.height == b.image.height &&
                    a.image.channels == b.image.channels && a.image.pixels == b.image.pixels,
                "all decoded pixels must be exactly equal");
        std::cout << "Decoded pixels exactly equal\n";
        return 0;
    }
    if (argc == 2) {
        const auto input = ImageCodec().read(argv[1]);
        require(input.ok, "read real fixture");
        const auto trails = detail::detectChromaticBeaconChains(input.image);
        for (const auto& t : trails)
            std::cout << t.x1 << ',' << t.y1 << " -> " << t.x2 << ',' << t.y2 << " length=" << t.length << '\n';
        std::cout << "real fixture chains=" << trails.size() << '\n';
        return 0;
    }
    auto clean = sky();
    stars(clean);
    require(detail::detectChromaticBeaconChains(clean).empty(), "rotating stars and a bright city are protected");
    for (bool vertical : {false, true})
        for (bool curved : {false, true}) {
            auto im = clean;
            plane(im, vertical, curved);
            const auto input = im.pixels;
            const auto trails = detail::detectChromaticBeaconChains(im);
            std::cout << "vertical=" << vertical << " curved=" << curved << " chains=" << trails.size() << std::endl;
            require(trails.size() == 1, "one faint beacon chain despite bright foreground and exposure gap");
            require(im.pixels == input, "detection must not mutate input");
            require(trails.front().path.size() > 10, "curved geometry is returned, not just an endpoint chord");
            double worst = 0;
            for (int i = 1; i < 27; ++i) {
                const double t = 30 + i * 30, bend = curved ? 9 * std::pow((t - 440) / 440, 2) : 0;
                const double x = vertical ? 200 + t * .17 : t, y = vertical ? 30 + t * .56 : 160 + t * .24 + bend;
                double closest = 1e9;
                for (const auto& p : trails.front().path)
                    closest = std::min(closest, std::hypot(p.x - x, p.y - y));
                worst = std::max(worst, closest);
            }
            require(worst < 10, "path covers every exposure segment and not just the brightest cluster");
            auto transformed = im;
            transformed.width = im.height;
            transformed.height = im.width;
            for (unsigned y = 0; y < im.height; ++y)
                for (unsigned x = 0; x < im.width; ++x)
                    for (int c = 0; c < 4; ++c) {
                        const float gain = c == 3 ? 1.F : (c == 0 ? .38F : c == 1 ? .45F : .52F);
                        transformed.pixels[((im.width - 1 - x) * transformed.width + y) * 4 + c] =
                            im.pixels[(y * im.width + x) * 4 + c] * gain;
                    }
            require(detail::detectChromaticBeaconChains(transformed).size() == 1,
                    "rotation plus reduced exposure and changed white balance preserve the detection");
        }
    auto meteor = clean;
    auto changingCadence = clean;
    double cadenceX = 10;
    for (int i = 0; i < 28; ++i) {
        spot(changingCadence, cadenceX, 200 + cadenceX * .12, {.05, .003, .002});
        spot(changingCadence, cadenceX + 7, 202 + cadenceX * .12, {.004, .04, .009});
        cadenceX += i < 13 ? 20 : i == 13 ? 32 : 48;
    }
    require(detail::detectChromaticBeaconChains(changingCadence).size() == 1,
            "independent red and green evidence survives a change in exposure cadence");
    auto chromaNoise = clean;
    for (int i = 0; i < 28; ++i) {
        const double x = 30 + i * 30, y = 200 + x * .12;
        // Coherent chroma-only marks are a stronger adversary than random
        // noise: geometry, regular cadence and both colors all look valid.
        spot(chromaNoise, x, y, {.03, -.015, -.015});
        spot(chromaNoise, x + 7, y + 2, {-.015, .03, -.015});
    }
    require(detail::detectChromaticBeaconChains(chromaNoise).empty(),
            "correlated chroma redistribution is not an added navigation light");
    auto wings = sky();
    for (int x = 30; x <= 840; x += 30) {
        spot(wings, x, 200, {.05, .004, .002});
        spot(wings, x + 12, 201, {.008, .035, .014});
    }
    for (int x = 40; x <= 190; x += 30)
        spot(wings, x, 211, {.003, .035, .007});
    spot(wings, 700, 211, {.003, .035, .007});
    const auto wingTrails = detail::detectChromaticBeaconChains(wings);
    require(wingTrails.size() == 1, "opposite-wing lights keep a single flight identity");
    auto wingsRepaired = wings;
    detail::repairChromaticBeaconChains(wings, wingsRepaired, wingTrails);
    const auto emptySky = sky();
    for (int x = 40; x <= 190; x += 30) {
        const auto p = (211 * wings.width + x) * 4 + 1;
        require(wingsRepaired.pixels[p] - emptySky.pixels[p] < .005,
                "supported opposite-wing lamps outside the red corridor are removed");
    }
    for (int y = 207; y <= 215; ++y)
        for (int x = 696; x <= 704; ++x)
            for (int c = 0; c < 4; ++c) {
                const auto p = (y * wings.width + x) * 4 + c;
                require(wingsRepaired.pixels[p] == wings.pixels[p],
                        "an isolated colored star beside the flight is not a companion chain");
            }
    auto occludedFlight = sky();
    plane(occludedFlight, false, false);
    for (unsigned y = 300; y < occludedFlight.height; ++y)
        for (unsigned x = 815; x < occludedFlight.width; ++x)
            for (int c = 0; c < 3; ++c)
                occludedFlight.pixels[(y * occludedFlight.width + x) * 4 + c] = .018F + .008F * ((x / 7 + y / 7) % 2);
    const auto occludedTrails = detail::detectChromaticBeaconChains(occludedFlight);
    require(occludedTrails.size() == 1, "a flight partially behind a dark foreground remains detectable");
    auto occludedRepaired = occludedFlight;
    detail::repairChromaticBeaconChains(occludedFlight, occludedRepaired, occludedTrails);
    for (unsigned y = 300; y < occludedFlight.height; ++y)
        for (unsigned x = 815; x < occludedFlight.width; ++x)
            for (int c = 0; c < 4; ++c) {
                const auto p = (y * occludedFlight.width + x) * 4 + c;
                require(occludedRepaired.pixels[p] == occludedFlight.pixels[p],
                        "repair must not carve a dark foreground behind a verified flight");
            }
    auto weakEnds = clean;
    for (int i = 0; i < 28; ++i) {
        const double x = 30 + i * 30, y = 180 + x * .2;
        const double gain = i >= 9 && i <= 18 ? 1 : .052;
        spot(weakEnds, x, y, {.047 * gain, .004 * gain, .002 * gain});
        spot(weakEnds, x + 12, y + 1, {.008 * gain, .035 * gain, .014 * gain});
    }
    const auto extended = detail::detectChromaticBeaconChains(weakEnds);
    require(extended.size() == 1, "a strong central lamp chain authenticates its faint continuation");
    std::cout << "weak endpoints=" << extended.front().x1 << ',' << extended.front().x2 << std::endl;
    require(std::min(extended.front().x1, extended.front().x2) < 50 &&
                std::max(extended.front().x1, extended.front().x2) > 810,
            "weak endpoints are followed by evidence rather than a fixed total extension distance");
    auto bendingEnds = clean;
    for (int i = 0; i < 28; ++i) {
        const double x = 30 + i * 30;
        const double outside = std::max({0., 300 - x, x - 570});
        const double y = 180 + x * .2 + .0004 * outside * outside;
        const double gain = outside == 0 ? 1 : .052;
        spot(bendingEnds, x, y, {.047 * gain, .004 * gain, .002 * gain});
        spot(bendingEnds, x + 12, y + 1, {.008 * gain, .035 * gain, .014 * gain});
    }
    const auto curvedEnds = detail::detectChromaticBeaconChains(bendingEnds);
    require(curvedEnds.size() == 1, "curving weak tails retain one aircraft identity");
    for (double x : {30., 60., 810., 840.}) {
        const double outside = std::max({0., 300 - x, x - 570});
        const double y = 180 + x * .2 + .0004 * outside * outside;
        double distance = 1e9;
        for (const auto& p : curvedEnds.front().path)
            distance = std::min(distance, std::hypot(x - p.x, y - p.y));
        std::cout << "curved endpoint x=" << x << " distance=" << distance << std::endl;
        require(distance < 12, "curved terminal flashes remain inside the narrow repair corridor");
    }
    for (int i = 0; i < 280; ++i)
        spot(meteor, 150 + i, 270 + i * .3, {.013, .002, .001});
    require(detail::detectChromaticBeaconChains(meteor).empty(), "a continuous red meteor is not a blinking aircraft");
    auto redOnly = clean;
    for (int i = 0; i < 28; ++i)
        spot(redOnly, 30 + i * 30, 180 + i * 7, {.05, .003, .002});
    require(detail::detectChromaticBeaconChains(redOnly).empty(),
            "red-only periodic stellar fragments need independent navigation-light evidence");
    auto shortChain = clean;
    for (int i = 0; i < 4; ++i) {
        spot(shortChain, 200 + i * 30, 300, {.05, .003, .002});
        spot(shortChain, 212 + i * 30, 297, {.005, .05, .01});
    }
    require(detail::detectChromaticBeaconChains(shortChain).empty(), "too few colored points are not enough evidence");
    auto fringedStreak = clean;
    plane(fringedStreak, false, false);
    for (int x = 25; x < 875; ++x)
        spot(fringedStreak, x, 160 + x * .24, {.025, .03, .035}, .8);
    require(detail::detectChromaticBeaconChains(fringedStreak).empty(),
            "color fragments along a continuous stellar ridge are not sparse aircraft beacons");
    auto crossingPlanes = clean;
    plane(crossingPlanes, false, true);
    plane(crossingPlanes, true, false);
    require(detail::detectChromaticBeaconChains(crossingPlanes).size() == 2,
            "two intersecting aircraft chains keep independent identities");
    auto baseline = clean;
    auto skyline = clean;
    for (unsigned y = 400; y < skyline.height; ++y)
        for (unsigned x = 0; x < 180; ++x)
            for (unsigned c = 0; c < 3; ++c)
                skyline.pixels[(y * skyline.width + x) * 4 + c] = ((x / 8 + y / 8) % 2 == 0 ? .03F : .12F);
    for (int x = 10; x <= 850; x += 30) {
        if (x >= 180 && x < 250)
            continue;
        // Colored building windows may align with the genuine flight in sky.
        spot(skyline, x, 450, {.05, .004, .002});
        spot(skyline, x + 12, 451, {.008, .035, .014});
    }
    const auto skylineTrails = detail::detectChromaticBeaconChains(skyline);
    require(skylineTrails.size() == 1, "a sky flight remains detectable beside colored building windows");
    auto skylineRepaired = skyline;
    detail::repairChromaticBeaconChains(skyline, skylineRepaired, skylineTrails);
    for (unsigned y = 400; y < skyline.height; ++y)
        for (unsigned x = 0; x < 180; ++x)
            for (unsigned c = 0; c < 4; ++c) {
                const auto p = (y * skyline.width + x) * 4 + c;
                require(skylineRepaired.pixels[p] == skyline.pixels[p],
                        "faint endpoint extension must not consume aligned foreground windows");
            }
    for (int y = 190; y < 290; ++y)
        spot(baseline, 330, y, {.035, .04, .044}, .9);
    // Exposure segments can end inside the aircraft corridor. Requiring two
    // intact stellar arms alone would erase the end of this short segment.
    for (int y = 275; y <= 305; ++y)
        spot(baseline, 600, y, {.035, .04, .044}, .9);
    auto contaminated = baseline;
    plane(contaminated, false, false);
    // A neutral strobe is close enough to an unrelated stellar ridge that
    // distant one-sided samples alone would incorrectly preserve it.
    spot(contaminated, 324, 240, {.04, .04, .04}, .8);
    const auto detected = detail::detectChromaticBeaconChains(contaminated);
    require(detected.size() == 1, "aircraft crossing a star is still detected");
    for (double scale : {.5, 1., 2.}) {
        auto variant = contaminated;
        variant.width = static_cast<unsigned>(contaminated.width * scale);
        variant.height = static_cast<unsigned>(contaminated.height * scale);
        variant.pixels.resize(variant.sampleCount());
        std::mt19937 noise(0x53544152);
        std::uniform_real_distribution<float> jitter(-.0008F, .0008F);
        for (unsigned y = 0; y < variant.height; ++y)
            for (unsigned x = 0; x < variant.width; ++x)
                for (unsigned c = 0; c < 4; ++c) {
                    const auto sx = static_cast<unsigned>(x / scale), sy = static_cast<unsigned>(y / scale);
                    variant.pixels[(y * variant.width + x) * 4 + c] =
                        contaminated.pixels[(sy * contaminated.width + sx) * 4 + c] + (c == 3 ? 0 : jitter(noise));
                }
        const auto variantInput = variant.pixels;
        const auto variantTrails = detail::detectChromaticBeaconChains(variant);
        require(variantTrails.size() == 1, "scaled and noisy aircraft remains one verified chain");
        require(variant.pixels == variantInput, "scaled noisy detection does not mutate input");
    }
    auto wrongCurve = detected.front();
    wrongCurve.path[wrongCurve.path.size() / 2].y += 70;
    require(!detail::sameBeaconChain(wrongCurve, detected.front()),
            "shared endpoints cannot authenticate a different repair corridor");
    require(detail::sameBeaconChain(detected.front(), detected.front()),
            "verified geometry authenticates its full path");
    auto repaired = contaminated;
    detail::repairChromaticBeaconChains(contaminated, repaired, detected);
    for (int c = 0; c < 3; ++c) {
        const auto p = (240 * baseline.width + 324) * 4 + c;
        require(std::abs(repaired.pixels[p] - baseline.pixels[p]) < .004,
                "a nearby disconnected star cannot authenticate a white aircraft strobe");
    }
    ArtifactTrailOptions roundTrip;
    roundTrip.useDetectedTrails = true;
    roundTrip.detectedTrails = detected;
    roundTrip.removeDrones = false;
    roundTrip.removeSatellites = false;
    std::vector<double> progressValues;
    int repairUpdates = 0;
    roundTrip.progress = [&](const ArtifactTrailProgress& event) {
        progressValues.push_back(event.progress);
        if (event.stage == ArtifactTrailProgressStage::Inpainting)
            ++repairUpdates;
    };
    const auto integrated = ArtifactTrailRemover().remove(contaminated, roundTrip);
    require(integrated.ok && integrated.removedTrails == 1, "public removal accepts the detected polyline");
    require(integrated.image.pixels == repaired.pixels, "geometry round trip uses the structure-preserving repair");
    require(repairUpdates >= 3 && std::is_sorted(progressValues.begin(), progressValues.end()) &&
                progressValues.back() == 1.0,
            "beacon-only repair reports monotonic intermediate progress through completion");
    roundTrip.removeAirplanes = false;
    const auto disabled = ArtifactTrailRemover().remove(contaminated, roundTrip);
    require(disabled.ok && disabled.removedTrails == 0 && disabled.image.pixels == contaminated.pixels,
            "disabling aircraft removal is an exact no-op");
    double before = 0, after = 0, positive = 0, negative = 0;
    for (int i = 1; i < 27; ++i) {
        if (i >= 12 && i <= 15)
            continue;
        const int x = 30 + i * 30, y = std::lround(160 + x * .24);
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 14; ++dx)
                for (int c = 0; c < 3; ++c) {
                    const auto p = ((y + dy) * baseline.width + x + dx) * 4 + c;
                    before += std::abs(contaminated.pixels[p] - baseline.pixels[p]);
                    after += std::abs(repaired.pixels[p] - baseline.pixels[p]);
                    positive += std::max(0.F, repaired.pixels[p] - baseline.pixels[p]);
                    negative += std::max(0.F, baseline.pixels[p] - repaired.pixels[p]);
                }
    }
    std::cout << "aircraft residual fraction=" << after / before << " positive=" << positive / before
              << " negative=" << negative / before << std::endl;
    double originalStar = 0, repairedStar = 0;
    for (int y = 232; y <= 246; ++y) {
        const auto p = (y * baseline.width + 330) * 4 + 1;
        originalStar += baseline.pixels[p] - clean.pixels[p];
        repairedStar += repaired.pixels[p] - clean.pixels[p];
    }
    std::cout << "crossing stellar flux retention=" << repairedStar / originalStar << std::endl;
    require(after < before * .15, "red and green aircraft residuals reduced by at least 85 percent");
    require(repairedStar > originalStar * .85, "crossing stellar core is not cut out");
    double shortBefore = 0, shortAfter = 0;
    for (int y = 300; y <= 305; ++y) {
        const auto p = (y * baseline.width + 600) * 4 + 1;
        shortBefore += baseline.pixels[p] - clean.pixels[p];
        shortAfter += repaired.pixels[p] - clean.pixels[p];
    }
    std::cout << "short stellar endpoint retention=" << shortAfter / shortBefore << std::endl;
    require(shortAfter > shortBefore * .85, "short stellar segment ending inside the mask is protected");
    for (unsigned y = 570; y < baseline.height; ++y)
        for (unsigned x = 0; x < baseline.width; ++x)
            for (int c = 0; c < 4; ++c) {
                const auto p = (y * baseline.width + x) * 4 + c;
                require(repaired.pixels[p] == contaminated.pixels[p],
                        "foreground is bit-exact outside the aircraft mask");
            }
    std::cout << "Chromatic beacon tests passed\n";
}
