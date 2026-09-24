#include "photonstack/MultiscaleDenoiser.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace photonstack {
namespace {
int reflect(int x, int size) {
    if (size == 1) return 0;
    const int period = 2 * (size - 1);
    x %= period; if (x < 0) x += period;
    return x < size ? x : period - x;
}
double median(std::vector<float>& values) {
    auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end()); return *middle;
}
std::vector<float> structureSupport(const std::vector<float>& detail, unsigned w, unsigned h, double threshold, unsigned radius) {
    const auto stride=std::size_t(w)+1;
    std::vector<unsigned> summed(stride*(h+1),0);
    for(unsigned y=0;y<h;++y) {
        unsigned row=0;
        for(unsigned x=0;x<w;++x) {
            row+=std::abs(detail[std::size_t(y)*w+x])>threshold;
            summed[(std::size_t(y)+1)*stride+x+1]=row+summed[std::size_t(y)*stride+x+1];
        }
    }
    std::vector<float> support(detail.size());
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        const auto x0=x>radius?x-radius:0,y0=y>radius?y-radius:0;
        const auto x1=std::min(w,x+radius+1),y1=std::min(h,y+radius+1);
        const double n=summed[std::size_t(y1)*stride+x1]+summed[std::size_t(y0)*stride+x0]-
            summed[std::size_t(y0)*stride+x1]-summed[std::size_t(y1)*stride+x0];
        support[std::size_t(y)*w+x]=float(std::min(1.,8*n/((x1-x0)*(y1-y0))));
    }
    return support;
}
std::vector<float> smooth(const std::vector<float>& input, unsigned w, unsigned h, int step) {
    std::vector<float> temporary(input.size()), output(input.size());
    constexpr float k[5] = {1.f/16, 4.f/16, 6.f/16, 4.f/16, 1.f/16};
    for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x) {
        double sum=0;
        for(int i=-2;i<=2;++i) sum+=k[i+2]*input[std::size_t(y)*w+reflect(int(x)+i*step,w)];
        temporary[std::size_t(y)*w+x]=float(sum);
    }
    for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x) {
        double sum=0;
        for(int i=-2;i<=2;++i) sum+=k[i+2]*temporary[std::size_t(reflect(int(y)+i*step,h))*w+x];
        output[std::size_t(y)*w+x]=float(sum);
    }
    return output;
}

}
NonlocalDenoiseResult MultiscaleDenoiser::apply(const ImageBuffer& image, const ImageBuffer& blend,
                                               const MultiscaleDenoiseOptions& options,
                                               const ImageBuffer* noiseReference) const {
    const auto fail=[](const char* message) { NonlocalDenoiseResult r; r.errorCode="ArgumentInvalid"; r.message=message; return r; };
    if (image.colorEncoding!=ColorEncoding::Linear || image.empty() || (image.channels!=3 && image.channels!=4) || image.pixels.size()!=image.sampleCount() ||
        blend.width!=image.width || blend.height!=image.height || blend.channels!=1 || blend.pixels.size()!=image.pixelCount())
        return fail("Multiscale denoising requires RGB and a matching grayscale blend map");
    if (!std::isfinite(options.luminance) || !std::isfinite(options.chroma) || options.luminance<0 || options.luminance>1 ||
        options.chroma<0 || options.chroma>1 || options.scales<1 || options.scales>6 ||
        (options.noiseReferenceScope != MultiscaleNoiseReferenceScope::AllComponents &&
         options.noiseReferenceScope != MultiscaleNoiseReferenceScope::LuminanceOnly))
        return fail("Invalid multiscale strength or scale count");
    for(std::size_t p=0;p<image.pixelCount();++p) {
        if (!std::isfinite(blend.pixels[p]) || blend.pixels[p]<0 || blend.pixels[p]>1) return fail("Invalid blend weight");
        for(unsigned c=0;c<image.channels;++c) if(!std::isfinite(image.pixels[p*image.channels+c])) return fail("Nonfinite image sample");
        if(image.channels==4 && image.pixels[p*4+3]!=1) return fail("Full coverage is required");
    }
    if(noiseReference) {
        const auto& n=*noiseReference;
        if(n.colorEncoding!=ColorEncoding::Linear || n.width!=image.width || n.height!=image.height ||
           (n.channels!=3 && n.channels!=4) || n.pixels.size()!=n.sampleCount())
            return fail("Noise reference requires matching linear RGB geometry");
        for(std::size_t p=0;p<n.pixelCount();++p) {
            for(unsigned c=0;c<n.channels;++c)
                if(!std::isfinite(n.pixels[p*n.channels+c])) return fail("Nonfinite noise reference sample");
            if(n.channels==4 && n.pixels[p*4+3]!=1) return fail("Noise reference requires full coverage");
        }
    }
    NonlocalDenoiseResult result; result.ok=true; result.image=image;
    if(options.luminance==0 && options.chroma==0) return result;
    const auto count=image.pixelCount();
    // Process planes separately to bound peak memory at full sensor resolution.
    std::vector<std::vector<float>> luminanceSupport(options.scales);
    for(unsigned component=0;component<3;++component) {
        const double amount=component==0?options.luminance:options.chroma;
        if(amount==0 && component!=0) continue;
        const bool useReference = noiseReference && (component==0 ||
            options.noiseReferenceScope==MultiscaleNoiseReferenceScope::AllComponents);
        const bool preserveSceneSupport = component==0 && noiseReference && options.chroma>0 &&
            options.noiseReferenceScope==MultiscaleNoiseReferenceScope::LuminanceOnly;
        std::vector<float> current(count), removed(count,0), noiseCurrent;
        if(useReference) noiseCurrent.resize(count);
        for(std::size_t p=0;p<count;++p) {
            const auto i=p*image.channels;
            current[p]=component==0 ? float(.2126*image.pixels[i]+.7152*image.pixels[i+1]+.0722*image.pixels[i+2]) :
                image.pixels[i+(component==1?0:2)]-image.pixels[i+1];
            if(useReference) {
                const auto j=p*noiseReference->channels;
                const auto& samples=noiseReference->pixels;
                noiseCurrent[p]=component==0 ? float(.2126*samples[j]+.7152*samples[j+1]+.0722*samples[j+2]) :
                    samples[j+(component==1?0:2)]-samples[j+1];
            }
        }
        for(unsigned scale=0;scale<options.scales;++scale) {
            auto next=smooth(current,image.width,image.height,1<<scale);
            auto noiseNext=useReference ? smooth(noiseCurrent,image.width,image.height,1<<scale) : std::vector<float>{};
            std::vector<float> samples; const auto stride=std::max<std::size_t>(1,count/200000);
            for(std::size_t p=0;p<count;p+=stride) if(blend.pixels[p]>.5F)
                samples.push_back(useReference ? noiseCurrent[p]-noiseNext[p] : current[p]-next[p]);
            if(samples.size()<32) break;
            const double center=median(samples);
            for(auto& value:samples) value=float(std::abs(value-center));
            const double sigma=1.4826*median(samples);
            std::vector<float> details;
            if(sigma>1.e-10 || preserveSceneSupport) {
                details.resize(count);
                for(std::size_t p=0;p<count;++p) details[p]=current[p]-next[p];
            }
            if(preserveSceneSupport) {
                // Color protection must not change just because the luminance
                // threshold uses an independent noise estimate. This also runs
                // for a zero-noise reference, where luminance is a bypass.
                samples.clear();
                for(std::size_t p=0;p<count;p+=stride) if(blend.pixels[p]>.5F)
                    samples.push_back(details[p]);
                const double sceneCenter=median(samples);
                for(auto& value:samples) value=float(std::abs(value-sceneCenter));
                const double sceneSigma=1.4826*median(samples);
                if(sceneSigma>1.e-10)
                    luminanceSupport[scale]=structureSupport(details,image.width,image.height,4*sceneSigma,2U<<scale);
            }
            if(sigma>1.e-10) {
                const double threshold=(component==0?2.0:2.8)*sigma;
                auto support=structureSupport(details,image.width,image.height,2*threshold,2U<<scale);
                if(component==0 && !preserveSceneSupport) luminanceSupport[scale]=support;
                else if(component!=0 && !luminanceSupport[scale].empty())
                    for(std::size_t p=0;p<count;++p) support[p]=std::max(support[p],luminanceSupport[scale][p]);
                for(std::size_t p=0;p<count;++p) {
                    const double detail=double(current[p])-next[p];
                    // Firm threshold: strong detail is unchanged, weak detail is
                    // suppressed continuously; coarse structure is never removed.
                    const double keep = std::clamp((std::abs(detail)-threshold)/threshold,0.0,1.0);
                    removed[p]+=float(detail*(1-keep)*(1-support[p]));
                }
            }
            current=std::move(next);
            if(useReference) noiseCurrent=std::move(noiseNext);
        }
        for(std::size_t p=0;p<count;++p) {
            if(blend.pixels[p]==0) continue;
            const double delta=amount*blend.pixels[p]*removed[p]; const auto i=p*image.channels;
            if(component==0) for(unsigned c=0;c<3;++c) result.image.pixels[i+c]-=float(delta);
            else if(component==1) {
                result.image.pixels[i]-=float(.7874*delta);
                result.image.pixels[i+1]+=float(.2126*delta); result.image.pixels[i+2]+=float(.2126*delta);
            } else {
                result.image.pixels[i]-=float(-.0722*delta);
                result.image.pixels[i+1]+=float(.0722*delta); result.image.pixels[i+2]-=float(.9278*delta);
            }
        }
    }
    for(float value:result.image.pixels) if(!std::isfinite(value)) return fail("Output exceeds finite Float32 range");
    return result;
}
}
