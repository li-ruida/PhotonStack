#pragma once
#include "photonstack/FileDigest.hpp"
#include "photonstack/FitsCodec.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace photonstack::detail {
// The bundle is bound to the uncropped scientific master, not to a display TIFF.
// No filenames supplied by a manifest are followed. A moved complete project is
// valid; a different master, edited diagnostic or changed provenance is rejected.
struct DeepSkyNoiseReference {
    ImageBuffer image;
    std::filesystem::path imagePath, manifestPath, inputsPath;
};
inline void requireNoiseReference(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
inline DeepSkyNoiseReference saveNoiseReference(const std::filesystem::path& master,
                                               ImageBuffer noise, const std::string& provenance) {
    namespace fs=std::filesystem;
    DeepSkyNoiseReference r;
    r.imagePath=master.parent_path()/"stack-noise.fits";
    r.manifestPath=master.parent_path()/"stack-noise-reference.txt";
    r.inputsPath=master.parent_path()/"stack-noise-inputs.txt";
    for(const auto& p:{r.imagePath,r.manifestPath,r.inputsPath})
        requireNoiseReference(!fs::exists(p),"Noise reference output already exists");
    const auto written=FitsCodec().write(noise,r.imagePath);
    requireNoiseReference(written.ok,written.message.c_str());
    std::ofstream inputs(r.inputsPath,std::ios::binary);inputs<<provenance;inputs.close();
    requireNoiseReference(bool(inputs),"Cannot write noise reference provenance");
    std::ofstream manifest(r.manifestPath,std::ios::binary);
    manifest<<"PHOTONSTACK_NOISE_REFERENCE 1\nmaster "<<fileSHA256(master)
        <<"\nnoise "<<fileSHA256(r.imagePath)<<"\ninputs "<<fileSHA256(r.inputsPath)
        <<"\nshape "<<noise.width<<' '<<noise.height
        <<"\nstage registered-linear-rgba\nestimator sigma-alternating-half-v1\n";
    manifest.close();requireNoiseReference(bool(manifest),"Cannot write noise reference manifest");
    r.image=std::move(noise);return r;
}
inline DeepSkyNoiseReference loadNoiseReference(const std::filesystem::path& master, const ImageBuffer& image) {
    namespace fs=std::filesystem;
    DeepSkyNoiseReference r;
    r.imagePath=master.parent_path()/"stack-noise.fits";
    r.manifestPath=master.parent_path()/"stack-noise-reference.txt";
    r.inputsPath=master.parent_path()/"stack-noise-inputs.txt";
    for(const auto& p:{r.imagePath,r.manifestPath,r.inputsPath})
        requireNoiseReference(fs::is_regular_file(p) && !fs::is_symlink(p),"Matching noise reference is missing; rebuild the stack");
    requireNoiseReference(fs::file_size(r.manifestPath)<=4096 && fs::file_size(r.inputsPath)<=16*1024*1024,
                          "Noise reference metadata exceeds limits");
    std::ifstream manifest(r.manifestPath);
    const auto token=[&](const char* expected){std::string t;manifest>>t;requireNoiseReference(t==expected,"Invalid noise reference metadata");};
    token("PHOTONSTACK_NOISE_REFERENCE");token("1");
    std::string masterHash,noiseHash,inputsHash;std::uint32_t width=0,height=0;
    token("master");manifest>>masterHash;token("noise");manifest>>noiseHash;token("inputs");manifest>>inputsHash;
    token("shape");manifest>>width>>height;
    token("stage");token("registered-linear-rgba");token("estimator");token("sigma-alternating-half-v1");
    std::string extra;requireNoiseReference(!(manifest>>extra),"Unexpected noise reference metadata");
    requireNoiseReference(width==image.width && height==image.height && masterHash==fileSHA256(master) &&
                          noiseHash==fileSHA256(r.imagePath) && inputsHash==fileSHA256(r.inputsPath),
                          "Noise reference does not match this master or provenance");
    FitsDecodeOptions options;options.mode=FitsDecodeMode::Scientific;options.maskNonFinitePixels=false;
    auto read=FitsCodec().read(r.imagePath,options);
    requireNoiseReference(read.ok,read.message.c_str());
    requireNoiseReference(read.image.width==width && read.image.height==height && read.image.channels==4 &&
                          read.image.colorEncoding==ColorEncoding::Linear,"Noise reference geometry or units mismatch");
    r.image=std::move(read.image);return r;
}
} // namespace photonstack::detail
