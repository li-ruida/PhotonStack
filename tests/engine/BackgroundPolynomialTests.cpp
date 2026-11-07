#include "photonstack/BackgroundExtractor.hpp"
#include <cmath>
#include <stdexcept>
#include <source_location>
void require(bool b, std::source_location loc=std::source_location::current()) {
    if(!b) throw std::runtime_error("Polynomial background regression at " + std::to_string(loc.line()));
}
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
    auto explicitQuadratic=opts; explicitQuadratic.degree=2;
    require(extractor.extractPolynomial(im,explicitQuadratic).image.pixels==result.image.pixels);
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

    // Known sky with a broad, explicitly protected nebula: changing model order
    // must remove the gradient, not the subject. Work in signed linear units.
    for (unsigned degree : {1U,3U,4U}) {
        ImageBuffer source; source.width=source.height=256; source.channels=4;
        source.colorEncoding=ColorEncoding::Linear; source.pixels.resize(source.sampleCount());
        const auto signal=[](int x,int y) {
            const double r2=(x-128.)*(x-128.)+(y-128.)*(y-128.);
            return r2<60*60 ? 50*(1-r2/(60*60)) : 0;
        };
        for(int y=0;y<256;++y) for(int x=0;x<256;++x) {
            const double u=(x-128.)/128, v=(y-128.)/128;
            double sky=100+10*u+6*v;
            if(degree>=3) sky+=20*u*u*v+35*v*v*v;
            if(degree>=4) sky+=70*u*u*u*u+40*u*v*v*v+60*v*v*v*v;
            for(int c=0;c<3;++c) source.pixels[(y*256+x)*4+c]=sky*(1+c*.3)+signal(x,y);
            source.pixels[(y*256+x)*4+3]=1;
        }
        const auto original=source.pixels;
        BackgroundPolynomialOptions high;
        high.degree=degree; high.columns=high.rows=24;
        high.extraction.clampOutput=false; high.extraction.preserveBrightness=false;
        high.exclusions={{128,128,75,75,0}};
        const auto corrected=extractor.extractPolynomial(source,high);
        require(corrected.ok && source.pixels==original);
        double rms=0, oldRms=0; bool signedValue=false;
        auto quadratic=high; quadratic.degree=2;
        const auto old=extractor.extractPolynomial(source,quadratic); require(old.ok);
        for(int y=0;y<256;++y) for(int x=0;x<256;++x) for(int c=0;c<3;++c) {
            const auto i=(y*256+x)*4+c;
            rms+=std::pow(corrected.image.pixels[i]-signal(x,y),2);
            oldRms+=std::pow(old.image.pixels[i]-signal(x,y),2);
            signedValue |= corrected.image.pixels[i]<0;
        }
        rms=std::sqrt(rms/(256*256*3)); oldRms=std::sqrt(oldRms/(256*256*3));
        require(rms<.5);
        require(std::abs(corrected.image.pixels[(128*256+128)*4]-50)<.5);
        if(degree>2) { require(rms<oldRms*.1); require(signedValue); }
        // Unit/offset invariance in unclamped linear output, including negatives.
        for(std::size_t i=0;i<source.pixels.size();++i) if(i%4!=3) source.pixels[i]=source.pixels[i]*.01F-2;
        const auto scaled=extractor.extractPolynomial(source,high); require(scaled.ok);
        for(std::size_t i=0;i<source.pixels.size();++i) if(i%4!=3)
            require(std::abs(scaled.image.pixels[i]-corrected.image.pixels[i]*.01F)<.001);
        high.degree=0; require(!extractor.extractPolynomial(source,high).ok);
        high.degree=5; require(!extractor.extractPolynomial(source,high).ok);
        high.degree=4; high.columns=high.rows=4; high.exclusions.clear();
        require(!extractor.extractPolynomial(source,high).ok);
        // Enough samples by count but all on one horizontal strip: reject the
        // singular fit instead of extrapolating an unconstrained surface.
        high.columns=64; high.rows=16;
        for(int y=0;y<256;++y) for(int x=0;x<256;++x)
            source.pixels[(y*256+x)*4+3]=(y>=115 && y<=125) ? 1 : 0;
        const auto singular=extractor.extractPolynomial(source,high);
        require(!singular.ok && singular.errorCode=="BackgroundSamplesInsufficient");
    }
}
