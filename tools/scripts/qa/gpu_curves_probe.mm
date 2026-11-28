// Experimental display LUT only; deliberately not a production backend.
#include "photonstack/Curves.hpp"
#import <Metal/Metal.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace photonstack;
namespace {
const char* source = R"metal(
#include <metal_stdlib>
using namespace metal;
kernel void curves(device const float4* input [[buffer(0)]], device float4* output [[buffer(1)]],
                   device const float* lut [[buffer(2)]], uint p [[thread_position_in_grid]]) {
    float4 v=input[p];
    for(uint c=0;c<3;++c) {
        float scaled=clamp(v[c],0.f,1.f)*4096.f;
        uint lo=uint(scaled),hi=min(lo+1,4096u);
        v[c]=fma(lut[hi]-lut[lo],scaled-float(lo),lut[lo]);
    }
    output[p]=v;
}
)metal";
struct GPU {
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> pipeline;
    GPU() {
        if(!device) throw std::runtime_error("Metal unavailable");
        queue=[device newCommandQueue];
        auto options=[MTLCompileOptions new];
        if(@available(macOS 15,iOS 18,*)) options.mathMode=MTLMathModeSafe;
        else options.fastMathEnabled=NO;
        auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:source] options:options error:nullptr];
        pipeline=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"curves"] error:nullptr];
        if(!pipeline) throw std::runtime_error("Metal pipeline unavailable");
    }
    id<MTLBuffer> borrow(const PixelStorage& pixels) {
        const auto page=detail::pixelPageSize(),bytes=pixels.size()*sizeof(float);
        return [device newBufferWithBytesNoCopy:const_cast<float*>(pixels.data()) length:(bytes+page-1)/page*page
                                         options:MTLResourceStorageModeShared deallocator:nil];
    }
};
ImageBuffer applyGPU(const ImageBuffer& image,const ImageBuffer& lut) {
    static GPU gpu;
    ImageBuffer output;
    output.width=image.width;output.height=image.height;output.pixels.resize(output.sampleCount());
    const auto input=gpu.borrow(image.pixels),destination=gpu.borrow(output.pixels),lookup=gpu.borrow(lut.pixels);
    if(!input||!destination||!lookup)throw std::runtime_error("Borrowing pages failed");
    const auto command=[gpu.queue commandBuffer];
    const auto encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:gpu.pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];[encoder setBuffer:destination offset:0 atIndex:1];
    [encoder setBuffer:lookup offset:0 atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(image.pixelCount(),1,1)
       threadsPerThreadgroup:MTLSizeMake(gpu.pipeline.threadExecutionWidth,1,1)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted)throw std::runtime_error("GPU execution failed");
    return output;
}
} // namespace
int main(int argc,char**) { @autoreleasepool { try {
    ImageBuffer image;image.width=argc>1?6001:2049;image.height=argc>1?4001:1025;image.pixels.resize(image.sampleCount());
    for(std::size_t p=0;p<image.pixelCount();++p){for(unsigned c=0;c<3;++c)image.pixels[4*p+c]=float((p*719+c*313)%65536)/65535;image.pixels[4*p+3]=1;}
    CurvesOptions options;options.points={{0,.01F},{.2F,.35F},{.6F,.55F},{1,.99F}};
    ImageBuffer gray;gray.width=4097;gray.height=1;gray.channels=1;gray.format=PixelFormat::Float32Gray;gray.pixels.resize(4097);
    for(unsigned i=0;i<4097;++i)gray.pixels[i]=float(i)/4096;
    const auto lut=Curves{}.apply(gray,options).image;
    ImageBuffer reference;
    for(unsigned run=0;run<8;++run) {
        const auto start=std::chrono::steady_clock::now();
        auto result=run%2?applyGPU(image,lut):Curves{}.apply(image,options).image;
        std::cout<<(run%2?"gpu":"cpu")<<','<<run/2<<','<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<'\n'<<std::flush;
        if(result.empty())throw std::runtime_error("Empty result");
        if(run%2) {if(std::memcmp(reference.pixels.data(),result.pixels.data(),image.sampleCount()*sizeof(float)))throw std::runtime_error("Pixels differ");}
        else reference=std::move(result);
    }
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} } }
