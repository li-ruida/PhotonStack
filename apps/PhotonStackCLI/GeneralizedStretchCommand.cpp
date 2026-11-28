#include "GeneralizedStretchCommand.hpp"
#include "OutputPath.hpp"
#include "ChannelRecipe.hpp"
#include "photonstack/GeneralizedStretch.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/ImageResizer.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <iostream>

namespace {
using namespace photonstack;
using namespace photonstack::cli;
namespace fs=std::filesystem;
struct Recipe {
    fs::path source;
    std::string sha;
    GeneralizedStretchOptions options;
    std::vector<GeneralizedStretchOptions> prefix;
    unsigned previewWidth=0;
    std::map<std::string,std::string> products;
};
std::set<std::string> names(unsigned width) {
    std::set<std::string> result{"before.tiff","after.tiff","report.json"};
    if(!width){result.insert("display.fits");result.insert("developed.tiff");}
    return result;
}
void verifySource(const Recipe& r) {
    (void)GeneralizedStretchCurve(r.options);
    if(r.prefix.size()>31)throw std::invalid_argument("At most 32 GHS steps are supported");
    for(const auto& step:r.prefix)(void)GeneralizedStretchCurve(step);
    if(!r.source.is_absolute()||!channelSHA(r.sha)||r.previewWidth>2048)
        throw std::invalid_argument("Invalid GHS recipe");
    if(fileSHA256(r.source)!=r.sha)throw std::runtime_error("GHS source changed");
}
std::string mode(const GeneralizedStretchOptions& o){return o.saturation?"saturation":o.luminance?"luminance":"independent";}
void writeOptions(std::ostream& out,const GeneralizedStretchOptions& o){
    out<<o.logStretch<<' '<<o.localIntensity<<' '<<o.symmetry<<' '<<o.shadowProtection<<' '<<o.highlightProtection<<' '
       <<o.black<<' '<<o.white<<' '<<mode(o)<<' '<<o.inverse<<'\n';
}
void readOptions(std::istream& in,GeneralizedStretchOptions& o,bool legacy){
    std::string mode;int inverse=0;
    if(!(in>>o.logStretch>>o.localIntensity>>o.symmetry>>o.shadowProtection>>o.highlightProtection>>o.black>>o.white>>mode)||
       (mode!="luminance"&&mode!="independent"&&(!legacy&&mode!="saturation")))throw std::invalid_argument("Invalid GHS step");
    if(legacy&&mode!="luminance"&&mode!="independent")throw std::invalid_argument("Invalid legacy GHS mode");
    if(!legacy&&(!(in>>inverse)||(inverse!=0&&inverse!=1)))throw std::invalid_argument("Invalid inverse flag");
    o.luminance=mode=="luminance";o.saturation=mode=="saturation";o.inverse=inverse!=0;
    (void)GeneralizedStretchCurve(o);
}
void saveRecipe(const fs::path& path,const Recipe& r) {
    verifySource(r);std::ofstream out(path);
    out<<std::setprecision(17)<<"PhotonStack-GHS-2\n"<<std::quoted(r.source.string())<<' '<<r.sha<<'\n'
       <<r.previewWidth<<' '<<r.prefix.size()+1<<'\n';
    for(const auto& step:r.prefix)writeOptions(out,step);writeOptions(out,r.options);
    for(const auto& [name,sha]:r.products)out<<name<<' '<<sha<<'\n';
    out.flush();if(!out)throw std::runtime_error("Cannot save GHS recipe");
}
Recipe readRecipe(const fs::path& file,bool products) {
    if(fs::file_size(file)>16384)throw std::invalid_argument("GHS recipe too large");
    Recipe r;std::ifstream in(file);std::string header,source;
    if(!(in>>header>>std::quoted(source)>>r.sha)||(header!="PhotonStack-GHS-1"&&header!="PhotonStack-GHS-2"))throw std::invalid_argument("Invalid GHS recipe header");
    if(header=="PhotonStack-GHS-1"){
        readOptions(in,r.options,true);if(!(in>>r.previewWidth))throw std::invalid_argument("Invalid preview width");
    }else{
        unsigned count=0;if(!(in>>r.previewWidth>>count)||count<1||count>32)throw std::invalid_argument("Invalid GHS step count");
        for(unsigned i=0;i<count;++i){GeneralizedStretchOptions o;readOptions(in,o,false);if(i+1==count)r.options=o;else r.prefix.push_back(o);}
    }
    if(r.previewWidth>2048)throw std::invalid_argument("Invalid preview width");
    r.source=source;const auto expected=names(r.previewWidth);
    std::string name,sha;
    while(in>>name){if(!(in>>sha)||!expected.contains(name)||!channelSHA(sha)||!r.products.emplace(name,sha).second)
        throw std::invalid_argument("Invalid GHS product record");}
    if(!in.eof()||r.products.size()!=expected.size())throw std::invalid_argument("Incomplete GHS recipe");
    verifySource(r);
    if(products)for(const auto& [n,s]:r.products)if(fileSHA256(file.parent_path()/n)!=s)throw std::runtime_error("GHS product changed: "+n);
    return r;
}
void writeImage(const ImageBuffer& image,const fs::path& path) {
    ImageWriteOptions o;o.bitDepth=ImageWriteBitDepth::Sixteen;o.colorSpace=ImageWriteColorSpace::SRGB;
    const auto result=ImageCodec().write(image,path,o);if(!result.ok)throw std::runtime_error(result.message);
}
void writePreview(const ImageBuffer& image,const fs::path& path,unsigned width) {
    if(image.width<=width){writeImage(image,path);return;}
    auto scaled=ImageResizer().resizeToWidth(image,width);if(!scaled.ok)throw std::runtime_error(scaled.message);
    writeImage(scaled.image,path);
}
std::string report(const Recipe& r,const DevelopableFitsImage& input,const GeneralizedStretchResult& result) {
    const auto& o=r.options;std::ostringstream out;out<<std::setprecision(17);
    out<<"{\"type\":\"complete\",\"command\":\"ghs-stretch\",\"source\":"<<channelJSON(r.source.string())
       <<",\"sourceSHA\":"<<channelJSON(r.sha)<<",\"sourceDomain\":"<<channelJSON(input.image.colorEncoding==ColorEncoding::Linear?"linear":"display-srgb")
       <<",\"inverse\":"<<(o.inverse?"true":"false")<<",\"saturation\":"<<(o.saturation?"true":"false")<<",\"stepCount\":"<<r.prefix.size()+1
       <<",\"outputDomain\":\"display-srgb\",\"width\":"<<input.image.width<<",\"height\":"<<input.image.height
       <<",\"previewWidth\":"<<r.previewWidth<<",\"wcsPreserved\":"<<(input.sky?"true":"false")
       <<",\"logStretch\":"<<o.logStretch<<",\"localIntensity\":"<<o.localIntensity<<",\"symmetry\":"<<o.symmetry
       <<",\"shadowProtection\":"<<o.shadowProtection<<",\"highlightProtection\":"<<o.highlightProtection
       <<",\"black\":"<<o.black<<",\"white\":"<<o.white<<",\"luminance\":"<<(o.luminance?"true":"false")
       <<",\"coveredPixels\":"<<result.coveredPixels<<",\"belowBlack\":"<<result.belowBlack<<",\"aboveWhite\":"<<result.aboveWhite
       <<",\"gamutBlendedPixels\":"<<result.gamutBlendedPixels<<",\"beforeHistogram\":[";
    for(unsigned i=0;i<256;++i){if(i)out<<',';out<<result.beforeHistogram[i];}out<<"],\"afterHistogram\":[";
    for(unsigned i=0;i<256;++i){if(i)out<<',';out<<result.afterHistogram[i];}out<<"],\"curve\":[";
    GeneralizedStretchCurve curve(o);for(unsigned i=0;i<=1024;++i){if(i)out<<',';out<<curve.evaluate(i/1024.);}
    out<<"]}\n";return out.str();
}
}
int runGeneralizedStretch(const std::vector<std::string>& args) {
    Recipe r;fs::path output,recipe,chain;bool prefixOnly=false;std::string expectedSHA;bool blackSet=false,whiteSet=false;
    const bool fresh=args[0]=="ghs-stretch",inspect=args[0]=="ghs-inspect";std::set<std::string> seen;
    for(std::size_t i=1;i<args.size();i+=2){
        const auto& key=args[i];if(i+1>=args.size()||!seen.insert(key).second)throw std::invalid_argument("Missing or duplicate GHS option");const auto& value=args[i+1];
        if(key=="--input"&&fresh)r.source=fs::canonical(value);
        else if(key=="--expected-sha"&&fresh)expectedSHA=value;
        else if(key=="--recipe"&&!fresh)recipe=value;
        else if(key=="--chain-recipe"&&fresh)chain=value;
        else if((key=="--chain-prefix-only"||key=="--inverse")&&fresh){
            if(value!="on"&&value!="off")throw std::invalid_argument("Expected on or off");
            if(key=="--inverse")r.options.inverse=value=="on";else prefixOnly=value=="on";
        }
        else if(key=="--output-dir"&&!inspect)output=value;
        else if(key=="--mode"&&fresh){if(value!="luminance"&&value!="independent"&&value!="saturation")throw std::invalid_argument("GHS mode must be luminance, independent or saturation");r.options.luminance=value=="luminance";r.options.saturation=value=="saturation";}
        else if(fresh&&std::set<std::string>{"--log-stretch","--local-intensity","--symmetry","--shadow-protection","--highlight-protection","--black","--white","--preview-width"}.contains(key)){
            std::size_t used=0;const double v=std::stod(value,&used);if(used!=value.size()||!std::isfinite(v))throw std::invalid_argument("Invalid GHS number");auto& o=r.options;
            if(key=="--log-stretch")o.logStretch=v;else if(key=="--local-intensity")o.localIntensity=v;else if(key=="--symmetry")o.symmetry=v;
            else if(key=="--shadow-protection")o.shadowProtection=v;else if(key=="--highlight-protection")o.highlightProtection=v;
            else if(key=="--black"){o.black=v;blackSet=true;}else if(key=="--white"){o.white=v;whiteSet=true;}
            else{if(v<0||v>2048||v!=std::floor(v))throw std::invalid_argument("GHS preview width must be 0...2048");r.previewWidth=unsigned(v);}
        }else throw std::invalid_argument("Unknown GHS option: "+key);
    }
    if (!inspect) photonstack::cli::requireNewOutputPath(output);
    if(!fresh){
        if(recipe.empty())throw std::invalid_argument("Specify --recipe");r=readRecipe(recipe,inspect);
        if(inspect){std::ifstream in(recipe.parent_path()/"report.json");std::cout<<in.rdbuf();return 0;}
    }else{
        if(r.source.empty())throw std::invalid_argument("Specify --input");
        if(blackSet!=whiteSet)throw std::invalid_argument("Specify both --black and --white, or neither for automatic input range");
        r.sha=fileSHA256(r.source);if(!expectedSHA.empty()&&expectedSHA!=r.sha)throw std::runtime_error("GHS source changed");
    }
    if(fresh&&!chain.empty()){
        const auto prior=readRecipe(chain,true);
        if(prior.source!=r.source||prior.sha!=r.sha)throw std::invalid_argument("GHS chain refers to a different source");
        r.prefix=prior.prefix;if(!prefixOnly)r.prefix.push_back(prior.options);
    }else if(prefixOnly)throw std::invalid_argument("Prefix mode requires a chain recipe");
    DevelopableFitsImage input;
    if(isFitsPath(r.source))input=readDevelopableFits(r.source);
    else{
        auto ext=r.source.extension().string();std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(ext!=".tiff"&&ext!=".tif"&&ext!=".png"&&ext!=".jpg"&&ext!=".jpeg")throw std::invalid_argument("Select a FITS master or developed TIFF/PNG/JPEG");
        auto decoded=ImageCodec().read(r.source);if(!decoded.ok)throw std::runtime_error(decoded.message);input.image=std::move(decoded.image);
    }
    if(fresh&&!blackSet&&r.prefix.empty()&&input.image.colorEncoding==ColorEncoding::Linear){
        double low=std::numeric_limits<double>::infinity(),high=-low;
        for(std::size_t p=0;p<input.image.pixelCount();++p)if(input.image.pixels[p*4+3]>1e-6)
            for(unsigned c=0;c<3;++c){const double v=input.image.pixels[p*4+c];low=std::min(low,v);high=std::max(high,v);}
        r.options.black=low;r.options.white=high;
    }
    verifySource(r);
    auto working=input.image;
    for(const auto& step:r.prefix)working=generalizedStretch(working,step).image;
    auto result=generalizedStretch(working,r.options);const auto json=report(r,input,result);
    if(!fs::create_directory(output))throw std::invalid_argument("Choose a new GHS output directory");
    try{
        if(!r.previewWidth){
            const auto saved=FitsCodec().write(result.image,output/"display.fits",input.sky?&input.sky->grid:nullptr,input.sky?&input.sky->distortion:nullptr,FitsOutputDomain::DisplaySRGB);
            if(!saved.ok)throw std::runtime_error(saved.message);writeImage(result.image,output/"developed.tiff");
        }
        const auto width=r.previewWidth?r.previewWidth:1200;
        writePreview(result.normalized,output/"before.tiff",width);writePreview(result.image,output/"after.tiff",width);
        std::ofstream out(output/"report.json");out<<json;out.close();if(!out)throw std::runtime_error("Cannot save GHS report");
        r.products.clear();for(const auto& name:names(r.previewWidth))r.products[name]=fileSHA256(output/name);
        saveRecipe(output/"recipe.txt",r);std::cout<<json;
    }catch(...){std::error_code ec;for(const auto& name:names(r.previewWidth))fs::remove(output/name,ec);fs::remove(output/"recipe.txt",ec);fs::remove(output,ec);throw;}
    return 0;
}
