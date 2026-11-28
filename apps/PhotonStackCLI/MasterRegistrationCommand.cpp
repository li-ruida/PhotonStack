#include "MasterRegistrationCommand.hpp"
#include "OutputPath.hpp"
#include "MasterRegistrationRecipe.hpp"
#include <iostream>
#include <algorithm>

int runMasterRegistration(const std::vector<std::string>& args){
    using namespace photonstack;using namespace photonstack::cli;namespace fs=std::filesystem;
    const bool fresh=args.at(0)=="channel-register",inspect=args[0]=="channel-registration-inspect";
    MasterRegistrationRecipe r;fs::path directory,recipePath;std::set<std::string> seen;
    for(std::size_t i=1;i<args.size();i+=2){
        if(i+1>=args.size())throw std::invalid_argument("Missing master registration option value");
        const auto& key=args[i];const auto& value=args[i+1];
        if(key!="--input"&&!seen.insert(key).second)throw std::invalid_argument("Duplicate registration option");
        if(key=="--output-dir"&&!inspect)directory=value;
        else if(key=="--recipe"&&!fresh)recipePath=value;
        else if(key=="--mode"&&fresh)r.mode=value;
        else if(key=="--reference"&&fresh)r.reference=value;
        else if(key=="--input"&&fresh){
            const auto split=value.find('=');
            if(split==std::string::npos||r.sources.size()>=16)throw std::invalid_argument("Specify 1..16 --input symbol=/path/master.fits");
            r.sources.push_back({value.substr(0,split),fs::canonical(value.substr(split+1)),{}});
        }else throw std::invalid_argument("Unknown master registration option: "+key);
    }
    if (!inspect) photonstack::cli::requireNewOutputPath(directory);
    if(!fresh){
        if(recipePath.empty())throw std::invalid_argument("Specify --recipe");
        r=readMasterRegistration(recipePath,inspect);
        if(inspect){printMasterRegistration(std::cout,r,recipePath.parent_path());return 0;}
    }else for(auto& s:r.sources)s.sha=fileSHA256(s.path);
    validateMasterRegistration(r,!fresh);verifyMasterSources(r);
    const auto ref=std::find_if(r.sources.begin(),r.sources.end(),[&](const auto& s){return s.name==r.reference;});
    const auto reference=readLinearMonoMaster(ref->path);
    const auto mode=r.mode=="stars"?MasterRegistrationMode::Stars:MasterRegistrationMode::Wcs;
    if(mode==MasterRegistrationMode::Wcs&&!reference.sky)throw std::invalid_argument("Reference master has no supported WCS");
    if(!fs::create_directory(directory))throw std::invalid_argument("Choose a new output directory");
    try{
        r.products.clear();
        for(const auto& source:r.sources){
            const bool isReference=source.name==r.reference;
            // Hold only the reference, current source, and current output in memory.
            const auto moving=isReference?LinearMonoMaster{}:readLinearMonoMaster(source.path);
            const auto output=registerMonoMaster(reference,isReference?reference:moving,mode,fresh?nullptr:&r.measurements.at(source.name),isReference);
            r.measurements[source.name]=output.measurement;
            for(const auto& [name,image]:std::array<std::pair<std::string,const ImageBuffer*>,2>{{{source.name+".fits",&output.image},{source.name+"-validity.fits",&output.validity}}}){
                const auto result=FitsCodec().write(*image,directory/name,reference.sky?&reference.sky->grid:nullptr,reference.sky?&reference.sky->distortion:nullptr);
                if(!result.ok)throw std::runtime_error(result.message);
                r.products[name]=fileSHA256(directory/name);
            }
        }
        verifyMasterSources(r);writeMasterRegistration(directory/"recipe.txt",r);
        printMasterRegistration(std::cout,r,directory);
    }catch(...){
        std::error_code ignored;
        for(const auto& source:r.sources){fs::remove(directory/(source.name+".fits"),ignored);fs::remove(directory/(source.name+"-validity.fits"),ignored);}
        fs::remove(directory/"recipe.txt",ignored);fs::remove(directory,ignored);throw;
    }
    return 0;
}
