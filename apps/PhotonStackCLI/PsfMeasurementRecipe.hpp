#pragma once
#include "ChannelRecipe.hpp"
#include "photonstack/PsfMeasurement.hpp"
#include <cmath>
#include <iostream>

namespace photonstack::cli {
struct PsfRecipe {
    std::filesystem::path source;
    std::string sha;
    PsfMeasureOptions options;
    PsfMeasurement measurement;
    std::map<std::string,std::string> products;
};
inline void validatePsfRecipe(const PsfRecipe& r){
    const auto& o=r.options;const auto& m=r.measurement;
    if(!r.source.is_absolute()||!channelSHA(r.sha)||!std::set<std::string>{"luminance","red","green","blue"}.contains(o.channel)||
       !std::isfinite(o.sigmaThreshold)||o.sigmaThreshold<2||o.sigmaThreshold>30||!std::isfinite(o.saturation)||o.saturation<0)
        throw std::invalid_argument("Invalid PSF source or options");
    if(!m.width||!m.height||m.stars.size()>2048||m.accepted<12||m.accepted>m.stars.size()||m.detected<m.stars.size())
        throw std::invalid_argument("Invalid PSF sample counts");
    for(double v:{m.xx,m.yy,m.majorFwhm,m.minorFwhm,m.fwhmP10,m.fwhmP50,m.fwhmP90,m.medianResidual})
        if(!std::isfinite(v)||v<0)throw std::invalid_argument("Invalid PSF statistics");
    if(!std::isfinite(m.xy)||!std::isfinite(m.angleDegrees)||m.angleDegrees < -90||m.angleDegrees>90||m.fwhmP10<=0||m.fwhmP50<m.fwhmP10||m.fwhmP90<m.fwhmP50||m.majorFwhm<m.minorFwhm)
        throw std::invalid_argument("Invalid PSF shape");
    (void)gaussianPsfKernel(m.xx,m.yy,m.xy);
    std::set<std::size_t> excluded(o.excluded.begin(),o.excluded.end());
    if(excluded.size()!=o.excluded.size()||(!excluded.empty()&&*excluded.rbegin()>=m.stars.size()))throw std::invalid_argument("Invalid PSF exclusions");
    std::size_t used=0,regions=0;
    const std::set<std::string> statuses{"accepted","excluded","edge","masked","saturated","crowded","fit-failed","shape-outlier"};
    for(std::size_t i=0;i<m.stars.size();++i){const auto& s=m.stars[i];
        if(s.id!=i||s.sampleX>=m.width||s.sampleY>=m.height||!statuses.contains(s.status)||!std::isfinite(s.x)||!std::isfinite(s.y)||s.x<0||s.y<0||s.x>=m.width||s.y>=m.height||
           !std::isfinite(s.xx)||!std::isfinite(s.yy)||!std::isfinite(s.xy)||!std::isfinite(s.residual)||s.residual<0||
           (excluded.contains(i)!=(s.status=="excluded")))throw std::invalid_argument("Invalid PSF star record");
        if(s.status=="accepted"){++used;if(s.xx*s.yy-s.xy*s.xy<=0)throw std::invalid_argument("Invalid fitted PSF star covariance");}
    }
    for(const auto& s:m.regions){if(s.count>m.accepted||!std::isfinite(s.fwhm)||s.fwhm<0||!std::isfinite(s.xx)||!std::isfinite(s.yy)||!std::isfinite(s.xy)||(s.count>0&&s.xx*s.yy-s.xy*s.xy<=0))throw std::invalid_argument("Invalid PSF region");regions+=s.count;}
    if(used!=m.accepted||regions!=m.accepted||r.products.size()!=2||!r.products.contains("psf.fits")||!r.products.contains("stars-preview.tiff"))
        throw std::invalid_argument("Incomplete PSF recipe");
    for(const auto& [name,sha]:r.products)if(!channelSHA(sha))throw std::invalid_argument("Invalid PSF product digest");
}
inline void verifyPsfSource(const PsfRecipe& r){if(fileSHA256(r.source)!=r.sha)throw std::runtime_error("PSF source changed: "+r.source.string());}
inline void writePsfRecipe(const std::filesystem::path& path,const PsfRecipe& r){
    validatePsfRecipe(r);verifyPsfSource(r);const auto& m=r.measurement;
    std::ofstream out(path);out<<std::setprecision(17)<<"PhotonStack-PSF-Measurement-1\n"<<std::quoted(r.source.string())<<' '<<r.sha<<'\n'
       <<r.options.channel<<' '<<r.options.sigmaThreshold<<' '<<r.options.saturation<<' '<<r.options.excluded.size();
    for(auto id:r.options.excluded)out<<' '<<id;
    out<<'\n'<<m.width<<' '<<m.height<<' '<<m.detected<<' '<<m.accepted<<' '<<m.xx<<' '<<m.yy<<' '<<m.xy<<' '
       <<m.majorFwhm<<' '<<m.minorFwhm<<' '<<m.angleDegrees<<' '<<m.fwhmP10<<' '<<m.fwhmP50<<' '<<m.fwhmP90<<' '<<m.medianResidual<<' '<<m.spatiallyVariable<<'\n';
    for(const auto& region:m.regions)out<<region.count<<' '<<region.fwhm<<' '<<region.xx<<' '<<region.yy<<' '<<region.xy<<'\n';
    out<<m.stars.size()<<'\n';
    for(const auto& s:m.stars)out<<s.id<<' '<<s.sampleX<<' '<<s.sampleY<<' '<<s.x<<' '<<s.y<<' '<<s.xx<<' '<<s.yy<<' '<<s.xy<<' '<<s.residual<<' '<<s.status<<'\n';
    for(const auto& [name,sha]:r.products)out<<name<<' '<<sha<<'\n';
    out.flush();if(!out)throw std::runtime_error("Cannot save PSF recipe");
}
inline PsfRecipe readPsfRecipe(const std::filesystem::path& path){
    if(std::filesystem::file_size(path)>1024*1024)throw std::invalid_argument("PSF recipe exceeds 1 MiB");
    PsfRecipe r;std::ifstream in(path);std::string token,source;std::size_t count=0;
    if(!(in>>token)||token!="PhotonStack-PSF-Measurement-1")throw std::invalid_argument("Invalid PSF recipe header");
    in>>std::quoted(source)>>r.sha>>r.options.channel>>r.options.sigmaThreshold>>r.options.saturation>>count;r.source=source;
    if(!in||count>2048)throw std::invalid_argument("Invalid PSF exclusions");
    for(std::size_t i=0;i<count;++i){std::size_t id;in>>id;r.options.excluded.push_back(id);}
    auto& m=r.measurement;
    in>>m.width>>m.height>>m.detected>>m.accepted>>m.xx>>m.yy>>m.xy>>m.majorFwhm>>m.minorFwhm>>m.angleDegrees
      >>m.fwhmP10>>m.fwhmP50>>m.fwhmP90>>m.medianResidual>>m.spatiallyVariable;
    for(auto& region:m.regions)in>>region.count>>region.fwhm>>region.xx>>region.yy>>region.xy;
    in>>count;if(!in||count>2048)throw std::invalid_argument("Invalid PSF star count");
    for(std::size_t i=0;i<count;++i){PsfStarMeasurement s;in>>s.id>>s.sampleX>>s.sampleY>>s.x>>s.y>>s.xx>>s.yy>>s.xy>>s.residual>>s.status;m.stars.push_back(s);}
    for(int i=0;i<2;++i){std::string name,sha;if(!(in>>name>>sha)||!r.products.emplace(name,sha).second)throw std::invalid_argument("Invalid PSF product");}
    if(!in||in>>token||!in.eof())throw std::invalid_argument("Invalid PSF recipe records");
    validatePsfRecipe(r);verifyPsfSource(r);
    for(const auto& [name,sha]:r.products)if(fileSHA256(path.parent_path()/name)!=sha)throw std::runtime_error("PSF product changed: "+name);
    return r;
}
inline void printPsfRecipe(const PsfRecipe& r,const std::filesystem::path& directory){
    const auto& m=r.measurement;
    std::cout<<std::setprecision(17)<<"{\"type\":\"complete\",\"command\":\"psf-measure\",\"directory\":"<<channelJSON(std::filesystem::absolute(directory).string())
       <<",\"source\":"<<channelJSON(r.source.string())<<",\"sha\":"<<channelJSON(r.sha)<<",\"channel\":"<<channelJSON(r.options.channel)
       <<",\"sigma\":"<<r.options.sigmaThreshold<<",\"saturation\":"<<r.options.saturation<<",\"width\":"<<m.width<<",\"height\":"<<m.height<<",\"detected\":"<<m.detected<<",\"accepted\":"<<m.accepted
       <<",\"xx\":"<<m.xx<<",\"yy\":"<<m.yy<<",\"xy\":"<<m.xy<<",\"majorFwhm\":"<<m.majorFwhm<<",\"minorFwhm\":"<<m.minorFwhm<<",\"angle\":"<<m.angleDegrees
       <<",\"fwhmP10\":"<<m.fwhmP10<<",\"fwhmP50\":"<<m.fwhmP50<<",\"fwhmP90\":"<<m.fwhmP90<<",\"medianResidual\":"<<m.medianResidual
       <<",\"spatiallyVariable\":"<<(m.spatiallyVariable?"true":"false")<<",\"regions\":[";
    for(std::size_t i=0;i<16;++i){if(i)std::cout<<',';const auto& s=m.regions[i];std::cout<<"{\"count\":"<<s.count<<",\"fwhm\":"<<s.fwhm<<",\"xx\":"<<s.xx<<",\"yy\":"<<s.yy<<",\"xy\":"<<s.xy<<'}';}
    std::cout<<"],\"stars\":[";
    for(std::size_t i=0;i<m.stars.size();++i){if(i)std::cout<<',';const auto& s=m.stars[i];
        std::cout<<"{\"id\":"<<s.id<<",\"sampleX\":"<<s.sampleX<<",\"sampleY\":"<<s.sampleY<<",\"x\":"<<s.x<<",\"y\":"<<s.y<<",\"xx\":"<<s.xx<<",\"yy\":"<<s.yy<<",\"xy\":"<<s.xy<<",\"residual\":"<<s.residual<<",\"status\":"<<channelJSON(s.status)<<'}';}
    std::cout<<"]}\n";
}
} // namespace photonstack::cli
