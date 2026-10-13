#include "photonstack/FitsCodec.hpp"
#include "photonstack/Stacker.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <source_location>
#include <stdexcept>

using namespace photonstack;
namespace fs = std::filesystem;
void require(bool ok, const std::source_location where = std::source_location::current()) {
    if (!ok) throw std::runtime_error("Extrema check at line " + std::to_string(where.line()));
}
int main() {
    const auto root = fs::temp_directory_path() / ("photonstack-extrema-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{root};
    ImageBuffer a;
    a.width = 6; a.height = 1; a.channels = 4;
    a.pixels = {-8, 2, 900, 1,  -5, -2, -1, 1,  99, 99, 99, 0,
                1, 2, 3, 0,  7, 8, 9, .5,  1, 2, 3, 1};
    auto b = a;
    b.pixels = {-3, 20, 400, 1,  -6, -8, -9, 1,  4, 5, 6, 1,
                8, 9, 10, 0,  10, 6, 8, 1,  std::numeric_limits<float>::quiet_NaN(), 90, 90, 1};
    std::vector<fs::path> inputs{root / "a.fits", root / "b.fits"};
    require(FitsCodec().write(a, inputs[0]).ok);
    require(FitsCodec().write(b, inputs[1]).ok);
    for (const auto method : {StackMethod::Maximum, StackMethod::Minimum}) {
        StackOptions o; o.method = method;
        // Streaming extrema do not need a full sequence scratch cache.
        o.temporaryDirectory = root / "does-not-exist";
        std::vector<std::size_t> consumed;
        o.inputConsumed = [&](std::size_t i) { consumed.push_back(i); };
        const auto r = Stacker().stack(inputs, o);
        require(r.ok && consumed == std::vector<std::size_t>{0, 1});
        const std::vector<float> expected = method == StackMethod::Maximum
            ? std::vector<float>{-3,20,900,1, -5,-2,-1,1, 4,5,6,.5, 0,0,0,0, 10,8,9,.75, 1,2,3,.5}
            : std::vector<float>{-8,2,400,1, -6,-8,-9,1, 4,5,6,.5, 0,0,0,0, 7,6,8,.75, 1,2,3,.5};
        require(r.image.pixels == expected);
        require(r.rejectionLow.empty() && r.rejectionHigh.empty());
        std::reverse(inputs.begin(), inputs.end());
        require(Stacker().stack(inputs, o).image.pixels == expected);
        std::reverse(inputs.begin(), inputs.end());
        const auto single = Stacker().stack({inputs[0]}, o);
        require(single.ok && single.image.pixels[0] == -8 && single.image.pixels[19] == .5);
        require(!Stacker().stack({}, o).ok);
        o.collectRejectionMaps = true; require(!Stacker().stack(inputs, o).ok);
        o.collectRejectionMaps = false;
        o.sigma.medianMad = true; require(!Stacker().stack(inputs, o).ok);
        o.sigma.medianMad = false;
        o.frameWeights = {1,1}; require(!Stacker().stack(inputs, o).ok);
    }
}
