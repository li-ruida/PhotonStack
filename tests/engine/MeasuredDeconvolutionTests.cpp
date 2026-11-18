#include "photonstack/MeasuredDeconvolution.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace photonstack;
void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
ImageBuffer gray(unsigned w,unsigned h){ImageBuffer im;im.width=w;im.height=h;im.channels=1;im.format=PixelFormat::Float32Gray;im.pixels.resize(w*h);return im;}
int main(){try{
    auto k=gray(3,3);k.pixels={.03F,.05F,.07F,.11F,.29F,.13F,.02F,.09F,.21F};
    ImageBuffer im;im.width=7;im.height=6;im.channels=4;im.pixels.resize(7*6*4);
    const auto n=im.pixelCount();std::vector<double> coverage(n),observed(n);
    for(std::size_t p=0;p<n;++p){coverage[p]=p%9==0?0:p%7==0?.3:1;im.pixels[p*4+3]=float(coverage[p]);coverage[p]=im.pixels[p*4+3];
        for(unsigned c=0;c<3;++c)im.pixels[p*4+c]=float(3+c+.13*p+std::sin(p*1.7));observed[p]=im.pixels[p*4];}
    // Explicit finite matrix, independent of FFT/correlation conventions.
    std::vector<double> a(n*n);double ks=0;for(auto v:k.pixels)ks+=v;
    for(int y=0;y<6;++y)for(int x=0;x<7;++x){std::size_t i=y*7+x;double sum=0;
        for(int v=0;v<3;++v)for(int u=0;u<3;++u){int sx=x-(u-1),sy=y-(v-1);if(sx<0||sy<0||sx>=7||sy>=6)continue;
            const auto j=sy*7+sx;a[i*n+j]=k.pixels[v*3+u]/ks*coverage[j];sum+=a[i*n+j];}
        if(sum>0)for(std::size_t j=0;j<n;++j)a[i*n+j]/=sum;
    }
    auto reference=observed;
    for(unsigned iterations:{1U,7U}){
        reference=observed;
        for(unsigned it=0;it<iterations;++it){std::vector<double> model(n),next(n);
            for(std::size_t i=0;i<n;++i)for(std::size_t j=0;j<n;++j)model[i]+=a[i*n+j]*reference[j];
            for(std::size_t j=0;j<n;++j){double adj=0,sensitivity=0;
                for(std::size_t i=0;i<n;++i)if(coverage[i]){adj+=a[i*n+j]*coverage[i]*observed[i]/model[i];sensitivity+=a[i*n+j]*coverage[i];}
                next[j]=coverage[j]?reference[j]*adj/sensitivity:0;
            }reference=next;
        }
        MeasuredDeconvolutionOptions o;o.iterations=iterations;
        auto result=restoreMeasuredPsf(im,k,o);
        for(std::size_t p=0;p<n;++p){require(result.image.pixels[p*4+3]==im.pixels[p*4+3],"coverage changed");
            if(coverage[p])require(std::abs(result.image.pixels[p*4]-reference[p])<2e-5,"FFT does not match dense weighted adjoint RL");}
        o.threads=1;auto serial=restoreMeasuredPsf(im,k,o);
        for(std::size_t p=0;p<n*4;++p)require(std::abs(serial.image.pixels[p]-result.image.pixels[p])<1e-6,"thread parity");
    }
    auto constant=im;for(std::size_t p=0;p<n;++p)for(unsigned c=0;c<3;++c)constant.pixels[p*4+c]=-4;
    MeasuredDeconvolutionOptions o;o.pedestal=8;o.iterations=30;
    auto flat=restoreMeasuredPsf(constant,k,o);for(std::size_t p=0;p<n;++p)if(coverage[p])require(flat.image.pixels[p*4]==-4,"constant boundary or hole changed");
    o.pedestal=0;bool failed=false;try{restoreMeasuredPsf(constant,k,o);}catch(const std::invalid_argument&){failed=true;}require(failed,"negative scientific source accepted silently");
    o.amount=0;auto unchanged=restoreMeasuredPsf(im,k,o);for(std::size_t p=0;p<n;++p)if(coverage[p])require(unchanged.image.pixels[p*4]==im.pixels[p*4],"zero amount not exact");
    o.amount=1;o.protectAbove=8;auto protectedResult=restoreMeasuredPsf(im,k,o);require(protectedResult.protectedPixels>0,"missing protection");
    for(std::size_t p=0;p<n;++p)if(coverage[p]&&protectedResult.protection.pixels[p]==1)require(protectedResult.image.pixels[p*4]==im.pixels[p*4],"protected core changed");
    // Huge damping approaches identity; finite outputs at a zero-filled input.
    o.protectAbove=0;o.noiseSigma=1e12;auto damped=restoreMeasuredPsf(im,k,o);
    for(std::size_t p=0;p<n;++p)if(coverage[p])require(std::abs(damped.image.pixels[p*4]-im.pixels[p*4])<1e-5,"noise damping failed");
    auto zero=gray(9,7);auto z=restoreMeasuredPsf(zero,k);for(auto v:z.image.pixels)require(v==0,"zero image changed");
    for(auto bad:{-1.F,std::numeric_limits<float>::quiet_NaN()}){auto broken=k;broken.pixels[0]=bad;failed=false;try{restoreMeasuredPsf(im,broken);}catch(const std::invalid_argument&){failed=true;}require(failed,"bad PSF accepted");}
    std::cout<<"Measured PSF dense adjoint, boundaries, masks, thread parity and protection passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
