#include "DeepSkyDrizzle.hpp"
#include "photonstack/WorkingMemory.hpp"
#include "photonstack/CfaDrizzle.hpp"
#include "photonstack/SensorMasterBuilder.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/FileDigest.hpp"
#include "photonstack/StageProfile.hpp"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>

namespace photonstack::detail {
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
BayerPattern pattern(const CalibrationAcquisition& a) {
    require(a.text.contains("sampling"), "CFA Drizzle requires an explicit Bayer pattern");
    const auto& s = a.text.at("sampling");
    if (s == "CFA:RGGB") return BayerPattern::RGGB;
    if (s == "CFA:GRBG") return BayerPattern::GRBG;
    if (s == "CFA:GBRG") return BayerPattern::GBRG;
    if (s == "CFA:BGGR") return BayerPattern::BGGR;
    throw std::invalid_argument("CFA Drizzle requires a Bayer sensor plane, not RGB or mono");
}
template<class M> bool sameField(const M& a, const M& b, const char* key) {
    const auto x=a.find(key), y=b.find(key);
    return x==a.end() ? y==b.end() : y!=b.end() && x->second==y->second;
}
}
void validateDrizzleSource(const std::filesystem::path& path, const std::filesystem::path& reference) {
    // This reader respects integer BLANK, signed calibrated values and nonfinite masks.
    // Missing PSCOLOR is accepted only for explicit camera CFA; nonlinear declarations fail.
    SensorFitsReader reader(path, false, SensorFitsReader::Domain::CameraCFA);
    SensorFitsReader ref(reference, false, SensorFitsReader::Domain::CameraCFA);
    const auto& a=reader.metadata(); const auto& b=ref.metadata();
    (void)pattern(a); (void)pattern(b);
    require(a.width==b.width && a.height==b.height, "CFA Drizzle requires matching sensor dimensions");
    for (const auto* key : {"camera", "cameraSerial", "units", "filter", "readoutMode", "calibration"})
        require(sameField(a.text,b.text,key), "CFA Drizzle source acquisition or calibration states differ");
    for (const auto* key : {"exposure", "gain", "iso", "offset", "binX", "binY"})
        require(sameField(a.numeric,b.numeric,key), "CFA Drizzle source exposure, gain or binning differs");
}

ImageBuffer combineCfaDrizzle(const std::vector<std::size_t>& selected,
    const DeepSkyWorkflowOptions& o, const std::filesystem::path& directory,
    DeepSkyWorkflowResult& r, const DeepSkyProgress& progress) {
    StageScope timer(Stage::Combine);
    const auto& reference=r.frames.at(r.referenceIndex).input;
    SensorFitsReader ref(reference, false, SensorFitsReader::Domain::CameraCFA);
    const auto& meta=ref.metadata();
    const double scale=o.drizzleScale;
    const auto width=std::uint32_t(std::ceil(meta.width*scale));
    const auto height=std::uint32_t(std::ceil(meta.height*scale));
    const std::uint64_t pixels=std::uint64_t(width)*height, native=std::uint64_t(meta.width)*meta.height;
    // Double RGB sums/weights + output/weight RGBA buffers + raw + one reference.
    // Finishing has its own working set after the accumulator is destroyed.
    require(pixels<=100000000 && pixels*80+native*24<=workingMemoryBudget(),
            "CFA Drizzle exceeds the available working-memory budget; reduce output scale or image size");
    CfaDrizzleAccumulator accumulator({width,height,0,0,scale,o.drizzlePixfrac});
    auto sky=readOptionalFitsWcs(reference);
    if (sky) {
        sky->grid.x=(sky->grid.x-.5)*scale+.5; sky->grid.y=(sky->grid.y-.5)*scale+.5;
        for (auto& v:sky->grid.cd) v/=scale;
        for (auto& term:sky->distortion) {
            const auto factor=std::pow(scale,1.-term.i-term.j);
            term.a*=factor; term.b*=factor;
        }
    }
    std::ostringstream provenance;
    provenance<<std::setprecision(17)<<"PHOTONSTACK_CFA_DRIZZLE 1\n"
        <<"backend cpu\nscale "<<scale<<"\npixfrac "<<o.drizzlePixfrac
        <<"\ncanvas "<<width<<' '<<height<<"\norigin 0 0\n"
        <<"normalization per-color-covered-surface-brightness\nrejection none\nhole-fill none\n"
        <<"transform source-zero-based-centers-to-native-reference\n";
    for (std::size_t slot=0;slot<selected.size();++slot) {
        const auto i=selected[slot]; const auto& report=r.frames.at(i);
        SensorFitsReader source(report.input, true, SensorFitsReader::Domain::CameraCFA);
        const auto& a=source.metadata();
        std::vector<float> samples(std::size_t(a.width)*a.height); source.read(0,samples);
        require(source.sourceSHA256() == report.sha256, "CFA source bytes changed before accumulation");
        CfaDrizzleFrame f; f.width=a.width; f.height=a.height; f.samples=samples;
        f.pattern=pattern(a); // Metadata already incorporates stored-row Bayer offsets.
        f.weight=o.stack.frameWeights.empty()?1:o.stack.frameWeights.at(i);
        if (i!=r.referenceIndex) {
            if (o.stack.alignAffine) {
                const auto t=report.registration.affine;
                f.transform={t.a,t.b,t.c,t.d,t.dx,t.dy};
            } else if (o.stack.alignSimilarity) {
                const auto t=report.registration.transform;
                const auto c=t.scale*std::cos(t.rotationRadians), s=t.scale*std::sin(t.rotationRadians);
                f.transform={c,-s,s,c,t.dx,t.dy};
            } else if (o.stack.alignTranslation) {
                f.transform[4]=report.registration.translation.dx;
                f.transform[5]=report.registration.translation.dy;
            }
        }
        accumulator.add(f);
        provenance<<"frame "<<i<<' '<<report.sha256<<' '<<std::quoted(report.input.string())
            <<' '<<a.text.at("sampling")<<' '<<f.weight;
        for (const auto v:f.transform) provenance<<' '<<v;
        provenance<<'\n';
        if(progress) progress("cfa-drizzle",.58+.27*(slot+1)/selected.size(),slot+1,selected.size());
    }
    ImageBuffer weights; weights.width=width; weights.height=height; weights.pixels.resize(pixels*4);
    std::array<std::uint64_t,3> covered{}; std::uint64_t complete=0;
    for (std::size_t p=0;p<pixels;++p) {
        bool all=true;
        for(unsigned c=0;c<3;++c) {
            const auto w=accumulator.weights()[p*3+c];
            require(std::isfinite(w) && w<=std::numeric_limits<float>::max(), "CFA coverage weight overflow");
            weights.pixels[p*4+c]=float(w); covered[c]+=w>0; all=all && w>0;
        }
        weights.pixels[p*4+3]=1; // All weights are defined, including genuine zero coverage.
        complete+=all;
    }
    r.drizzleWeights=directory/"drizzle-weights.fits";
    const auto write=[&](const ImageBuffer& image,const std::filesystem::path& path) {
        const auto result=FitsCodec().write(image,path,sky?&sky->grid:nullptr,sky?&sky->distortion:nullptr);
        if(!result.ok) throw std::runtime_error(result.message);
    };
    write(weights,r.drizzleWeights); weights={};
    auto output=accumulator.image();
    r.master=directory/"stack-linear.fits"; write(output,r.master);
    for(unsigned c=0;c<3;++c) r.drizzleCoverage[c]=double(covered[c])/pixels;
    r.drizzleCompleteCoverage=double(complete)/pixels;
    provenance<<"weights "<<fileSHA256(r.drizzleWeights)<<"\nmaster "<<fileSHA256(r.master)<<'\n';
    r.drizzleProvenance=directory/"drizzle-inputs.txt";
    std::ofstream out(r.drizzleProvenance); out<<provenance.str(); out.close();
    if(!out) throw std::runtime_error("Cannot write CFA Drizzle provenance");
    require(complete>0, "No pixels have all three colors; acquire more dither phases or increase pixfrac");
    return output;
}
}
