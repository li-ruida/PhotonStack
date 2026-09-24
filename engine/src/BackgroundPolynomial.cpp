#include "photonstack/BackgroundExtractor.hpp"
#include "MaskedImageSampling.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace photonstack {
namespace {
using Basis = std::array<double, 6>;
struct Sample { Basis basis; std::array<double, 3> color; };
double median(std::vector<double> v) {
    auto m = v.begin() + v.size()/2;
    std::nth_element(v.begin(), m, v.end());
    return *m;
}
Basis basisAt(double x, double y, const ImageBuffer& im) {
    x = 2*x/im.width - 1; y = 2*y/im.height - 1;
    return {1,x,y,x*x,x*y,y*y};
}
double evaluate(const Basis& b, const Basis& c) { return std::inner_product(b.begin(),b.end(),c.begin(),0.0); }
bool fit(const std::vector<Sample>& samples, int channel, Basis& coefficients) {
    std::vector<bool> keep(samples.size(),true);
    for (int iteration=0;iteration<4;++iteration) {
        double system[6][7] = {};
        std::size_t count=0;
        for (std::size_t n=0;n<samples.size();++n) if (keep[n]) {
            ++count; const auto& s=samples[n];
            for (int i=0;i<6;++i) {
                for (int j=0;j<6;++j) system[i][j]+=s.basis[i]*s.basis[j];
                system[i][6]+=s.basis[i]*s.color[channel];
            }
        }
        if (count<12) return false;
        for (int i=0;i<6;++i) {
            int pivot=i;
            for (int j=i+1;j<6;++j) if (std::abs(system[j][i])>std::abs(system[pivot][i])) pivot=j;
            if (std::abs(system[pivot][i])<1e-10) return false;
            for(int j=0;j<7;++j) std::swap(system[i][j],system[pivot][j]);
            const double divisor=system[i][i];
            for(int j=i;j<7;++j) system[i][j]/=divisor;
            for(int k=0;k<6;++k) if(k!=i) {
                const double factor=system[k][i];
                for(int j=i;j<7;++j) system[k][j]-=factor*system[i][j];
            }
        }
        for(int i=0;i<6;++i) coefficients[i]=system[i][6];
        if(iteration==3) break;
        std::vector<double> residuals;
        for(const auto& s:samples) residuals.push_back(s.color[channel]-evaluate(s.basis,coefficients));
        const double center=median(residuals);
        std::vector<double> deviations;
        for(double v:residuals) deviations.push_back(std::abs(v-center));
        const double threshold=std::max(1e-10,4.4478*median(deviations));
        for(std::size_t n=0;n<samples.size();++n) keep[n]=std::abs(residuals[n]-center)<=threshold;
    }
    return true;
}
}
BackgroundExtractionResult BackgroundExtractor::extractPolynomial(const ImageBuffer& im,
    const BackgroundPolynomialOptions& opt) const {
    const auto fail=[](const char* code,const char* message) { BackgroundExtractionResult r; r.errorCode=code;r.message=message;return r; };
    if(im.empty() || im.channels==0 || im.channels>4 || im.pixels.size()!=im.sampleCount() || im.width<16 || im.height<16)
        return fail("ImageBufferInvalid","Polynomial background requires an image at least 16 by 16 pixels");
    if(detail::imageHasInvalidCoveredColor(im)) return fail("ImageBufferInvalid","Covered image samples must be finite");
    if(opt.extraction.mode!=BackgroundMode::Subtract || !std::isfinite(opt.extraction.strength) ||
       opt.extraction.strength<0 || opt.extraction.strength>1 || opt.columns<4 || opt.rows<4 || opt.columns>512 || opt.rows>512)
        return fail("ArgumentInvalid","Polynomial background requires subtraction, strength 0...1, and grid dimensions 4...512");
    for(const auto& e:opt.exclusions)
        if(!std::isfinite(e.x)||!std::isfinite(e.y)||!std::isfinite(e.major)||!std::isfinite(e.minor)||!std::isfinite(e.angleDegrees)||e.major<=0||e.minor<=0)
            return fail("ArgumentInvalid","Background exclusion ellipses must be finite with positive semiaxes");
    const auto excluded=[&](double x,double y) {
        for(const auto& e:opt.exclusions) {
            const double angle=e.angleDegrees*3.141592653589793/180;
            const double dx=x-e.x,dy=y-e.y;
            const double u=(dx*std::cos(angle)+dy*std::sin(angle))/e.major;
            const double v=(-dx*std::sin(angle)+dy*std::cos(angle))/e.minor;
            if(u*u+v*v<=1) return true;
        }
        return false;
    };
    const int channels=std::min<int>(3,im.channels);
    std::vector<Sample> samples;
    for(std::uint32_t gy=0;gy<opt.rows;++gy) for(std::uint32_t gx=0;gx<opt.columns;++gx) {
        const int x=static_cast<int>((gx+.5)*im.width/opt.columns), y=static_cast<int>((gy+.5)*im.height/opt.rows);
        if(x<4||y<4||x+4>=static_cast<int>(im.width)||y+4>=static_cast<int>(im.height)) continue;
        std::array<std::vector<double>,3> values;
        bool subject=false;
        for(int dy=-4;dy<=4;++dy) for(int dx=-4;dx<=4;++dx) {
            if(excluded(x+dx,y+dy)) { subject=true;continue; }
            const auto p=static_cast<std::size_t>(y+dy)*im.width+x+dx;
            if(!detail::pixelHasValidColor(im,p) || (im.channels==4 && im.pixels[p*4+3]<.99F)) continue;
            for(int c=0;c<channels;++c) values[c].push_back(im.pixels[p*im.channels+c]);
        }
        if(subject||values[0].size()<60) continue;
        Sample sample; sample.basis=basisAt(x,y,im);
        for(int c=0;c<channels;++c) sample.color[c]=median(values[c]);
        samples.push_back(sample);
    }
    std::array<Basis,3> coefficients;
    if(samples.size()<12) return fail("BackgroundSamplesInsufficient","At least twelve sky samples outside the exclusions are required");
    for(int c=0;c<channels;++c) if(!fit(samples,c,coefficients[c]))
        return fail("BackgroundSamplesInsufficient","Sky samples do not constrain a quadratic background surface");
    BackgroundExtractionResult result; result.image=im;
    result.sampledGridCells=static_cast<std::uint32_t>(samples.size());result.gridColumns=opt.columns;result.gridRows=opt.rows;
    std::array<double,3> pedestal{};
    for(int c=0;c<channels;++c) {
        std::vector<double> v;
        for(const auto& s:samples) v.push_back(evaluate(s.basis,coefficients[c]));
        pedestal[c]=median(v);
    }
    result.background=static_cast<float>(pedestal[0]);
    for(std::uint32_t y=0;y<im.height;++y) for(std::uint32_t x=0;x<im.width;++x) {
        const auto p=static_cast<std::size_t>(y)*im.width+x;
        if(!detail::pixelHasValidColor(im,p)) { detail::clearMaskedPixel(result.image,p);continue; }
        const auto b=basisAt(x,y,im);
        for(int c=0;c<channels;++c) {
            const auto o=p*im.channels+c;
            const double value=im.pixels[o]-opt.extraction.strength*(evaluate(b,coefficients[c])-(opt.extraction.preserveBrightness?pedestal[c]:0));
            if(!std::isfinite(value)||std::abs(value)>std::numeric_limits<float>::max()) return fail("ImageValueInvalid","Background subtraction exceeds Float32 range");
            result.image.pixels[o]=static_cast<float>(opt.extraction.clampOutput?std::clamp(value,0.0,1.0):value);
        }
    }
    result.ok=true; return result;
}
}
