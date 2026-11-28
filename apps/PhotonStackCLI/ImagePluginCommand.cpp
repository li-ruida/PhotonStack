#include "ImagePluginCommand.hpp"
#include "ChannelRecipe.hpp"
#include "NewProductDirectory.hpp"
#include "LocalProcess.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/AstroDevelop.hpp"
#include <chrono>
#include <cmath>
#include <iostream>

namespace {
namespace fs=std::filesystem;using namespace photonstack;using namespace photonstack::cli;
void require(bool v,const std::string& message){if(!v)throw std::invalid_argument(message);}
struct Plugin {
    std::string id,name,sha,domain,grid,units,licenseSHA;
    fs::path executable,license;
    unsigned timeout=0;
    std::vector<std::string> arguments;
};
Plugin readPlugin(const fs::path& path){
    require(fs::is_regular_file(path)&&fs::file_size(path)<=65536,"Plugin definition must be a regular file at most 64 KiB");
    std::ifstream input(path);std::string header;std::getline(input,header);require(header=="PhotonStack-ImagePlugin-1","Unsupported image plugin definition version");
    Plugin p;std::set<std::string> seen;std::string line;
    while(std::getline(input,line)){
        if(line.empty())continue;std::istringstream in(line);std::string key,value;require(bool(in>>key),"Invalid plugin record");
        if(key=="argument"){require(bool(in>>std::quoted(value))&&p.arguments.size()<64,"Invalid plugin argument");p.arguments.push_back(value);}
        else {require(seen.insert(key).second&&bool(in>>std::quoted(value)),"Duplicate or incomplete plugin record");
            if(key=="id")p.id=value;else if(key=="name")p.name=value;else if(key=="executable")p.executable=value;else if(key=="sha256")p.sha=value;
            else if(key=="domain")p.domain=value;else if(key=="grid")p.grid=value;else if(key=="units")p.units=value;
            else if(key=="license-file")p.license=value;else if(key=="accepted-license-sha256")p.licenseSHA=value;
            else if(key=="timeout"){std::size_t n=0;auto v=std::stoul(value,&n);require(n==value.size()&&v>=1&&v<=600,"Plugin timeout must be 1..600 seconds");p.timeout=unsigned(v);}
            else throw std::invalid_argument("Unknown plugin record: "+key);
        }
        std::string extra;require(!(in>>extra)&&value.size()<4096&&value.find('\0')==value.npos,"Invalid trailing plugin data");
    }
    require(!p.id.empty()&&p.id.size()<=100&&p.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_")==p.id.npos&&!p.name.empty()&&p.name.size()<=100,"Invalid plugin identity");
    require(p.domain=="linear-fits"&&p.grid=="preserve"&&p.units=="preserve"&&p.timeout,"Plugin v1 requires linear-fits, grid preserve, units preserve, and a timeout");
    require(p.executable.is_absolute()&&fs::is_regular_file(p.executable)&&channelSHA(p.sha)&&fileSHA256(p.executable)==p.sha,"Plugin executable is missing or its SHA-256 changed");
    require(std::count(p.arguments.begin(),p.arguments.end(),"{input}")==1&&std::count(p.arguments.begin(),p.arguments.end(),"{output}")==1,"Plugin needs one complete {input} and {output} argument");
    require(p.license.empty()==p.licenseSHA.empty(),"Plugin license declaration is incomplete");
    if(!p.license.empty())require(p.license.is_absolute()&&fs::is_regular_file(p.license)&&fs::file_size(p.license)<=131072&&channelSHA(p.licenseSHA)&&fileSHA256(p.license)==p.licenseSHA,"Plugin license changed; review the full terms before accepting this version");
    // New StarNet's CLI has an explicit per-user acceptance requirement.
    auto identity=p.name+" "+p.executable.filename().string();std::transform(identity.begin(),identity.end(),identity.begin(),[](unsigned char c){return char(std::tolower(c));});
    require(identity.find("starnet")==identity.npos||!p.license.empty(),"StarNet requires a reviewed license and explicit accepted-license-sha256 record");
    return p;
}
void describe(std::ostream& out,const Plugin& p){out<<"\"id\":"<<channelJSON(p.id)<<",\"name\":"<<channelJSON(p.name)<<",\"executable\":"<<channelJSON(p.executable.string())<<",\"executableSHA256\":"<<channelJSON(p.sha)<<",\"domain\":"<<channelJSON(p.domain)<<",\"grid\":"<<channelJSON(p.grid)<<",\"units\":"<<channelJSON(p.units)<<",\"timeout\":"<<p.timeout<<",\"arguments\":[";for(std::size_t n=0;n<p.arguments.size();++n){if(n)out<<',';out<<channelJSON(p.arguments[n]);}out<<']';}
void save(const fs::path& path,const std::string& text){std::ofstream out(path);out<<text;out.close();require(bool(out),"Cannot save plugin record");}
void validateResult(const fs::path& source,const fs::path& result,const LinearScienceImage& original){
    require(fs::is_regular_file(result)&&!fs::is_symlink(result),"Plugin must produce a regular FITS file");auto output=readLinearScienceImage(result);
    const auto sourceHeader=readFitsPrimaryValues(source),resultHeader=readFitsPrimaryValues(result);
    const auto value=[](const auto& header,const std::string& key){auto i=header.find(key);return i==header.end()?std::string():i->second;};
    require(value(sourceHeader,"BUNIT")==value(resultHeader,"BUNIT"),"Plugin changed or dropped sample units");
    const auto a=FitsCodec().inspect(source),b=FitsCodec().inspect(result);
    require(a.ok&&b.ok&&a.metadata.channels==b.metadata.channels&&original.image.width==output.image.width&&original.image.height==output.image.height,"Plugin changed image geometry or channels");
    require(bool(original.sky)==bool(output.sky),"Plugin must preserve the source WCS presence");
    if(original.sky){const SkyProjection projection(*output.sky,original.sky->grid),reference(*original.sky,original.sky->grid);
        for(unsigned y=0;y<=8;++y)for(unsigned x=0;x<=8;++x){const double px=x*(original.image.width-1.)/8,py=y*(original.image.height-1.)/8;const auto p=projection.toCanvas(px,py),q=reference.toCanvas(px,py);require(std::hypot(p.x-q.x,p.y-q.y)<1e-4,"Plugin changed the source WCS grid");}}
    for(std::size_t pixel=0;pixel<original.image.pixelCount();++pixel){const auto a=original.image.pixels[pixel*4+3],b=output.image.pixels[pixel*4+3];require(a==b,"Plugin changed valid coverage or introduced invalid pixels");}
    // Pixel values can change; identical units and physical alignment remain
    // author-declared contracts, not facts provable from image dimensions.
}
}
int runImagePlugin(const std::vector<std::string>& args){
    fs::path definition,source,destination;std::string expectedDefinitionSHA;std::set<std::string> seen;const bool inspect=args[0]=="plugin-inspect";
    for(std::size_t n=1;n<args.size();n+=2){require(n+1<args.size()&&seen.insert(args[n]).second,"Missing or duplicate plugin option");const auto& key=args[n];
        if(key=="--definition")definition=fs::canonical(args[n+1]);else if(!inspect&&key=="--definition-sha256")expectedDefinitionSHA=args[n+1];else if(!inspect&&key=="--input")source=fs::canonical(args[n+1]);else if(!inspect&&key=="--output-dir")destination=args[n+1];else throw std::invalid_argument("Unknown plugin option: "+key);}
    require(!definition.empty(),"Choose an image plugin definition");const auto definitionSHA=fileSHA256(definition);const auto plugin=readPlugin(definition);
    require(expectedDefinitionSHA.empty()||expectedDefinitionSHA==definitionSHA,"Plugin definition changed since review");
    if(inspect){std::cout<<"{\"type\":\"complete\",\"command\":\"plugin-inspect\",\"definitionSHA256\":"<<channelJSON(definitionSHA)<<',';describe(std::cout,plugin);std::cout<<"}\n";return 0;}
    require(!source.empty()&&!destination.empty(),"Plugin run needs a linear FITS source and a new output directory");const auto sourceSHA=fileSHA256(source);const auto image=readLinearScienceImage(source);
    NewProductDirectory product(destination);fs::copy_file(source,product.stage/"input.fits");fs::copy_file(definition,product.stage/"definition.psplugin");
    require(fileSHA256(product.stage/"input.fits")==sourceSHA&&fileSHA256(product.stage/"definition.psplugin")==definitionSHA,"Plugin inputs changed during preparation");
    std::vector<std::string> expanded;for(const auto& argument:plugin.arguments)expanded.push_back(argument=="{input}"?(product.stage/"input.fits").string():argument=="{output}"?(product.stage/"result.fits").string():argument);
    const auto start=std::chrono::steady_clock::now();
    try{runLocalProcess(plugin.executable,expanded,product.stage,plugin.timeout,"plugin.log","Image plugin");}
    catch(...){std::ifstream log(product.stage/"plugin.log");if(log){log.seekg(0,std::ios::end);auto size=log.tellg();log.seekg(std::max<std::streamoff>(0,std::streamoff(size)-8192));std::cerr<<log.rdbuf();}throw;}
    require(fileSHA256(product.stage/"input.fits")==sourceSHA&&fileSHA256(source)==sourceSHA,"Plugin changed its input or the original source");
    require(fileSHA256(definition)==definitionSHA&&fileSHA256(plugin.executable)==plugin.sha,"Plugin definition or executable changed during execution");
    for(const auto& entry:fs::directory_iterator(product.stage)) {
        const auto name=entry.path().filename().string();
        require((name=="input.fits"||name=="result.fits"||name=="definition.psplugin"||name=="plugin.log")&&fs::is_regular_file(entry.path())&&!fs::is_symlink(entry.path()),"Plugin created an undeclared output file");
    }
    validateResult(product.stage/"input.fits",product.stage/"result.fits",image);
    const auto duration=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::ostringstream record;record<<std::setprecision(17)<<"{\"version\":1,\"source\":"<<channelJSON(source.string())<<",\"sourceSHA256\":"<<channelJSON(sourceSHA)<<",\"definitionSHA256\":"<<channelJSON(definitionSHA)<<",\"resultSHA256\":"<<channelJSON(fileSHA256(product.stage/"result.fits"))<<",\"seconds\":"<<duration<<",\"sameGridAndUnitsDeclared\":true,\"coveragePreserved\":true,\"wcsPreserved\":true,";describe(record,plugin);record<<"}\n";save(product.stage/"plugin.json",record.str());product.publish();
    std::cout<<"{\"type\":\"complete\",\"command\":\"plugin-run\",\"directory\":"<<channelJSON(fs::absolute(destination).string())<<",\"result\":\"result.fits\"}\n";return 0;
}
