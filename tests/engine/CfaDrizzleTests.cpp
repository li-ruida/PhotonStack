#include "photonstack/CfaDrizzle.hpp"
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

using namespace photonstack;
namespace {
void check(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
void close(double a, double b, double eps, const char* msg) { check(std::abs(a-b)<=eps,msg); }
template<class F> void rejects(F f) {
    bool threw=false; try { f(); } catch(const std::invalid_argument&) { threw=true; }
    check(threw,"invalid input must throw");
}
}
int main() {
    // Independent known color tiles; signed scientific values must survive normalization.
    const int colors[4][4]={{0,1,1,2},{1,0,2,1},{1,2,0,1},{2,1,1,0}};
    const float values[3]={-123.5F,4096.F,70000.F};
    for (int pat=0;pat<4;++pat) for (int xo=-1;xo<=1;++xo) for (int yo=-1;yo<=1;++yo) {
        CfaDrizzleAccumulator acc({8,8,4,4,1,1});
        std::vector<float> raw(16*16);
        for (int y=0;y<16;++y) for (int x=0;x<16;++x)
            raw[y*16+x]=values[colors[pat][((y+yo+2)%2)*2+(x+xo+2)%2]];
        CfaDrizzleFrame f; f.width=f.height=16; f.samples=raw;
        f.pattern=static_cast<BayerPattern>(pat); f.xOffset=xo; f.yOffset=yo;
        // All four integer dither phases cover every color without interpolation.
        for (int y=0;y<2;++y) for(int x=0;x<2;++x) { f.transform[4]=x; f.transform[5]=y; acc.add(f); }
        const auto image=acc.image();
        for(std::size_t i=0;i<image.pixelCount();++i) {
            check(image.pixels[i*4+3]==1,"complete CFA coverage");
            for(int c=0;c<3;++c) {
                close(image.pixels[i*4+c],values[c],0,"CFA phase or constant-field failure");
                close(acc.weights()[i*3+c],c==1?2:1,1e-12,"independent RGB weights");
            }
        }
    }
    // A single sensor sample's weighted counts and overlap fractions are conserved,
    // including rotations, shears, reflections and output scale. No canvas clipping.
    for (double scale : {1.,2.}) for(double fraction : {.4,1.}) for(int reflected : {0,1}) {
        CfaDrizzleAccumulator acc({40,40,0,0,scale,fraction});
        const std::vector<float> raw={24};
        CfaDrizzleFrame f; f.width=f.height=1; f.samples=raw;
        f.transform={reflected?-.8:.8,.3,-.2,1.1,8.3,9.7}; f.weight=3;
        f.background={4,0,0}; f.gain={2,1,1}; acc.add(f);
        close(std::accumulate(acc.weights().begin(),acc.weights().end(),0.),3,1e-10,"drop area conservation");
        close(std::accumulate(acc.weightedSums().begin(),acc.weightedSums().end(),0.),120,1e-8,"weighted counts conservation");
        auto im=acc.image();
        for(std::size_t p=0;p<im.pixelCount();++p) {
            check(im.pixels[p*4+3]==0,"missing colors must remain invalid");
            check(std::isnan(im.pixels[p*4+1]) && std::isnan(im.pixels[p*4+2]),"no invented color");
        }
    }
    // Known half-pixel geometry: one sample lands in exactly four equal areas.
    {
        CfaDrizzleAccumulator acc({3,3}); std::vector<float> raw={7};
        CfaDrizzleFrame f; f.width=f.height=1; f.samples=raw; f.transform[4]=f.transform[5]=.5;
        acc.add(f);
        for(int y=0;y<3;++y) for(int x=0;x<3;++x)
            close(acc.weights()[(y*3+x)*3],x<2&&y<2?.25:0,1e-14,"half-pixel convention");
    }
    // Native ROI origin must match cropping a larger reconstruction exactly.
    {
        CfaDrizzleAccumulator full({30,30}), crop({12,12,7,9});
        std::vector<float> raw(30*30);
        for(std::size_t i=0;i<raw.size();++i) raw[i]=float(i%37-18.);
        CfaDrizzleFrame f; f.width=f.height=30; f.samples=raw; f.pattern=BayerPattern::GRBG;
        f.transform={.98,.04,-.04,.98,.7,1.3};
        full.add(f);crop.add(f);
        for(int y=0;y<12;++y) for(int x=0;x<12;++x) for(int c=0;c<3;++c) {
            close(crop.weights()[(y*12+x)*3+c],full.weights()[((y+9)*30+x+7)*3+c],1e-12,"ROI weight equivalence");
            close(crop.weightedSums()[(y*12+x)*3+c],full.weightedSums()[((y+9)*30+x+7)*3+c],1e-10,"ROI sum equivalence");
        }
        std::vector<double> before(full.weights().begin(),full.weights().end());
        f.transform[0]=std::numeric_limits<double>::quiet_NaN(); rejects([&]{full.add(f);});
        check(std::equal(before.begin(),before.end(),full.weights().begin()),"rejected frame leaves accumulator unchanged");
    }
    {
        CfaDrizzleAccumulator acc({2,2});
        std::vector<float> raw={5,6,std::numeric_limits<float>::quiet_NaN(),8}, validity={1,0,1,-1};
        CfaDrizzleFrame f; f.width=f.height=2; f.samples=raw; f.validity=validity;
        acc.add(f); close(std::accumulate(acc.weights().begin(),acc.weights().end(),0.),1,0,"invalid samples skipped");
        f.transform={1,1,1,1,0,0}; rejects([&]{acc.add(f);});
        f.transform={1,0,0,1,0,0}; f.samples={}; rejects([&]{acc.add(f);});
        rejects([]{ CfaDrizzleAccumulator invalid({4,4,0,0,1,0}); });
    }
    {
        CfaDrizzleAccumulator acc({2,2}); std::vector<float> raw={1,2,3,4};
        CfaDrizzleFrame f; f.width=f.height=2;f.samples=raw;f.gain={1,1e100,1};
        rejects([&]{acc.add(f);});
        close(std::accumulate(acc.weights().begin(),acc.weights().end(),0.),0,0,"calibration overflow is atomic");
        f.gain={1,1,1};f.weight=1e308;rejects([&]{acc.add(f);});
    }
}
