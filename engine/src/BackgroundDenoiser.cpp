#include "photonstack/BackgroundDenoiser.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace photonstack {
namespace {
int reflect(int x, int size) {
    if(size==1) return 0;
    const int period=2*(size-1); x%=period; if(x<0)x+=period;
    return x<size?x:period-x;
}
void smooth(std::vector<float>& values, unsigned w, unsigned h) {
    std::vector<float> temp(values.size());
    constexpr double kernel[]{1./16,4./16,6./16,4./16,1./16};
    for(int step: {1,2,4,8}) {
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
            double sum=0;
            for(int k=-2;k<=2;++k)sum+=kernel[k+2]*values[std::size_t(y)*w+reflect(int(x)+k*step,w)];
            temp[std::size_t(y)*w+x]=float(sum);
        }
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
            double sum=0;
            for(int k=-2;k<=2;++k)sum+=kernel[k+2]*temp[std::size_t(reflect(int(y)+k*step,h))*w+x];
            values[std::size_t(y)*w+x]=float(sum);
        }
    }
}
double median(std::vector<float>& a) {
    auto m=a.begin()+a.size()/2;std::nth_element(a.begin(),m,a.end());return *m;
}
std::vector<float> protectPeaks(const ImageBuffer& image, const ImageBuffer& blend) {
    const auto w=image.width,h=image.height;
    std::vector<double> integral((std::size_t(w)+1)*(h+1),0);
    for(unsigned y=0;y<h;++y) {
        double row=0;
        for(unsigned x=0;x<w;++x) {
            const auto p=(std::size_t(y)*w+x)*image.channels;
            row+=.2126*image.pixels[p]+.7152*image.pixels[p+1]+.0722*image.pixels[p+2];
            integral[(std::size_t(y)+1)*(w+1)+x+1]=row+integral[std::size_t(y)*(w+1)+x+1];
        }
    }
    const auto mean=[&](unsigned x,unsigned y,unsigned r) {
        const auto x0=x>r?x-r:0,y0=y>r?y-r:0,x1=std::min(w,x+r+1),y1=std::min(h,y+r+1);
        return (integral[std::size_t(y1)*(w+1)+x1]+integral[std::size_t(y0)*(w+1)+x0]-
            integral[std::size_t(y1)*(w+1)+x0]-integral[std::size_t(y0)*(w+1)+x1])/((x1-x0)*(y1-y0));
    };
    // A small matched footprint rejects individual noisy pixels. Measure this
    // band's noise directly: the earlier Haar estimate misses correlated noise.
    std::vector<float> detail(image.pixelCount()),samples;
    const auto stride=std::max<std::size_t>(1,image.pixelCount()/200000);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        const auto p=std::size_t(y)*w+x;detail[p]=float(mean(x,y,1)-mean(x,y,6));
        if(p%stride==0 && blend.pixels[p]>.5F)samples.push_back(detail[p]);
    }
    auto mask=blend.pixels;
    if(samples.size()<32)return mask;
    const auto center=median(samples);
    for(auto& v:samples)v=float(std::abs(v-center));
    const auto threshold=center+5*1.4826*median(samples);
    for(unsigned y=1;y+1<h;++y)for(unsigned x=1;x+1<w;++x) {
        const auto p=std::size_t(y)*w+x;
        if(detail[p]<=threshold)continue;
        bool peak=true;
        for(int dy=-1;dy<=1 && peak;++dy)for(int dx=-1;dx<=1;++dx) {
            if(dx==0 && dy==0)continue;
            if(detail[std::size_t(int(y)+dy)*w+int(x)+dx]>=detail[p]){peak=false;break;}
        }
        if(!peak)continue;
        for(unsigned yy=y>10?y-10:0;yy<std::min(h,y+11);++yy)
            for(unsigned xx=x>10?x-10:0;xx<std::min(w,x+11);++xx) {
                const auto t=std::clamp((std::hypot(double(xx)-x,double(yy)-y)-6)/4,0.,1.);
                auto& weight=mask[std::size_t(yy)*w+xx];weight=std::min(weight,float(t*t*(3-2*t)));
            }
    }
    return mask;
}
}
NonlocalDenoiseResult BackgroundDenoiser::apply(const ImageBuffer& image, const ImageBuffer& blend,
                                               double strength) const {
    const auto fail=[](const char* message){NonlocalDenoiseResult r;r.errorCode="ArgumentInvalid";r.message=message;return r;};
    if(image.empty() || image.colorEncoding!=ColorEncoding::Linear || (image.channels!=3 && image.channels!=4) ||
       image.pixels.size()!=image.sampleCount() || blend.width!=image.width || blend.height!=image.height ||
       blend.channels!=1 || blend.pixels.size()!=image.pixelCount() || !std::isfinite(strength) || strength<0 || strength>1)
        return fail("Background denoising requires linear RGB, a matching blend map and strength in [0,1]");
    for(std::size_t p=0;p<image.pixelCount();++p) {
        if(!std::isfinite(blend.pixels[p]) || blend.pixels[p]<0 || blend.pixels[p]>1)return fail("Invalid blend weight");
        for(unsigned c=0;c<image.channels;++c)if(!std::isfinite(image.pixels[p*image.channels+c]))return fail("Nonfinite image sample");
        if(image.channels==4 && image.pixels[p*4+3]!=1)return fail("Full coverage is required");
    }
    NonlocalDenoiseResult out;out.ok=true;out.image=image;
    if(strength==0)return out;
    const auto n=image.pixelCount();
    const auto mask=protectPeaks(image,blend);
    auto weights=mask;smooth(weights,image.width,image.height);
    std::vector<float> residual(n*3),keep(n,0);
    // Estimate a background-only guide. Bright sources excluded by the map
    // cannot be smeared into their surroundings by the convolution.
    for(unsigned c=0;c<3;++c) {
        std::vector<float> guide(n);
        for(std::size_t p=0;p<n;++p)guide[p]=image.pixels[p*image.channels+c]*mask[p];
        smooth(guide,image.width,image.height);
        for(std::size_t p=0;p<n;++p)
            residual[p*3+c]=weights[p]>.05F ? image.pixels[p*image.channels+c]-guide[p]/weights[p] : 0;
    }
    for(float v:residual)if(!std::isfinite(v))return fail("Background residual exceeds finite Float32 range");
    // Shared luminance/chroma significance protects colored structures without
    // independently thresholding channels, which would change their ratios.
    for(unsigned component=0;component<3;++component) {
        const auto value=[&](std::size_t p) {
            const auto i=p*3;
            return component==0 ? .2126*residual[i]+.7152*residual[i+1]+.0722*residual[i+2] :
                double(residual[i+(component==1?0:2)])-residual[i+1];
        };
        std::vector<float> samples;
        const auto stride=std::max<std::size_t>(1,n/200000);
        for(std::size_t p=0;p<n;p+=stride)if(mask[p]>.95F && weights[p]>.5F)samples.push_back(float(value(p)));
        if(samples.size()<32)return out;
        const auto center=median(samples);
        for(auto& v:samples)v=float(std::abs(v-center));
        const auto sigma=1.4826*median(samples);
        if(sigma<=1.e-12) {
            for(std::size_t p=0;p<n;++p)if(std::abs(value(p))>1.e-10)keep[p]=1;
        } else for(std::size_t p=0;p<n;++p)
            keep[p]=std::max(keep[p],float(std::clamp((std::abs(value(p))-2*sigma)/(2*sigma),0.,1.)));
    }
    for(std::size_t p=0;p<n;++p) {
        if(mask[p]==0 || weights[p]<=.05F)continue;
        const auto amount=strength*mask[p]*(1-keep[p]);
        for(unsigned c=0;c<3;++c)out.image.pixels[p*image.channels+c]-=float(amount*residual[p*3+c]);
    }
    for(float v:out.image.pixels)if(!std::isfinite(v))return fail("Output exceeds finite Float32 range");
    return out;
}
}
