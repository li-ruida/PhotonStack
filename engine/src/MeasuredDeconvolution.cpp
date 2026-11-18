#include "photonstack/MeasuredDeconvolution.hpp"
#include "MaskedImageSampling.hpp"
#include "vendor/pocketfft_hdronly.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace photonstack {
namespace {
unsigned smoothSize(unsigned n) {
    for (;;++n) { unsigned q=n;for(unsigned factor:{2U,3U,5U})while(q%factor==0)q/=factor;if(q==1)return n; }
}
class Convolution {
    unsigned width,height,fw,fh,threads;
    std::vector<double> real;
    std::vector<std::complex<double>> spectrum,transfer;
    pocketfft::shape_t shape,axes{0,1};
    pocketfft::stride_t rs,cs;
public:
    Convolution(unsigned w,unsigned h,const ImageBuffer& kernel,unsigned workers)
        :width(w),height(h),fw(smoothSize(w+kernel.width-1)),fh(smoothSize(h+kernel.height-1)),threads(workers),
         real(std::size_t(fw)*fh),spectrum(std::size_t(fh)*(fw/2+1)),transfer(spectrum.size()),
         shape{fh,fw},rs{std::ptrdiff_t(fw*sizeof(double)),sizeof(double)},
         cs{std::ptrdiff_t((fw/2+1)*sizeof(std::complex<double>)),sizeof(std::complex<double>)} {
        double sum=0;for(float v:kernel.pixels)sum+=v;
        for(unsigned y=0;y<kernel.height;++y)for(unsigned x=0;x<kernel.width;++x)
            real[std::size_t((y+fh-kernel.height/2)%fh)*fw+(x+fw-kernel.width/2)%fw]=kernel.pixels[std::size_t(y)*kernel.width+x]/sum;
        pocketfft::r2c(shape,rs,cs,axes,true,real.data(),transfer.data(),1.,threads);
    }
    unsigned paddedWidth() const{return fw;}
    unsigned paddedHeight() const{return fh;}
    void apply(const std::vector<double>& input,std::vector<double>& output,bool transpose=false) {
        std::fill(real.begin(),real.end(),0);
        for(unsigned y=0;y<height;++y)std::copy_n(input.data()+std::size_t(y)*width,width,real.data()+std::size_t(y)*fw);
        pocketfft::r2c(shape,rs,cs,axes,true,real.data(),spectrum.data(),1.,threads);
        for(std::size_t i=0;i<spectrum.size();++i)spectrum[i]*=transpose?std::conj(transfer[i]):transfer[i];
        pocketfft::c2r(shape,cs,rs,axes,false,spectrum.data(),real.data(),1./real.size(),threads);
        output.resize(input.size());
        for(unsigned y=0;y<height;++y)std::copy_n(real.data()+std::size_t(y)*fw,width,output.data()+std::size_t(y)*width);
    }
};
void store(float& destination,double value) {
    if(!std::isfinite(value)||std::abs(value)>std::numeric_limits<float>::max())throw std::overflow_error("PSF restoration exceeds Float32 range");
    destination=float(value);
}
// Chessboard distance to thresholded cores. Linear time, including large cores.
std::vector<double> protection(const ImageBuffer& image,double limit,unsigned radius) {
    const auto n=image.pixelCount();const int w=int(image.width),h=int(image.height),cap=int(2*radius+1);
    std::vector<int> distance(n,cap);
    if(limit>0)for(std::size_t p=0;p<n;++p)if(detail::pixelHasValidColor(image,p))
        for(unsigned c=0;c<detail::colorChannelCount(image);++c)if(image.pixels[p*image.channels+c]>=limit)distance[p]=0;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){const auto p=std::size_t(y)*w+x;
        if(x>0)distance[p]=std::min(distance[p],distance[p-1]+1);
        if(y>0)for(int dx=-1;dx<=1;++dx)if(x+dx>=0&&x+dx<w)distance[p]=std::min(distance[p],distance[p-w+dx]+1);
    }
    for(int y=h-1;y>=0;--y)for(int x=w-1;x>=0;--x){const auto p=std::size_t(y)*w+x;
        if(x+1<w)distance[p]=std::min(distance[p],distance[p+1]+1);
        if(y+1<h)for(int dx=-1;dx<=1;++dx)if(x+dx>=0&&x+dx<w)distance[p]=std::min(distance[p],distance[p+w+dx]+1);
    }
    std::vector<double> weights(n);
    if(limit>0)for(std::size_t p=0;p<n;++p)if(distance[p]<int(2*radius))weights[p]=distance[p]<=int(radius)?1:.5*(1+std::cos(std::numbers::pi*(double(distance[p])/radius-1)));
    return weights;
}
}
MeasuredDeconvolutionResult restoreMeasuredPsf(const ImageBuffer& source,const ImageBuffer& kernel,const MeasuredDeconvolutionOptions& o) {
    if(source.empty()||(source.channels!=1&&source.channels!=3&&source.channels!=4)||source.pixels.size()!=source.sampleCount()||
       source.width>16384||source.height>16384||source.pixelCount()>16*1024*1024||source.colorEncoding!=ColorEncoding::Linear||detail::imageHasInvalidCoveredColor(source))
        throw std::invalid_argument("PSF restoration requires a finite linear mono/RGB image up to 16 megapixels");
    if(kernel.empty()||kernel.channels!=1||kernel.pixels.size()!=kernel.sampleCount()||kernel.width>63||kernel.height>63||!(kernel.width%2)||!(kernel.height%2)||kernel.colorEncoding!=ColorEncoding::Linear)
        throw std::invalid_argument("PSF kernel must be linear mono, odd-sized and at most 63 by 63");
    double kernelSum=0;for(float v:kernel.pixels){if(!std::isfinite(v)||v<0)throw std::invalid_argument("PSF kernel must be finite and nonnegative");kernelSum+=v;}
    if(!(kernelSum>0)||kernel.pixels[(kernel.height/2)*kernel.width+kernel.width/2]<=0)throw std::invalid_argument("PSF kernel requires positive flux and center");
    if(!o.iterations||o.iterations>100||!o.threads||o.threads>16||!std::isfinite(o.amount)||o.amount<0||o.amount>1||
       !std::isfinite(o.pedestal)||o.pedestal<0||!std::isfinite(o.noiseSigma)||o.noiseSigma<0||!std::isfinite(o.protectAbove)||o.protectAbove<0)
        throw std::invalid_argument("Invalid measured PSF restoration options");
    const auto n=source.pixelCount();const auto colors=detail::colorChannelCount(source);
    MeasuredDeconvolutionResult result;result.image=source;result.difference=source;result.residual=source;
    result.protection.width=source.width;result.protection.height=source.height;result.protection.channels=1;
    result.protection.format=PixelFormat::Float32Gray;result.protection.colorEncoding=ColorEncoding::Linear;result.protection.pixels.resize(n);
    auto protectedWeight=protection(source,o.protectAbove,std::max(1U,std::max(kernel.width,kernel.height)/2));
    std::vector<double> coverage(n);
    for(std::size_t p=0;p<n;++p){
        coverage[p]=detail::pixelHasValidColor(source,p)?detail::pixelCoverage(source,p):0;
        if(coverage[p]>0){++result.coveredPixels;if(protectedWeight[p]>0)++result.protectedPixels;}
        result.protection.pixels[p]=coverage[p]>0?float(protectedWeight[p]):std::numeric_limits<float>::quiet_NaN();
        if(!coverage[p])for(auto* im:{&result.image,&result.difference,&result.residual})detail::clearMaskedPixel(*im,p);
        for(unsigned c=0;c<colors;++c)if(coverage[p]&&source.pixels[p*source.channels+c]+o.pedestal<0)
            throw std::invalid_argument("RL requires nonnegative samples; specify an explicit pedestal for negative calibrated values");
    }
    if(!result.coveredPixels)throw std::invalid_argument("PSF restoration has no covered pixels");
    Convolution convolution(source.width,source.height,kernel,o.threads);result.fftWidth=convolution.paddedWidth();result.fftHeight=convolution.paddedHeight();
    std::vector<double> support,sensitivity,work(n),model,correction;
    convolution.apply(coverage,support);
    for(std::size_t p=0;p<n;++p){
        if(coverage[p]&&support[p]<=0)throw std::runtime_error("PSF support is numerically singular");
        work[p]=coverage[p]?coverage[p]/support[p]:0;
    }
    convolution.apply(work,sensitivity,true);
    for(std::size_t p=0;p<n;++p)if(coverage[p]&&sensitivity[p]<=0)throw std::runtime_error("PSF adjoint sensitivity is numerically singular");
    for(unsigned c=0;c<colors;++c){
        bool same=c>0;
        if(same)for(std::size_t p=0;p<n;++p)if(coverage[p]&&source.pixels[p*source.channels+c]!=source.pixels[p*source.channels]){same=false;break;}
        if(same){
            for(auto* im:{&result.image,&result.difference,&result.residual})for(std::size_t p=0;p<n;++p)im->pixels[p*source.channels+c]=im->pixels[p*source.channels];
            result.channels.push_back(result.channels.front());continue;
        }
        std::vector<double> observed(n),estimate(n);double scale=0,weightSum=0;
        DeconvolutionChannelMetrics metrics;
        for(std::size_t p=0;p<n;++p)if(coverage[p]){
            observed[p]=double(source.pixels[p*source.channels+c])+o.pedestal;scale=std::max(scale,observed[p]);
            metrics.beforeFlux+=coverage[p]*source.pixels[p*source.channels+c];weightSum+=coverage[p];
        }
        const double floor=std::max(scale*1e-12,std::numeric_limits<double>::min());
        for(std::size_t p=0;p<n;++p)estimate[p]=coverage[p]?std::max(observed[p],floor):0;
        auto forward=[&](const std::vector<double>& values){
            for(std::size_t p=0;p<n;++p)work[p]=values[p]*coverage[p];
            convolution.apply(work,model);
            for(std::size_t p=0;p<n;++p)model[p]=coverage[p]?std::max(0.,model[p]/support[p]):0;
        };
        forward(observed);
        for(std::size_t p=0;p<n;++p)metrics.beforeRms+=coverage[p]*std::pow(observed[p]-model[p],2);
        for(unsigned iteration=0;iteration<o.iterations;++iteration){
            forward(estimate);
            for(std::size_t p=0;p<n;++p){
                if(!coverage[p]){work[p]=0;continue;}
                const double predicted=std::max(model[p],floor),residual=observed[p]-predicted;
                const double damp=o.noiseSigma>0?std::pow(residual/std::hypot(residual,o.noiseSigma),2):1;
                const double ratio=std::max(0.,1+damp*residual/predicted);
                work[p]=coverage[p]*ratio/support[p];
            }
            convolution.apply(work,correction,true);
            for(std::size_t p=0;p<n;++p)if(coverage[p]){
                estimate[p]*=std::max(0.,correction[p]/sensitivity[p]);
                if(!std::isfinite(estimate[p]))throw std::overflow_error("PSF iteration exceeds numeric range");
            }
        }
        for(std::size_t p=0;p<n;++p)if(coverage[p]){
            const auto index=p*source.channels+c;const double amount=o.amount*(1-protectedWeight[p]);
            const double restored=amount==0?source.pixels[index]:source.pixels[index]+amount*(estimate[p]-observed[p]);
            store(result.image.pixels[index],restored);store(result.difference.pixels[index],double(result.image.pixels[index])-source.pixels[index]);
            estimate[p]=double(result.image.pixels[index])+o.pedestal;metrics.afterFlux+=coverage[p]*result.image.pixels[index];
        }
        forward(estimate);
        for(std::size_t p=0;p<n;++p)if(coverage[p]){
            const double residual=observed[p]-model[p];store(result.residual.pixels[p*source.channels+c],residual);
            metrics.afterRms+=coverage[p]*residual*residual;
        }
        metrics.beforeRms=std::sqrt(metrics.beforeRms/weightSum);metrics.afterRms=std::sqrt(metrics.afterRms/weightSum);result.channels.push_back(metrics);
    }
    return result;
}
}
