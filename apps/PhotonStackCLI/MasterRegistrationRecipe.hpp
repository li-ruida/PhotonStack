#pragma once
#include "ChannelRecipe.hpp"
#include "photonstack/MasterRegistration.hpp"
#include <cmath>

namespace photonstack::cli {
struct MasterRegistrationRecipe {
    std::string mode = "stars", reference;
    std::vector<ChannelSource> sources;
    std::map<std::string, MasterRegistrationMeasurement> measurements;
    std::map<std::string, std::string> products;
};
inline void validateMasterRegistration(const MasterRegistrationRecipe& r, bool saved) {
    if((r.mode!="stars"&&r.mode!="wcs")||r.sources.empty()||r.sources.size()>16)
        throw std::invalid_argument("Select stars or wcs registration and 1..16 masters");
    std::set<std::string> names, products;
    for(const auto& source:r.sources){
        if(!ChannelExpression::validName(source.name)||!names.insert(source.name).second||!source.path.is_absolute()||!channelSHA(source.sha))
            throw std::invalid_argument("Invalid or duplicate registration source");
        products.insert(source.name+".fits");products.insert(source.name+"-validity.fits");
    }
    if(!names.contains(r.reference))throw std::invalid_argument("Reference symbol must name one of the masters");
    if(!saved)return;
    if(r.measurements.size()!=names.size()||r.products.size()!=products.size())throw std::invalid_argument("Incomplete master registration recipe");
    for(const auto& [name,m]:r.measurements){
        const auto& t=m.transform;
        if(!names.contains(name)||!std::isfinite(t.scale)||t.scale<=0||!std::isfinite(t.rotationRadians)||!std::isfinite(t.dx)||!std::isfinite(t.dy)||
           !std::isfinite(m.fitRms)||m.fitRms<0||!std::isfinite(m.median)||m.median<0||!std::isfinite(m.p95)||m.p95<m.median||
           !std::isfinite(m.validFraction)||m.validFraction<=0||m.validFraction>1||m.checkStars>1000000||m.fitStars>1000000)
            throw std::invalid_argument("Invalid saved master registration measurement");
        if((name==r.reference||r.mode=="wcs")&&(t.scale!=1||t.rotationRadians!=0||t.dx!=0||t.dy!=0))
            throw std::invalid_argument("Reference/WCS registration cannot contain a similarity correction");
        if(name!=r.reference&&r.mode=="stars"&&(m.fitStars<12||m.fitRms>.75||m.checkStars<12||m.median>.5||m.p95>1))
            throw std::invalid_argument("Saved stellar registration lacks accepted evidence");
    }
    for(const auto& [name,sha]:r.products)if(!products.contains(name)||!channelSHA(sha))throw std::invalid_argument("Invalid registration product");
}
inline void verifyMasterSources(const MasterRegistrationRecipe& r){
    for(const auto& s:r.sources)if(fileSHA256(s.path)!=s.sha)throw std::runtime_error("Registration source changed: "+s.path.string());
}
inline void writeMasterRegistration(const std::filesystem::path& path,const MasterRegistrationRecipe& r){
    validateMasterRegistration(r,true);
    std::ofstream out(path);out<<std::setprecision(17)<<"PhotonStack-Master-Registration-1\nmode "<<std::quoted(r.mode)<<"\nreference "<<std::quoted(r.reference)<<'\n';
    for(const auto& s:r.sources)out<<"source "<<std::quoted(s.name)<<' '<<std::quoted(s.path.string())<<' '<<s.sha<<'\n';
    for(const auto& [name,m]:r.measurements){const auto& t=m.transform;
        out<<"measurement "<<std::quoted(name)<<' '<<t.scale<<' '<<t.rotationRadians<<' '<<t.dx<<' '<<t.dy<<' '
           <<m.fitStars<<' '<<m.checkStars<<' '<<m.fitRms<<' '<<m.median<<' '<<m.p95<<' '<<m.validFraction<<'\n';}
    for(const auto& [name,sha]:r.products)out<<"product "<<std::quoted(name)<<' '<<sha<<'\n';
    out.flush();if(!out)throw std::runtime_error("Cannot write registration recipe");
}
inline MasterRegistrationRecipe readMasterRegistration(const std::filesystem::path& path,bool verifyOutputs){
    if(std::filesystem::file_size(path)>128*1024)throw std::invalid_argument("Registration recipe too large");
    MasterRegistrationRecipe r;std::ifstream in(path);std::string tag;
    if(!(in>>tag)||tag!="PhotonStack-Master-Registration-1"||!(in>>tag)||tag!="mode"||!(in>>std::quoted(r.mode))||
       !(in>>tag)||tag!="reference"||!(in>>std::quoted(r.reference)))throw std::invalid_argument("Invalid registration recipe header");
    while(in>>tag){
        if(tag=="source"){
            ChannelSource s;std::string pathText;
            if(!(in>>std::quoted(s.name)>>std::quoted(pathText)>>s.sha)||r.sources.size()>=16)throw std::invalid_argument("Invalid registration source record");
            s.path=pathText;r.sources.push_back(s);
        }else if(tag=="measurement"){
            std::string name;MasterRegistrationMeasurement m;auto& t=m.transform;
            if(!(in>>std::quoted(name)>>t.scale>>t.rotationRadians>>t.dx>>t.dy>>m.fitStars>>m.checkStars>>m.fitRms>>m.median>>m.p95>>m.validFraction)||
               !r.measurements.emplace(name,m).second)throw std::invalid_argument("Invalid registration measurement record");
        }else if(tag=="product"){
            std::string name,sha;if(!(in>>std::quoted(name)>>sha)||!r.products.emplace(name,sha).second)throw std::invalid_argument("Invalid registration product record");
        }else throw std::invalid_argument("Unknown registration recipe field");
    }
    if(!in.eof())throw std::invalid_argument("Cannot read registration recipe");
    validateMasterRegistration(r,true);verifyMasterSources(r);
    if(verifyOutputs)for(const auto& [name,sha]:r.products)if(fileSHA256(path.parent_path()/name)!=sha)throw std::runtime_error("Registered product changed: "+name);
    return r;
}
inline void printMasterRegistration(std::ostream& out,const MasterRegistrationRecipe& r,const std::filesystem::path& directory){
    out<<std::setprecision(17)<<"{\"type\":\"complete\",\"command\":\"channel-register\",\"directory\":"<<channelJSON(std::filesystem::absolute(directory).string())
       <<",\"mode\":"<<channelJSON(r.mode)<<",\"reference\":"<<channelJSON(r.reference)<<",\"masters\":[";
    for(std::size_t i=0;i<r.sources.size();++i){if(i)out<<',';const auto& s=r.sources[i];const auto& m=r.measurements.at(s.name);const auto& t=m.transform;
        out<<"{\"name\":"<<channelJSON(s.name)<<",\"source\":"<<channelJSON(s.path.string())<<",\"image\":"<<channelJSON(s.name+".fits")
           <<",\"validity\":"<<channelJSON(s.name+"-validity.fits")<<",\"fitStars\":"<<m.fitStars<<",\"checkStars\":"<<m.checkStars
           <<",\"fitRms\":"<<m.fitRms<<",\"median\":"<<m.median<<",\"p95\":"<<m.p95<<",\"validFraction\":"<<m.validFraction<<",\"transform\":["
           <<t.scale<<','<<t.rotationRadians<<','<<t.dx<<','<<t.dy<<"]}";
    }
    out<<"]}\n";
}
} // namespace photonstack::cli
