#include "../../engine/src/RegisteredStackCache.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace photonstack;
namespace fs = std::filesystem;
void require(bool ok) { if (!ok) throw std::runtime_error("Registered RAM cache regression"); }
void equal(const ImageBuffer& a, const ImageBuffer& b) {
    require(a.width == b.width && a.height == b.height && a.channels == b.channels && a.pixels.size() == b.pixels.size());
    require(a.pixels.empty() || std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size()*sizeof(float)) == 0);
}
int main() {
    const auto root = fs::temp_directory_path()/ ("photonstack-ram-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup { fs::path p; ~Cleanup() {std::error_code e;fs::remove_all(p,e);} } cleanup{root};
    std::vector<fs::path> diskPaths, mixedPaths;
    std::vector<detail::RegisteredFrame> disk, mixed;
    for (unsigned f=0; f<19; ++f) {
        ImageBuffer image; image.width=131; image.height=83; image.pixels.resize(image.sampleCount());
        for (std::size_t p=0;p<image.pixelCount();++p) {
            for (unsigned c=0;c<3;++c) image.pixels[p*4+c] = float((p*71+f*43+c*91)%500)*.1f - 10.f;
            image.pixels[p*4+3] = p%19==0 ? 0.f : f%3==0 ? .3f : 1.f;
        }
        detail::RegisteredFrame a,b;
        diskPaths.push_back(root/("disk"+std::to_string(f)));
        mixedPaths.push_back(root/("mixed"+std::to_string(f)));
        require(detail::writeRegisteredFrame(diskPaths.back(),image,true,a).ok);
        require(detail::writeRegisteredFrame(mixedPaths.back(),image,true,b,f%2==0).ok);
        require(bool(b.resident)==(f%2==0));
        require(fs::exists(mixedPaths.back())==(f%2!=0));
        disk.push_back(a);mixed.push_back(b);
    }
    for (auto method : {StackMethod::Median,StackMethod::SigmaClip,StackMethod::WinsorizedSigmaClip,StackMethod::PercentileClip}) {
        StackOptions o; o.method=method; o.normalizeBackground=true; o.collectRejectionMaps=true;
        o.combineTileBytes=19*4*sizeof(float)*17; // cross-row and short final tiles
        o.combineWorkers=3;
        for (unsigned i=0;i<19;++i) o.frameWeights.push_back(float(i%5+1));
        if(method==StackMethod::SigmaClip) {
            o.sigma.medianMad=true;o.collectNoiseDiagnostic=true;o.collectAcceptedWeights=true;
            for(unsigned i=0;i<19;++i) {o.frameNoiseVariances.push_back(i%7+1);o.acceptedGroups.push_back(i%2);}
        }
        auto a=detail::combineRegisteredFrames(diskPaths,disk,o),b=detail::combineRegisteredFrames(mixedPaths,mixed,o);
        require(a.ok&&b.ok);equal(a.image,b.image);equal(a.rejectionLow,b.rejectionLow);equal(a.rejectionHigh,b.rejectionHigh);
        equal(a.noiseDiagnostic,b.noiseDiagnostic);equal(a.acceptedWeights,b.acceptedWeights);
        for(unsigned g=0;g<2;++g) {equal(a.acceptedGroupImages[g],b.acceptedGroupImages[g]);equal(a.acceptedGroupWeights[g],b.acceptedGroupWeights[g]);}
        require(a.comparedSamples==b.comparedSamples&&a.rejectedLowSamples==b.rejectedLowSamples&&a.rejectedHighSamples==b.rejectedHighSamples);
        o.progress=[](const auto& p){if(p.stage==StackProgressStage::Combining&&p.row>0) throw std::runtime_error("cancel");};
        bool cancelled=false;try{detail::combineRegisteredFrames(mixedPaths,mixed,o);}catch(const std::runtime_error& e){cancelled=std::string(e.what())=="cancel";} require(cancelled);
    }
    StackOptions one;one.method=StackMethod::Median;
    const auto a=detail::combineRegisteredFrames({diskPaths[0]},{disk[0]},one);
    const auto b=detail::combineRegisteredFrames({mixedPaths[0]},{mixed[0]},one);
    require(a.ok&&b.ok);equal(a.image,b.image);
    std::cout<<"RAM/disk mixed-cache, weighted rejection, normalization, groups, short tiles and cancellation exact\n";
}
