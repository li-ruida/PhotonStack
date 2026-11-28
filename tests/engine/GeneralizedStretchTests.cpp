#include "photonstack/GeneralizedStretch.hpp"
#include "photonstack/FitsCodec.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <stdexcept>
using namespace photonstack;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try {
    GeneralizedStretchOptions o;
    for(double b:{-5.,-2.,-1.-1e-10,-1.,-1.+1e-10,-.1,-1e-10,0.,1e-10,.1,1.,3.,15.})
    for(double d:{0.,1e-10,2.,10.})for(double sp:{0.,.2,.5,1.}) {
        o.localIntensity=b;o.logStretch=d;o.symmetry=sp;o.shadowProtection=sp/2;o.highlightProtection=(1+sp)/2;
        GeneralizedStretchCurve c(o);require(c.evaluate(0)==0&&c.evaluate(1)==1,"endpoints");
        double last=0;for(int i=0;i<=10000;++i){const double x=i/10000.,y=c.evaluate(x);require(std::isfinite(y)&&y>=last-2e-14&&y<=1,"monotonic finite curve");last=y;if(d==0)require(x==y,"identity");}
        for(double x:{o.shadowProtection,sp,o.highlightProtection})if(x>0&&x<1){
            const double h=1e-9,center=c.evaluate(x),left=(center-c.evaluate(x-h))/h,right=(c.evaluate(x+h)-center)/h;
            require(std::abs(left-right)<.001*std::max(1.,std::max(left,right)),"curve tangent continuity");}
    }
    o={};o.logStretch=2;o.localIntensity=1;GeneralizedStretchCurve harmonic(o);
    const double d=std::expm1(2.);for(int i=0;i<=100;++i){double x=i/100.;require(std::abs(harmonic.evaluate(x)-x*(1+d)/(1+d*x))<1e-14,"known harmonic curve");}
    o.localIntensity=-1;GeneralizedStretchCurve logarithmic(o);
    for(int i=0;i<=100;++i){double x=i/100.;require(std::abs(logarithmic.evaluate(x)-std::log1p(d*x)/std::log1p(d))<1e-14,"known logarithmic curve");}
    for(double b:{-5.,-1.-1e-10,-1.,-1.+1e-10,-.1,0.,.1,1.,15.})for(double sp:{0.,.2,.5,1.}) {
        o={};o.logStretch=2;o.localIntensity=b;o.symmetry=sp;o.shadowProtection=sp/2;o.highlightProtection=(1+sp)/2;
        GeneralizedStretchCurve forward(o);o.inverse=true;GeneralizedStretchCurve inverse(o);
        double last=0;
        for(int i=0;i<=1000;++i){const double x=i/1000.,y=inverse.evaluate(x);
            require(std::abs(forward.evaluate(y)-x)<1e-10,"inverse forward residual");
            require(std::abs(inverse.evaluate(forward.evaluate(x))-x)<1e-10,"forward inverse residual");
            require(y>=last&&y>=0&&y<=1,"inverse monotonic bounds");last=y;}
    }
    o={};o.logStretch=2;o.inverse=true;o.localIntensity=1;GeneralizedStretchCurve ih(o);
    for(int i=0;i<=100;++i){double y=i/100.;require(std::abs(ih.evaluate(y)-y/(1+d-d*y))<1e-13,"independent harmonic inverse");}
    ImageBuffer colors;colors.width=3;colors.height=1;colors.channels=4;colors.pixels={.8F,.4F,.2F,1, .3F,.3F,.3F,1, 0,0,0,1};
    o={};o.saturation=true;o.luminance=false;auto sat=generalizedStretch(colors,o);
    require(sat.image.pixels[0]==colors.pixels[0]&&sat.image.pixels[1]<colors.pixels[1]&&sat.image.pixels[2]<colors.pixels[2],"HSV saturation preserves value");
    require(std::abs((sat.image.pixels[1]-sat.image.pixels[2])/(sat.image.pixels[0]-sat.image.pixels[2])-1./3)<1e-6,"HSV hue preserved");
    for(int i=4;i<12;++i)require(sat.image.pixels[i]==colors.pixels[i],"neutral pixels changed");
    o.inverse=true;auto unsat=generalizedStretch(sat.image,o);
    for(std::size_t i=0;i<colors.pixels.size();++i)require(std::abs(unsat.image.pixels[i]-colors.pixels[i])<2e-6,"saturation inverse");
    ImageBuffer im;im.width=5;im.height=1;im.channels=4;im.pixels={.02F,.01F,.005F,1, 1,0,0,.25F, 0,0,0,1, 1,1,1,1, NAN,NAN,NAN,0};
    o={};auto result=generalizedStretch(im,o);require(result.coveredPixels==4,"coverage count");require(result.gamutBlendedPixels==1,"gamut count");
    for(auto v:result.image.pixels)require(std::isfinite(v)&&v>=0&&v<=1,"bounded output");
    require(result.image.pixels[7]==.25F&&result.image.pixels[19]==0,"alpha preserved");
    require(result.image.pixels[0]/result.image.pixels[1]==2,"unclipped chromaticity changed");
    require(std::accumulate(result.afterHistogram.begin(),result.afterHistogram.end(),std::size_t(0))==4,"histogram count");
    o.black=.1;o.white=.8;result=generalizedStretch(im,o);require(result.belowBlack==3&&result.aboveWhite==2,"clipping count");
    const auto temp=std::filesystem::temp_directory_path()/"photonstack-ghs-domain-test.fits";
    require(FitsCodec().write(result.image,temp,nullptr,nullptr,FitsOutputDomain::DisplaySRGB).ok,"display write");
    auto read=FitsCodec().read(temp);require(read.ok&&read.image.colorEncoding==ColorEncoding::SRGB&&read.image.pixels==result.image.pixels,"display round trip values or alpha");
    bool failed=false;try{readLinearScienceImage(temp);}catch(...){failed=true;}require(failed,"display accepted as science");std::filesystem::remove(temp);
    std::cout<<"GHS shape, continuity, RGB gamut, masks, domain and clipping passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
