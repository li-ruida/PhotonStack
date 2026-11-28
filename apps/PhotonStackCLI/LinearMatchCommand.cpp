#include "LinearMatchCommand.hpp"
#include "OutputPath.hpp"
#include "ChannelRecipe.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/LinearMatch.hpp"
#include <cmath>
#include <iostream>

namespace {
using namespace photonstack;using namespace photonstack::cli;namespace fs=std::filesystem;
struct Recipe {
    ChannelSource reference, target;
    LinearMatchOptions options;
    LinearMatchMeasurement measurement;
    std::map<std::string,std::string> products;
};
void validate(const Recipe& r,bool saved){
    if(r.reference.name==r.target.name)throw std::invalid_argument("Select distinct reference and target names");
    for(const auto* s:{&r.reference,&r.target}){
        if(!ChannelExpression::validName(s->name)||!s->path.is_absolute()||!channelSHA(s->sha))throw std::invalid_argument("Invalid intensity source");
        if(fileSHA256(s->path)!=s->sha)throw std::runtime_error("Intensity source changed: "+s->path.string());
    }
    const auto& o=r.options;
    if(!std::isfinite(o.lowQuantile)||!std::isfinite(o.highQuantile)||o.lowQuantile<0||o.highQuantile>1||o.highQuantile-o.lowQuantile<.1)
        throw std::invalid_argument("Invalid matching quantiles");
    if(!saved)return;
    const auto& m=r.measurement;const auto& c=m.coefficients;
    if(!std::isfinite(c.gain)||c.gain<=0||!std::isfinite(c.offset)||m.candidates>65536||m.fitSamples<64||m.checkSamples<64||
       m.fitSamples+m.checkSamples>m.candidates||m.downweighted>m.fitSamples||!std::isfinite(m.medianResidual))throw std::invalid_argument("Invalid intensity measurement");
    for(double v:{m.beforeRms,m.afterRms,m.p95AbsoluteResidual,m.referenceSpan,m.validFraction})
        if(!std::isfinite(v)||v<0)throw std::invalid_argument("Invalid intensity statistics");
    if(m.validFraction<=0||m.validFraction>1)throw std::invalid_argument("Invalid intensity coverage");
    std::size_t count=0;
    for(const auto& s:m.regions){
        if(s.count>m.checkSamples||!std::isfinite(s.beforeMean)||!std::isfinite(s.afterMean)||!std::isfinite(s.afterRms)||s.afterRms<0)
            throw std::invalid_argument("Invalid intensity region");
        count+=s.count;
    }
    if(count!=m.checkSamples)throw std::invalid_argument("Invalid intensity region counts");
    if(r.products.size()!=3)throw std::invalid_argument("Incomplete intensity products");
    for(const auto* name:{"matched.fits","validity.fits","residual.fits"})
        if(!r.products.contains(name)||!channelSHA(r.products.at(name)))throw std::invalid_argument("Invalid intensity product");
}
void write(const fs::path& path,const Recipe& r){
    validate(r,true);std::ofstream out(path);const auto& m=r.measurement;
    out<<std::setprecision(17)<<"PhotonStack-Linear-Match-1\naligned 1\n";
    for(const auto* s:{&r.reference,&r.target})out<<std::quoted(s->name)<<' '<<std::quoted(s->path.string())<<' '<<s->sha<<'\n';
    out<<r.options.lowQuantile<<' '<<r.options.highQuantile<<'\n'<<m.coefficients.gain<<' '<<m.coefficients.offset<<'\n'
       <<m.candidates<<' '<<m.fitSamples<<' '<<m.checkSamples<<' '<<m.downweighted<<'\n'
       <<m.beforeRms<<' '<<m.afterRms<<' '<<m.medianResidual<<' '<<m.p95AbsoluteResidual<<' '<<m.referenceSpan<<' '<<m.validFraction<<'\n';
    for(const auto& s:m.regions)out<<s.count<<' '<<s.beforeMean<<' '<<s.afterMean<<' '<<s.afterRms<<'\n';
    for(const auto& [name,sha]:r.products)out<<name<<' '<<sha<<'\n';
    out.flush();if(!out)throw std::runtime_error("Cannot save intensity recipe");
}
Recipe read(const fs::path& path,bool products){
    if(fs::file_size(path)>16384)throw std::invalid_argument("Intensity recipe too large");
    Recipe r;std::ifstream in(path);std::string word;
    if(!(in>>word)||word!="PhotonStack-Linear-Match-1"||!(in>>word)||word!="aligned"||!(in>>word)||word!="1")throw std::invalid_argument("Invalid intensity recipe header");
    for(auto* s:{&r.reference,&r.target}){std::string p;in>>std::quoted(s->name)>>std::quoted(p)>>s->sha;s->path=p;}
    auto& m=r.measurement;
    in>>r.options.lowQuantile>>r.options.highQuantile>>m.coefficients.gain>>m.coefficients.offset
      >>m.candidates>>m.fitSamples>>m.checkSamples>>m.downweighted>>m.beforeRms>>m.afterRms>>m.medianResidual>>m.p95AbsoluteResidual>>m.referenceSpan>>m.validFraction;
    for(auto& s:m.regions)in>>s.count>>s.beforeMean>>s.afterMean>>s.afterRms;
    for(int i=0;i<3;++i){std::string name,sha;if(!(in>>name>>sha)||!r.products.emplace(name,sha).second)throw std::invalid_argument("Invalid intensity product record");}
    if(!in||in>>word||!in.eof())throw std::invalid_argument("Invalid intensity recipe records");
    validate(r,true);
    if(products)for(const auto& [name,sha]:r.products)if(fileSHA256(path.parent_path()/name)!=sha)throw std::runtime_error("Intensity product changed: "+name);
    return r;
}
void print(const Recipe& r,const fs::path& directory){
    const auto& m=r.measurement;
    std::cout<<std::setprecision(17)<<"{\"type\":\"complete\",\"command\":\"channel-match\",\"directory\":"<<channelJSON(fs::absolute(directory).string());
    for(const auto& [name,s]:std::array<std::pair<const char*,const ChannelSource*>,2>{{{"reference",&r.reference},{"target",&r.target}}})
        std::cout<<",\""<<name<<"\":{\"name\":"<<channelJSON(s->name)<<",\"path\":"<<channelJSON(s->path.string())<<'}';
    std::cout<<",\"low\":"<<r.options.lowQuantile<<",\"high\":"<<r.options.highQuantile<<",\"gain\":"<<m.coefficients.gain<<",\"offset\":"<<m.coefficients.offset
       <<",\"candidates\":"<<m.candidates<<",\"fitSamples\":"<<m.fitSamples<<",\"checkSamples\":"<<m.checkSamples<<",\"downweighted\":"<<m.downweighted
       <<",\"beforeRms\":"<<m.beforeRms<<",\"afterRms\":"<<m.afterRms<<",\"medianResidual\":"<<m.medianResidual<<",\"p95AbsoluteResidual\":"<<m.p95AbsoluteResidual
       <<",\"referenceSpan\":"<<m.referenceSpan<<",\"validFraction\":"<<m.validFraction<<",\"regions\":[";
    for(std::size_t i=0;i<m.regions.size();++i){const auto& s=m.regions[i];if(i)std::cout<<',';
        std::cout<<"{\"count\":"<<s.count<<",\"beforeMean\":"<<s.beforeMean<<",\"afterMean\":"<<s.afterMean<<",\"afterRms\":"<<s.afterRms<<'}';}
    std::cout<<"]}\n";
}
void checkGrid(const LinearMonoMaster& a,const LinearMonoMaster& b){
    if(!a.sky||!b.sky)return;
    const SkyProjection ref(*a.sky,a.sky->grid),other(*b.sky,a.sky->grid);
    for(int j=0;j<9;++j)for(int i=0;i<9;++i){
        const double x=(a.image.width-1)*(i/8.0),y=(a.image.height-1)*(j/8.0);
        const auto p=other.toCanvas(x,y),q=ref.toPanel(p.x,p.y);
        if(!std::isfinite(q.x)||!std::isfinite(q.y)||std::hypot(q.x-x,q.y-y)>.05)throw std::invalid_argument("Intensity source WCS grids differ; register first");
    }
}
}
int runLinearMatch(const std::vector<std::string>& args){
    Recipe r;fs::path directory,recipe;bool aligned=false;std::set<std::string> seen;
    const bool fresh=args[0]=="channel-match",inspect=args[0]=="channel-match-inspect";
    for(std::size_t i=1;i<args.size();++i){
        const auto key=args[i];if(!seen.insert(key).second)throw std::invalid_argument("Duplicate intensity option");
        if(key=="--aligned"&&fresh){aligned=true;continue;}
        if(++i>=args.size())throw std::invalid_argument("Missing intensity option value");
        const auto value=args[i];
        if(key=="--output-dir"&&!inspect)directory=value;
        else if(key=="--recipe"&&!fresh)recipe=value;
        else if((key=="--reference"||key=="--input")&&fresh){
            const auto split=value.find('=');if(split==std::string::npos)throw std::invalid_argument("Use symbol=/path/master.fits");
            auto& s=key=="--reference"?r.reference:r.target;s={value.substr(0,split),fs::canonical(value.substr(split+1)),{}};
        }else if((key=="--low"||key=="--high")&&fresh){
            std::size_t used=0;const double v=std::stod(value,&used);if(used!=value.size())throw std::invalid_argument("Invalid quantile");
            (key=="--low"?r.options.lowQuantile:r.options.highQuantile)=v;
        }else throw std::invalid_argument("Unknown intensity option: "+key);
    }
    if (!inspect) photonstack::cli::requireNewOutputPath(directory);
    if(!fresh){if(recipe.empty())throw std::invalid_argument("Specify --recipe");r=read(recipe,inspect);if(inspect){print(r,recipe.parent_path());return 0;}}
    else {
        if (!aligned) throw std::invalid_argument("Confirm registered masters with --aligned");
        r.reference.sha = fileSHA256(r.reference.path);
        r.target.sha = fileSHA256(r.target.path);
        validate(r, false);
    }
    const auto reference=readLinearMonoMaster(r.reference.path),target=readLinearMonoMaster(r.target.path);checkGrid(reference,target);
    auto output=matchLinearMaster(reference.image,target.image,r.options,fresh?nullptr:&r.measurement.coefficients);
    if(!fresh)output.measurement.downweighted=r.measurement.downweighted; // saved fit diagnostic; no new fit during replay
    r.measurement=output.measurement;
    if(!fs::create_directory(directory))throw std::invalid_argument("Choose a new output directory");
    try{
        r.products.clear();
        for(const auto& [name,image]:std::array<std::pair<const char*,const ImageBuffer*>,3>{{{"matched.fits",&output.image},{"validity.fits",&output.validity},{"residual.fits",&output.residual}}}){
            const auto result=FitsCodec().write(*image,directory/name,target.sky?&target.sky->grid:nullptr,target.sky?&target.sky->distortion:nullptr);
            if(!result.ok)throw std::runtime_error(result.message);r.products[name]=fileSHA256(directory/name);
        }
        write(directory/"recipe.txt",r);print(r,directory);
    }catch(...){std::error_code ec;for(const char* f:{"matched.fits","validity.fits","residual.fits","recipe.txt"})fs::remove(directory/f,ec);fs::remove(directory,ec);throw;}
    return 0;
}
