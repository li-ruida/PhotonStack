#include <cmath>
#include <array>
#include <limits>
#include "../../engine/src/StraightAlphaSampling.hpp"
#include <stdexcept>
#include "photonstack/Registration.hpp"

void require(bool b) { if (!b) throw std::runtime_error("Registration sampling regression"); }
int main() {
    using namespace photonstack;
    // Combined-channel interpolation must preserve the scalar contract at
    // borders, incomplete coverage, non-finite colors, and extra channels.
    for (const unsigned channels : {1U, 3U, 4U, 5U}) {
        ImageBuffer samples;
        samples.width = 9; samples.height = 7; samples.channels = channels;
        samples.pixels.resize(samples.sampleCount());
        for (std::size_t p = 0; p < samples.pixelCount(); ++p) {
            for (unsigned c = 0; c < channels; ++c)
                samples.pixels[p * channels + c] = float(int((p * 17 + c * 31) % 67) - 23) / 11.F;
            if (channels >= 4) samples.pixels[p * channels + 3] = float(p % 5) / 4.F;
        }
        samples.pixels[3 * channels] = std::numeric_limits<float>::quiet_NaN();
        if (channels >= 4) samples.pixels[4 * channels + 3] = std::numeric_limits<float>::infinity();
        for (bool clamp : {false, true}) for (int iy = -3; iy < 27; ++iy) for (int ix = -3; ix < 35; ++ix) {
            std::array<float, 5> out{};
            const float x = ix * .27F, y = iy * .27F;
            detail::sampleBilinearPixel(samples, x, y, clamp, out.data());
            for (unsigned c = 0; c < channels; ++c)
                require(out[c] == detail::sampleBilinearStraightAlpha(samples, x, y, c, clamp));
        }
    }
    ImageBuffer source;
    source.width = source.height = 33; source.channels = 4;
    source.colorEncoding = ColorEncoding::Linear;
    source.pixels.resize(source.sampleCount());
    const auto signal = [](double x, double y) { return -20 + 800*std::exp(-((x-16.3)*(x-16.3)+(y-15.7)*(y-15.7))/4.5); };
    for (int y=0;y<33;++y) for(int x=0;x<33;++x) {
        for(int c=0;c<3;++c) source.pixels[(y*33+x)*4+c]=signal(x,y);
        source.pixels[(y*33+x)*4+3]=1;
    }
    const Registration old, sharp(RegistrationInterpolation::Bicubic);
    const SimilarityTransform transform{1,0,.37F,-.42F};
    auto a=old.applySimilarity(source,transform), b=sharp.applySimilarity(source,transform);
    double ea=0,eb=0;
    for(int y=3;y<30;++y) for(int x=3;x<30;++x) {
        const auto i=(y*33+x)*4;
        const double expected=signal(x-transform.dx,y-transform.dy);
        ea+=std::pow(a.pixels[i]-expected,2); eb+=std::pow(b.pixels[i]-expected,2);
        require(b.pixels[i+3]==1);
    }
    require(eb<ea*.5);
    require(sharp.renderSimilarityRows(source,transform,[&](std::uint32_t y,const float* row,std::size_t n){
        for(std::size_t i=0;i<n;++i) require(row[i]==b.pixels[y*n+i]);
        return true;
    }));
    require(sharp.applyTranslation(source,{.37F,-.42F}).pixels==b.pixels);
    for(std::size_t i=0;i<source.pixelCount();++i) for(int c=0;c<3;++c) source.pixels[i*4+c]=-42;
    b=sharp.applySimilarity(source,transform);
    require(std::fabs(b.pixels[(16*33+16)*4]+42)<1e-5);
    // Invisible hostile RGB must neither bleed nor produce negative coverage.
    source.pixels[(16*33+16)*4]=1e8F; source.pixels[(16*33+16)*4+3]=0;
    a=old.applySimilarity(source,transform); b=sharp.applySimilarity(source,transform);
    for(int y=15;y<=17;++y) for(int x=15;x<=17;++x) {
        const auto i=(y*33+x)*4;
        for(int c=0;c<4;++c) require(std::fabs(a.pixels[i+c]-b.pixels[i+c])<1e-5);
        require(b.pixels[i+3]>=0 && b.pixels[i+3]<=1);
    }
}
