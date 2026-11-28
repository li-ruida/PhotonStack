#include "photonstack/SerSequence.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace photonstack {
namespace {
void require(bool ok,const char* message){if(!ok)throw std::invalid_argument(message);}
std::uint32_t u32(const unsigned char* b){return std::uint32_t(b[0])|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24);}
std::uint64_t u64(const unsigned char* b){return std::uint64_t(u32(b))|(std::uint64_t(u32(b+4))<<32);}
std::string ascii(const unsigned char* p){std::string s;for(int i=0;i<40&&p[i];++i){require(p[i]>=32&&p[i]<=126,"SER metadata contains non-ASCII text");s+=char(p[i]);}return s;}
constexpr std::uint64_t maximumTicks=3155378975999999999ULL;
std::string quote(const std::string& s){std::string q="'";for(char c:s){q+=c;if(c=='\'')q+='\'';}q+='\'';return q;}
}
struct SerSequence::Impl {std::ifstream stream;SerSequenceInfo info;std::uint64_t size=0;};
SerSequence::SerSequence(const std::filesystem::path& path,SerByteOrder order):impl(std::make_unique<Impl>()){
    auto& s=*impl;s.stream.open(path,std::ios::binary);require(bool(s.stream),"Cannot open SER sequence");
    s.size=std::filesystem::file_size(path);require(s.size>=178,"Truncated SER header");
    std::array<unsigned char,178> h{};s.stream.read(reinterpret_cast<char*>(h.data()),h.size());
    require(bool(s.stream)&&std::string(reinterpret_cast<char*>(h.data()),14)=="LUCAM-RECORDER","Invalid SER signature");
    auto& i=s.info;i.colorID=u32(h.data()+18);i.endianFlag=u32(h.data()+22);i.width=u32(h.data()+26);i.height=u32(h.data()+30);i.depth=u32(h.data()+34);i.frames=u32(h.data()+38);
    require(i.endianFlag<=1&&i.width>0&&i.height>0&&i.frames>0&&i.frames<=10000000&&i.depth>=1&&i.depth<=16,"Invalid SER dimensions, count, depth or byte-order flag");
    require(i.width<=32768&&i.height<=32768&&std::uint64_t(i.width)*i.height<=32*1024*1024,"SER frame exceeds 32 megapixel limit");
    require(i.colorID==0||(i.colorID>=8&&i.colorID<=11)||i.colorID==100||i.colorID==101,"Unsupported SER CFA: only mono, RGB/BGR and RGGB/GRBG/GBRG/BGGR are supported");
    i.channels=i.colorID>=100?3:1;
    if(i.colorID>=8&&i.colorID<=11)i.bayer=std::array<std::string,4>{"RGGB","GRBG","GBRG","BGGR"}[i.colorID-8];
    i.littleEndian=order==SerByteOrder::Little||(order==SerByteOrder::Compatibility&&i.endianFlag==0)||(order==SerByteOrder::Specification&&i.endianFlag==1);
    i.observer=ascii(h.data()+42);i.instrument=ascii(h.data()+82);i.telescope=ascii(h.data()+122);
    i.localStartTicks=u64(h.data()+162);i.utcStartTicks=u64(h.data()+170);
    require(i.localStartTicks<=maximumTicks&&i.utcStartTicks<=maximumTicks,"Unsupported SER timestamp flags or date range");
    i.frameBytes=std::uint64_t(i.width)*i.height*i.channels*(i.depth<=8?1:2);
    require(i.frames<=(std::numeric_limits<std::uint64_t>::max()-178)/i.frameBytes,"SER byte count overflow");
    const auto end=178+i.frameBytes*i.frames;
    require(s.size==end||s.size==end+std::uint64_t(i.frames)*8,"Truncated SER frames/trailer or unexpected trailing bytes");
    i.timestamps=s.size!=end;
    require(!i.timestamps||i.localStartTicks>0,"SER timestamp trailer conflicts with absent local start time");
}
SerSequence::~SerSequence()=default;
const SerSequenceInfo& SerSequence::info()const{return impl->info;}
SerFrame SerSequence::read(std::uint32_t index){
    auto& s=*impl;const auto& i=s.info;require(index<i.frames,"SER frame index out of range");
    SerFrame frame;auto& im=frame.image;im.width=i.width;im.height=i.height;im.channels=i.channels;im.sourceBitsPerChannel=std::uint16_t(i.depth);
    im.format=i.channels==1?PixelFormat::Float32Gray:PixelFormat::Float32RGBA;im.colorEncoding=ColorEncoding::Linear;im.pixels.resize(im.sampleCount());
    s.stream.clear();s.stream.seekg(178+i.frameBytes*index);
    std::array<unsigned char,65536> buffer{};const unsigned bytes=i.depth<=8?1:2;
    std::size_t p=0;const unsigned mask=(1U<<i.depth)-1;
    while(p<im.sampleCount()){
        const auto samples=std::min(im.sampleCount()-p,buffer.size()/bytes);s.stream.read(reinterpret_cast<char*>(buffer.data()),samples*bytes);require(bool(s.stream),"SER source truncated during frame read");
        for(std::size_t j=0;j<samples;++j){
            unsigned v=buffer[j*bytes];if(bytes==2)v=i.littleEndian?v+(unsigned(buffer[j*bytes+1])<<8):(v<<8)+buffer[j*bytes+1];
            if(i.depth<8){const unsigned shift=8-i.depth;require((v&((1U<<shift)-1))==0,"SER sample contradicts declared MSB-aligned bit depth");v>>=shift;}
            require(v<=mask,"SER sample exceeds declared bit depth");
            const auto sample=p+j;const auto dest=i.colorID==101?(sample/3)*3+2-sample%3:sample;im.pixels[dest]=float(v);
        }p+=samples;
    }
    if(i.timestamps){std::array<unsigned char,8> t{};s.stream.clear();s.stream.seekg(178+i.frameBytes*i.frames+std::uint64_t(index)*8);s.stream.read(reinterpret_cast<char*>(t.data()),8);require(bool(s.stream),"Cannot read SER frame timestamp");
        const auto ticks=u64(t.data());require(ticks<=maximumTicks,"SER frame timestamp contains unsupported flags");if(ticks)frame.utcTicks=ticks;
    }
    return frame;
}
std::string serTimestampISO(std::uint64_t ticks){
    require(ticks>0&&ticks<=maximumTicks,"Invalid SER UTC timestamp");
    using namespace std::chrono;
    const auto secondsFromYearOne=ticks/10000000;const auto dayCount=secondsFromYearOne/86400;
    const auto date=year_month_day(sys_days(year(1)/January/1)+days(dayCount));
    const auto secondsOfDay=secondsFromYearOne%86400;std::ostringstream out;
    out<<std::setfill('0')<<std::setw(4)<<int(date.year())<<'-'<<std::setw(2)<<unsigned(date.month())<<'-'<<std::setw(2)<<unsigned(date.day())
       <<'T'<<std::setw(2)<<secondsOfDay/3600<<':'<<std::setw(2)<<(secondsOfDay%3600)/60<<':'<<std::setw(2)<<secondsOfDay%60<<'.'<<std::setw(7)<<ticks%10000000;
    return out.str();
}
void writeSerFrameFits(const SerFrame& frame,const SerSequenceInfo& info,std::uint32_t index,const std::filesystem::path& path){
    require(!std::filesystem::exists(path)&&!std::filesystem::is_symlink(path),"SER FITS output must be new");
    const auto& im=frame.image;require(im.width==info.width&&im.height==info.height&&im.channels==info.channels&&im.pixels.size()==im.sampleCount()&&index<info.frames,"Invalid SER export frame");
    std::string header;
    const auto card=[&](const std::string& key,const std::string& value){require(key.size()<=8&&value.size()<=68,"SER FITS metadata too long");std::string line=key;line.resize(8,' ');line+="= "+(value.starts_with('\'')?value:std::string(value.size()<20?20-value.size():0,' ')+value);line.resize(80,' ');header+=line;};
    card("SIMPLE","T");card("BITPIX","-32");card("NAXIS",im.channels==1?"2":"3");card("NAXIS1",std::to_string(im.width));card("NAXIS2",std::to_string(im.height));if(im.channels==3)card("NAXIS3","3");
    card("PSCOLOR",quote("Linear"));card("BUNIT",quote("ADU"));card("SERFRAME",std::to_string(index));card("SERDEPTH",std::to_string(info.depth));card("ROWORDER",quote("TOP-DOWN"));
    if(!info.bayer.empty()){card("BAYERPAT",quote(info.bayer));card("XBAYROFF","0");card("YBAYROFF","0");}
    if(!info.observer.empty())card("OBSERVER",quote(info.observer));if(!info.instrument.empty())card("INSTRUME",quote(info.instrument));if(!info.telescope.empty())card("TELESCOP",quote(info.telescope));
    if(frame.utcTicks){card("DATE-OBS",quote(serTimestampISO(*frame.utcTicks)));card("TIMESYS",quote("UTC"));}
    header+="END";header.resize(((header.size()+2879)/2880)*2880,' ');
    auto* file=std::fopen(path.c_str(),"wbx");require(file,"Cannot create SER FITS output");
    try{
        require(std::fwrite(header.data(),1,header.size(),file)==header.size(),"Cannot write SER FITS header");
        std::array<unsigned char,65536> bytes{};std::size_t used=0;
        const auto flush=[&](){require(std::fwrite(bytes.data(),1,used,file)==used,"Cannot write SER FITS pixels");used=0;};
        for(unsigned c=0;c<im.channels;++c)for(std::size_t p=0;p<im.pixelCount();++p){
            const auto bits=std::bit_cast<std::uint32_t>(im.pixels[p*im.channels+c]);
            for(int shift=24;shift>=0;shift-=8)bytes[used++]=static_cast<unsigned char>(bits>>shift);
            if(used==bytes.size())flush();
        }
        if(used)flush();const auto padding=(2880-(im.sampleCount()*4)%2880)%2880;bytes.fill(0);used=padding;flush();
        const auto status=std::fclose(file);file=nullptr;require(status==0,"Cannot close SER FITS output");
    }catch(...){if(file)std::fclose(file);std::error_code error;std::filesystem::remove(path,error);throw;}
}
}
