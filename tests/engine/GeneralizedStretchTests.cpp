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
