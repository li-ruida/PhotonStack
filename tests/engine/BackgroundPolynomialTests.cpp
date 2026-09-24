#include "photonstack/BackgroundExtractor.hpp"
#include <cmath>
#include <stdexcept>
void require(bool b) { if(!b) throw std::runtime_error("Polynomial background regression"); }
int main() {
    using namespace photonstack;
    ImageBuffer im;im.width=im.height=128;im.channels=4;im.colorEncoding=ColorEncoding::Linear;im.pixels.resize(im.sampleCount());
    for(int y=0;y<128;++y) for(int x=0;x<128;++x) {
        double u=(x-64.)/64,v=(y-64.)/64;
        bool subject=(x-64)*(x-64)+(y-64)*(y-64)<25*25;
        for(int c=0;c<3;++c) im.pixels[(y*128+x)*4+c]=100+c*30+10*u+6*v+2*u*u+u*v+3*v*v+(subject?50:0);
        im.pixels[(y*128+x)*4+3]=1;
    }
    im.pixels[0]=1e8F;im.pixels[3]=0;
    BackgroundPolynomialOptions opts;opts.columns=opts.rows=16;opts.extraction.clampOutput=false;opts.extraction.preserveBrightness=false;
    opts.exclusions.push_back({64,64,40,40,0});
    BackgroundExtractor extractor;auto result=extractor.extractPolynomial(im,opts);
    require(result.ok && result.sampledGridCells>=12);
    require(result.image.pixels[0]==0 && result.image.pixels[3]==0);
    for(int c=0;c<3;++c) {
        require(std::abs(result.image.pixels[(64*128+64)*4+c]-50)<.1);
        require(std::abs(result.image.pixels[(20*128+20)*4+c])<.1);
    }
    opts.extraction.strength=0;result=extractor.extractPolynomial(im,opts);
    require(result.ok && result.image.pixels[(64*128+64)*4]==im.pixels[(64*128+64)*4]);
    opts.exclusions={{64,64,1000,1000,0}};
    require(!extractor.extractPolynomial(im,opts).ok);
    opts.exclusions={{64,64,0,40,0}};
    require(!extractor.extractPolynomial(im,opts).ok);
    opts.exclusions.clear();opts.extraction.mode=BackgroundMode::Divide;
    require(!extractor.extractPolynomial(im,opts).ok);
}
