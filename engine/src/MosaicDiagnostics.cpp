#include "photonstack/MosaicDiagnostics.hpp"
#include <algorithm>
#include <cmath>
#include <span>

namespace photonstack::detail {
namespace {
double rankedValue(std::span<double> values, std::size_t rank) {
    const auto selected = values.begin() + rank;
    std::nth_element(values.begin(), selected, values.end());
    return *selected;
}
double median(std::span<double> values) {
    return rankedValue(values, values.size() / 2);
}
double quantile(std::span<double> values, double fraction) {
    if (values.empty()) return 0;
    return rankedValue(values, static_cast<std::size_t>(fraction * (values.size() - 1)));
}
using Samples = std::vector<MosaicSeamSample>;
std::optional<MosaicSeamStatistics> statistics(const Samples& samples, unsigned offset, unsigned channels) {
    if (samples.empty()) return std::nullopt;
    MosaicSeamStatistics result;
    result.samples = samples.size();
    for (unsigned c = 0; c < channels; ++c) {
        std::vector<double> first, second, differences, absolute;
        double squares = 0;
        for (const auto& sample : samples) {
            first.push_back(sample[offset][c]); second.push_back(sample[offset + 1][c]);
            const auto d = sample[offset + 1][c] - sample[offset][c];
            differences.push_back(d); absolute.push_back(std::abs(d)); squares += d * d;
        }
        result.firstMedian[c] = median(first); result.secondMedian[c] = median(second);
        result.medianDifference[c] = median(differences);
        result.rmsDifference[c] = std::sqrt(squares / samples.size());
        for (auto& d : differences) d = std::abs(d - result.medianDifference[c]);
        result.madDifference[c] = median(differences);
        result.p95Absolute[c] = rankedValue(absolute, std::size_t(std::ceil(.95 * absolute.size())) - 1);
    }
    return result;
}
void finish(MosaicSeamRegion& region, const Samples& samples, unsigned channels) {
    region.before = statistics(samples, 0, channels);
    region.after = statistics(samples, 2, channels);
}
}
MosaicFinalSeamStatistics measureFinalMosaicSeam(const std::array<double,4>& bounds,
    std::uint16_t channels,const MosaicFinalSampler& sample) {
    MosaicFinalSeamStatistics result;
    if(channels<1||channels>3)return result;
    const int width=int(bounds[2]-bounds[0]),height=int(bounds[3]-bounds[1]);
    if(width<=0||height<=0)return result;
    struct Point {double brightness=0,structure=0;std::array<double,3> gradient{},detail{},range{},noise{},scale{};};
    std::vector<Point> points;std::vector<double> brightness,structure;
    const auto excess=[](double v,double a,double b){return std::max({0.,std::min(a,b)-v,v-std::max(a,b)});};
    const int nx=std::min(48,width),ny=std::min(48,height);
    points.reserve(nx * ny);
    brightness.reserve(nx * ny);
    structure.reserve(nx * ny);
    for(int gy=0;gy<ny;++gy)for(int gx=0;gx<nx;++gx){
        const double x=int(bounds[0])+int((gx+.5)*width/nx),y=int(bounds[1])+int((gy+.5)*height/ny);
        std::array<MosaicFinalSample,5> patch;bool valid=true;
        const int dx[]={0,-1,1,0,0},dy[]={0,0,0,-1,1};
        for(int i=0;i<5;++i){const auto value=sample(x+dx[i],y+dy[i]);if(!value){valid=false;break;}patch[i]=*value;
            for(const auto& colors:patch[i])for(unsigned c=0;c<channels;++c)if(!std::isfinite(colors[c]))valid=false;
        }
        if(!valid)continue;Point p;
        for(unsigned c=0;c<channels;++c){
            std::array<double,3> hp{},gx{},gy{};
            for(unsigned k=0;k<3;++k){hp[k]=patch[0][k][c]-(patch[1][k][c]+patch[2][k][c]+patch[3][k][c]+patch[4][k][c])*.25;
                gx[k]=(patch[2][k][c]-patch[1][k][c])*.5;gy[k]=(patch[4][k][c]-patch[3][k][c])*.5;
            }
            p.brightness+=(patch[0][0][c]+patch[0][1][c])*.5/channels;
            p.structure=std::max({p.structure,std::abs(hp[0]),std::abs(hp[1]),std::hypot(gx[0],gy[0]),std::hypot(gx[1],gy[1])});
            p.gradient[c]=std::hypot(excess(gx[2],gx[0],gx[1]),excess(gy[2],gy[0],gy[1]));
            p.detail[c]=excess(hp[2],hp[0],hp[1]);p.range[c]=excess(patch[0][2][c],patch[0][0][c],patch[0][1][c]);
            p.noise[c]=std::abs(hp[0]-hp[1]);p.scale[c]=std::max(std::abs(patch[0][0][c]),std::abs(patch[0][1][c]));
        }
        points.push_back(p);brightness.push_back(p.brightness);structure.push_back(p.structure);
    }
    result.samples=points.size();if(points.empty())return result;
    const auto dark=quantile(brightness,.4),smooth=quantile(structure,.5),structured=quantile(structure,.75);
    std::array<std::vector<double>,3> gradients,details,ranges,noises,scales;
    for(const auto& p:points){
        const bool background=p.brightness<=dark&&p.structure<=smooth;
        const bool feature=p.structure>=structured&&p.structure>0;
        result.backgroundCandidates+=background;result.structuredSamples+=feature;
        for(unsigned c=0;c<channels;++c){
            if(background){gradients[c].push_back(p.gradient[c]);noises[c].push_back(p.noise[c]);}
            if(feature)details[c].push_back(p.detail[c]);
            ranges[c].push_back(p.range[c]);scales[c].push_back(p.scale[c]);
        }
    }
    bool review=false;
    for(unsigned c=0;c<channels;++c){
        // Relative rounding floor plus robust inter-panel high-pass disagreement.
        // This is a review threshold, not a calibrated probability of a defect.
        result.tolerance[c]=std::max(1e-6*quantile(scales[c],.95),3*1.4826*quantile(noises[c],.5));
        result.tolerance[c]=std::max(1e-12,result.tolerance[c]);
        double squares=0;for(double v:gradients[c])squares+=v*v;
        result.backgroundGradientExcessRms[c]=gradients[c].empty()?0:std::sqrt(squares/gradients[c].size());
        result.backgroundGradientExcessP95[c]=quantile(gradients[c],.95);
        result.structureExcessP95[c]=quantile(details[c],.95);result.rangeExcessP95[c]=quantile(ranges[c],.95);
        std::size_t large=0;for(double v:details[c])large+=v>result.tolerance[c];
        result.structureExcessFraction[c]=details[c].empty()?0:double(large)/details[c].size();
        review=review||result.backgroundGradientExcessP95[c]>result.tolerance[c]||result.structureExcessFraction[c]>.1||result.rangeExcessP95[c]>3*result.tolerance[c];
    }
    if(result.samples>=64&&result.backgroundCandidates>=32)
        result.status=review?"review":"no-large-excess";
    return result;
}
MosaicSeamPair measureMosaicSeam(std::size_t first, std::size_t second,
    const std::array<double, 4>& bounds, std::uint16_t channels, const MosaicSeamSampler& sample) {
    MosaicSeamPair result;
    result.first = first; result.second = second; result.summary.bounds = bounds;
    const int width = int(bounds[2] - bounds[0]), height = int(bounds[3] - bounds[1]);
    if (width <= 0 || height <= 0 || channels < 1 || channels > 3) return result;
    result.cells.resize(16);
    std::array<Samples, 16> cells;
    Samples all;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
        result.cells[y * 4 + x].bounds = {bounds[0] + width * x / 4., bounds[1] + height * y / 4.,
                                        bounds[0] + width * (x + 1) / 4., bounds[1] + height * (y + 1) / 4.};
    const int nx = std::min(48, width), ny = std::min(48, height);
    for (int gy = 0; gy < ny; ++gy) for (int gx = 0; gx < nx; ++gx) {
        const int x = int(bounds[0]) + int((gx + .5) * width / nx);
        const int y = int(bounds[1]) + int((gy + .5) * height / ny);
        const int cell = std::min(3, int((y - bounds[1]) * 4 / height)) * 4 +
                         std::min(3, int((x - bounds[0]) * 4 / width));
        ++result.summary.candidates; ++result.cells[cell].candidates;
        std::array<MosaicSeamSample, 9> patch;
        bool valid = true;
        for (int dy = -1; dy <= 1 && valid; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const auto value = sample(x + dx, y + dy);
            if (!value) { valid = false; break; }
            for (const auto& color : *value) for (unsigned c = 0; c < channels; ++c)
                if (!std::isfinite(color[c])) valid = false;
            patch[(dy + 1) * 3 + dx + 1] = *value;
        }
        if (!valid) continue;
        MosaicSeamSample medians{};
        for (int k = 0; k < 4; ++k) for (unsigned c = 0; c < channels; ++c) {
            std::array<double, 9> values;
            for (std::size_t i = 0; i < patch.size(); ++i) values[i] = patch[i][k][c];
            medians[k][c] = median(values);
        }
        all.push_back(medians); cells[cell].push_back(medians);
    }
    finish(result.summary, all, channels);
    for (int i = 0; i < 16; ++i) finish(result.cells[i], cells[i], channels);
    return result;
}
}
