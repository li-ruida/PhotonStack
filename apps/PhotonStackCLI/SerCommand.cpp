#include "SerCommand.hpp"
#include "ChannelRecipe.hpp"
#include "NewProductDirectory.hpp"
#include "photonstack/SerSequence.hpp"
#include <charconv>
#include <iostream>
#include <limits>

namespace {
using namespace photonstack;using namespace photonstack::cli;namespace fs=std::filesystem;
unsigned integer(const std::string& s){unsigned v=0;const auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),v);if(e!=std::errc()||p!=s.data()+s.size())throw std::invalid_argument("SER frame range must use unsigned integers");return v;}
void info(std::ostream& out,const SerSequenceInfo& i){
    out<<"\"width\":"<<i.width<<",\"height\":"<<i.height<<",\"frames\":"<<i.frames<<",\"channels\":"<<i.channels<<",\"colorID\":"<<i.colorID<<",\"depth\":"<<i.depth
       <<",\"bayer\":"<<channelJSON(i.bayer)<<",\"littleEndian\":"<<(i.littleEndian?"true":"false")<<",\"endianFlag\":"<<i.endianFlag
       <<",\"timestamps\":"<<(i.timestamps?"true":"false")<<",\"frameBytes\":"<<i.frameBytes
       <<",\"observer\":"<<channelJSON(i.observer)<<",\"instrument\":"<<channelJSON(i.instrument)<<",\"telescope\":"<<channelJSON(i.telescope)
       <<",\"localStartTicks\":"<<channelJSON(std::to_string(i.localStartTicks))<<",\"utcStartTicks\":"<<channelJSON(std::to_string(i.utcStartTicks));
}
}
int runSer(const std::vector<std::string>& args){
    fs::path input,output;std::string byteOrder="compatibility";unsigned first=0,count=0,step=1;bool countSpecified=false;
    const bool inspect=args[0]=="ser-inspect";std::set<std::string> seen;
    for(std::size_t n=1;n<args.size();n+=2){if(n+1>=args.size()||!seen.insert(args[n]).second)throw std::invalid_argument("Missing or duplicate SER option");
        const auto& key=args[n];const auto& v=args[n+1];
        if(key=="--input")input=fs::canonical(v);else if(key=="--byte-order")byteOrder=v;
        else if(!inspect&&key=="--output-dir")output=v;else if(!inspect&&key=="--first")first=integer(v);
        else if(!inspect&&key=="--count"){count=integer(v);countSpecified=true;}else if(!inspect&&key=="--step")step=integer(v);
        else throw std::invalid_argument("Unknown SER option: "+key);
    }
    if(input.empty())throw std::invalid_argument("Choose a SER input");
    const std::map<std::string,SerByteOrder> orders{{"compatibility",SerByteOrder::Compatibility},{"specification",SerByteOrder::Specification},{"little",SerByteOrder::Little},{"big",SerByteOrder::Big}};
    if(!orders.contains(byteOrder))throw std::invalid_argument("SER byte order must be compatibility, specification, little or big");
    const auto sha=inspect?std::string():fileSHA256(input);
    SerSequence sequence(input,orders.at(byteOrder));const auto& i=sequence.info();
    if(inspect){std::cout<<"{\"type\":\"complete\",\"command\":\"ser-inspect\",\"source\":"<<channelJSON(input.string())<<",\"byteOrder\":"<<channelJSON(byteOrder)<<',';info(std::cout,i);std::cout<<"}\n";return 0;}
    if(output.empty()||!step||first>=i.frames)throw std::invalid_argument("Choose a new output directory and valid SER first/step");
    if(!countSpecified)count=1+(i.frames-1-first)/step;
    if(!count||count>10000||std::uint64_t(first)+std::uint64_t(count-1)*step>=i.frames)throw std::invalid_argument("SER extraction requires 1–10000 frames inside the source range");
    const auto outputBytes=std::uint64_t(count)*(5760+((std::uint64_t(i.width)*i.height*i.channels*4+2879)/2880)*2880);
    if(fs::space(fs::absolute(output).parent_path()).available<outputBytes)throw std::runtime_error("Insufficient disk space for selected Float32 FITS frames");
    NewProductDirectory product(output);
    std::ofstream manifest(product.stage/"sequence.json");manifest<<"{\"version\":1,\"source\":"<<channelJSON(input.string())<<",\"sourceSHA256\":"<<channelJSON(sha)<<",\"byteOrder\":"<<channelJSON(byteOrder)<<',';info(manifest,i);
    manifest<<",\"selected\":[";std::size_t missing=0;bool monotonic=true;std::optional<std::uint64_t> previous;
    for(unsigned n=0;n<count;++n){const unsigned index=first+n*step;const auto frame=sequence.read(index);
        std::ostringstream name;name<<"frame-"<<std::setfill('0')<<std::setw(8)<<index<<".fits";const auto path=product.stage/name.str();
        writeSerFrameFits(frame,i,index,path);
        if(n)manifest<<',';manifest<<"{\"index\":"<<index<<",\"file\":"<<channelJSON(name.str())<<",\"sha256\":"<<channelJSON(fileSHA256(path))<<",\"utcTicks\":";
        if(frame.utcTicks){manifest<<channelJSON(std::to_string(*frame.utcTicks));if(previous&&*frame.utcTicks<=*previous)monotonic=false;previous=frame.utcTicks;}
        else {manifest<<"null";++missing;}
        manifest<<'}';
        std::cerr<<"SER frame "<<(n+1)<<" / "<<count<<'\n';
    }
    if(fileSHA256(input)!=sha)throw std::runtime_error("SER source changed during extraction");
    manifest<<"],\"missingTimestamps\":"<<missing<<",\"strictlyIncreasingKnownTimestamps\":"<<(monotonic?"true":"false")<<"}\n";
    manifest.close();if(!manifest)throw std::runtime_error("Cannot save SER provenance");product.publish();
    std::cout<<"{\"type\":\"complete\",\"command\":\"ser-extract\",\"directory\":"<<channelJSON(fs::absolute(output).string())<<",\"sourceSHA256\":"<<channelJSON(sha)<<",\"extracted\":"<<count
             <<",\"missingTimestamps\":"<<missing<<",\"strictlyIncreasingKnownTimestamps\":"<<(monotonic?"true":"false")<<"}\n";return 0;
}
