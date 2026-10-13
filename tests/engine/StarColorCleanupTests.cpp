#include "photonstack/StarColorCleanup.hpp"
#include "photonstack/AstroDevelop.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace photonstack;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
ImageBuffer sky() {
    ImageBuffer im;
    im.width = 384; im.height = 256; im.channels = 4;
    im.pixels.resize(im.sampleCount());
    for (std::size_t p = 0; p < im.pixelCount(); ++p) {
        im.pixels[p*4] = 12; im.pixels[p*4+1] = -4; im.pixels[p*4+2] = 7; im.pixels[p*4+3] = 1;
    }
    return im;
}
void addStar(ImageBuffer& im, double x, double y, std::array<double,3> color, bool fringe, double width = 1.4) {
    for (unsigned iy = 0; iy < im.height; ++iy) for (unsigned ix = 0; ix < im.width; ++ix)
        for (int c = 0; c < 3; ++c) {
            const double sigma = width * (fringe && c == 2 ? 1.5 : 1.);
            const double dx = ix - x - (fringe ? (c-1)*.32 : 0.), dy = iy - y;
            im.pixels[(std::size_t(iy)*im.width+ix)*4+c] += color[c] * 800 * width*width/(sigma*sigma) *
                std::exp(-(dx*dx+dy*dy)/(2*sigma*sigma));
        }
}
double variation(const ImageBuffer& im, double x, double y, const std::array<float,3>& gains) {
    double sum = 0, mass = 0; std::array<double,3> first{}, second{};
    const double sky[] = {12,-4,7}, w[] = {.2126,.7152,.0722};
    for (int iy = y-8; iy <= y+8; ++iy) for (int ix = x-8; ix <= x+8; ++ix) {
        const auto p = (iy*im.width+ix)*4; std::array<double,3> q{}; double l = 0;
        for (int c = 0; c < 3; ++c) { q[c] = (im.pixels[p+c]-sky[c])*gains[c]; l += w[c]*q[c]; }
        if (l < 2) continue;
        mass += l;
        for (int c = 0; c < 3; ++c) { first[c] += q[c]; second[c] += q[c]*q[c]/l; }
    }
    for (int c = 0; c < 3; ++c) sum += second[c]/mass - std::pow(first[c]/mass,2);
    return sum;
}
int main() {
    const std::array<float,3> gains = {.8F,1.F,1.1F};
    auto im = sky();
    for (int y = 40; y < 230; y += 48) for (int x = 40; x < 350; x += 48)
        addStar(im, x+.2, y+.3, x%3 == 0 ? std::array<double,3>{1.8,1,.65} : std::array<double,3>{.75,1,1.7}, true);
    const auto original = im.pixels;
    const auto cleaned = StarColorCleanup().apply(im,gains,.8F);
    require(cleaned.ok && cleaned.correctedStars >= 20, "isolated stars are corrected");
    require(im.pixels == original, "input remains untouched");
    std::array<double,3> total{};
    double maxL = 0;
    for (std::size_t p = 0; p < im.pixelCount(); ++p) {
        double delta = 0; const double weights[] = {.2126,.7152,.0722};
        for (int c = 0; c < 3; ++c) {
            const double d = cleaned.image.pixels[p*4+c] - im.pixels[p*4+c];
            delta += weights[c]*gains[c]*d; total[c] += d;
        }
        maxL = std::max(maxL,std::abs(delta));
        require(cleaned.image.pixels[p*4+3] == im.pixels[p*4+3], "alpha preserved");
    }
    require(maxL < .0002, "no luminance smoothing or sharpening");
    for (double v : total) require(std::abs(v) < .03, "integrated RGB flux preserved");
    for (int y = 40; y < 230; y += 48) for (int x = 40; x < 350; x += 48) {
        require(variation(cleaned.image,x+.2,y+.3,gains) < .2*variation(im,x+.2,y+.3,gains),
                "inconsistent color within both blue and red stars is reduced");
        for (int c = 0; c < 3; ++c) {
            double delta = 0;
            for (int dy = -22; dy <= 22; ++dy) for (int dx = -22; dx <= 22; ++dx)
                delta += cleaned.image.pixels[((y+dy)*im.width+x+dx)*4+c] - im.pixels[((y+dy)*im.width+x+dx)*4+c];
            require(std::abs(delta) < .005, "each star keeps its own aperture color");
        }
    }
    const auto bypass = StarColorCleanup().apply(im,gains,0);
    require(bypass.ok && bypass.image.pixels == im.pixels && bypass.correctedStars == 0, "zero exactly bypasses");
    auto scale = im;
    for (std::size_t p = 0; p < im.pixelCount(); ++p) for (int c=0;c<3;++c) scale.pixels[p*4+c] *= .001F;
    const auto scaled = StarColorCleanup().apply(scale,gains,.8F);
    require(scaled.ok && scaled.correctedStars == cleaned.correctedStars, "FITS unit independent selection");
    for (std::size_t p = 0; p < im.pixelCount(); ++p) for (int c=0;c<3;++c)
        require(std::abs(scaled.image.pixels[p*4+c]*1000-cleaned.image.pixels[p*4+c]) < .001,
                "FITS unit independent correction");
    auto perfect = sky(); addStar(perfect,100.2,100.3,{.6,1,2.},false);
    const auto kept = StarColorCleanup().apply(perfect,gains,1);
    require(kept.ok && kept.correctedStars == 1, "consistent blue star detected");
    for (std::size_t i=0;i<perfect.sampleCount();++i)
        require(std::abs(kept.image.pixels[i]-perfect.pixels[i]) < .0003, "consistent real star color unchanged");
    auto diffuse = sky(); addStar(diffuse,180,120,{2,1,.3},true,12);
    const auto protectedDiffuse = StarColorCleanup().apply(diffuse,gains,1);
    require(protectedDiffuse.ok && protectedDiffuse.image.pixels == diffuse.pixels, "broad nebula is not a star");
    auto binary = sky(); addStar(binary,120,120,{2,1,.5},false); addStar(binary,126,120,{.5,1,2},false);
    const auto protectedBinary = StarColorCleanup().apply(binary,gains,1);
    require(protectedBinary.ok && protectedBinary.image.pixels == binary.pixels, "close differently colored double star protected");
    auto masked = perfect; const auto hole = (100*im.width+100)*4;
    masked.pixels[hole] = std::numeric_limits<float>::quiet_NaN(); masked.pixels[hole+3] = 0;
    const auto coverage = StarColorCleanup().apply(masked,gains,1);
    require(coverage.ok && coverage.correctedStars == 0 && std::isnan(coverage.image.pixels[hole]) && coverage.image.pixels[hole+3]==0,
            "missing coverage protects its surrounding star");
    for (float bad : {-1.F,1.1F,std::numeric_limits<float>::quiet_NaN()})
        require(!StarColorCleanup().apply(im,gains,bad).ok, "invalid strength rejected");
    AstroDevelopOptions options;
    options.stellarBalance=false; options.gains=gains; options.toneCurve=AstroToneCurve::Asinh;
    options.toneScale=20; options.whitePoint=3000; options.starExposure=.5;
    const auto before = AstroDevelop().apply(im,options);
    options.starColorCleanup=.8;
    const auto after = AstroDevelop().apply(im,options);
    require(before.ok && after.ok && after.colorCleanedStars>0 && after.adjustedStars>0,"cleanup composes with star exposure");
    for (std::size_t p=0;p<im.pixelCount();++p) {
        double delta=0; const double w[]={.2126,.7152,.0722};
        for(int c=0;c<3;++c)delta+=w[c]*(after.image.pixels[p*4+c]-before.image.pixels[p*4+c]);
        require(std::abs(delta)<.000002,"developed luminance preserved after exposure adjustment");
    }
    std::cout << "Stars " << cleaned.correctedStars << ", max linear luminance difference " << maxL << '\n';
}
