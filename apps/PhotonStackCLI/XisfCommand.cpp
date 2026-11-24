#include "XisfCommand.hpp"
#include "ChannelRecipe.hpp"
#include "NewProductDirectory.hpp"
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <zlib.h>
#include <charconv>
#include <cstring>
#include <iostream>
#include <memory>
#ifdef PHOTONSTACK_XISF_LZ4
#include <lz4.h>
#endif
#ifdef PHOTONSTACK_XISF_ZSTD
#include <zstd.h>
#endif
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif

namespace {
namespace fs=std::filesystem;
using namespace photonstack;using namespace photonstack::cli;
using Bytes=std::vector<unsigned char>;
constexpr std::size_t maxBlock=512*1024*1024, maxXML=4*1024*1024;
struct Keyword {std::string name,value,comment;};
struct Raster {std::size_t width=0,height=0,channels=0,bytes=0;std::string format,color,cfa,xml;Bytes pixels;std::vector<Keyword> keywords;};
void require(bool condition,const std::string& message){if(!condition)throw std::invalid_argument(message);}
std::string trim(std::string s){const auto a=s.find_first_not_of(" \t\r\n");if(a==s.npos)return {};return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);}
std::size_t number(const std::string& s){std::size_t n=0;const auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),n);require(!s.empty()&&e==std::errc()&&p==s.data()+s.size(),"Invalid XISF unsigned number");return n;}
std::vector<std::string> split(const std::string& s,char delimiter){std::vector<std::string> v;std::size_t start=0;for(;;){auto end=s.find(delimiter,start);v.push_back(s.substr(start,end-start));if(end==s.npos)return v;start=end+1;}}
Bytes read(std::ifstream& in,std::size_t offset,std::size_t count){require(count<=maxBlock,"XISF block exceeds 512 MiB limit");Bytes b(count);in.seekg(offset);in.read(reinterpret_cast<char*>(b.data()),count);require(bool(in),"Truncated image data");return b;}
void write(const fs::path& path,const Bytes& b){std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(b.data()),b.size());out.close();require(bool(out),"Cannot write image product");}
std::string attribute(xmlNode* node,const char* key,std::string fallback={}){xmlChar* p=xmlGetProp(node,BAD_CAST key);if(!p)return fallback;std::string value(reinterpret_cast<char*>(p));xmlFree(p);return value;}
bool tag(xmlNode* node,const char* name){return node->type==XML_ELEMENT_NODE&&xmlStrEqual(node->name,BAD_CAST name);}
std::string digest(const Bytes& bytes,std::string algorithm){
    if(algorithm=="sha-1")algorithm="sha1";else if(algorithm=="sha-256")algorithm="sha256";else if(algorithm=="sha-512")algorithm="sha512";
    unsigned char data[64]{};unsigned length=0;
#ifdef __APPLE__
    if(algorithm=="sha1"){CC_SHA1(bytes.data(),CC_LONG(bytes.size()),data);length=20;}
    else if(algorithm=="sha256"){CC_SHA256(bytes.data(),CC_LONG(bytes.size()),data);length=32;}
    else if(algorithm=="sha512"){CC_SHA512(bytes.data(),CC_LONG(bytes.size()),data);length=64;}
    else throw std::invalid_argument("Unsupported XISF checksum algorithm");
#else
    const EVP_MD* md=algorithm=="sha1"?EVP_sha1():algorithm=="sha256"?EVP_sha256():algorithm=="sha512"?EVP_sha512():nullptr;
    require(md&&EVP_Digest(bytes.data(),bytes.size(),data,&length,md,nullptr),"Unsupported XISF checksum algorithm");
#endif
    std::ostringstream out;for(unsigned i=0;i<length;++i)out<<std::hex<<std::setfill('0')<<std::setw(2)<<unsigned(data[i]);return out.str();
}
void dimensions(Raster& r){
    static const std::map<std::string,std::size_t> sizes{{"UInt8",1},{"UInt16",2},{"UInt32",4},{"Float32",4},{"Float64",8}};
    require(sizes.contains(r.format),"XISF supports UInt8/16/32 and Float32/64 samples");r.bytes=sizes.at(r.format);
    require(r.width&&r.width<=65536&&r.height&&r.height<=65536&&(r.channels==1||r.channels==3||r.channels==4),"Unsupported XISF image geometry");
    require(r.width*r.height<=maxBlock/(r.channels*r.bytes),"Image exceeds 512 MiB decoded limit");
    require((r.channels==1&&r.color=="Gray")||(r.channels>=3&&r.color=="RGB"),"XISF requires Gray or RGB color space");
}
void checkCfa(Raster& r){if(r.cfa.empty())return;require(r.channels==1&&(r.cfa=="RGGB"||r.cfa=="GRBG"||r.cfa=="GBRG"||r.cfa=="BGGR"),"Only 2x2 RGB Bayer CFA is supported");}
std::string unquote(const std::string& s){if(s.size()<2||s.front()!='\''||s.back()!='\'')return s;std::string out;for(std::size_t i=1;i+1<s.size();++i){out+=s[i];if(s[i]=='\''&&i+2<s.size()&&s[i+1]=='\'')++i;}return trim(out);}
bool structural(const std::string& name){return name=="SIMPLE"||name=="BITPIX"||name.starts_with("NAXIS")||name=="EXTEND"||name=="BSCALE"||name=="BZERO"||name=="BLANK"||name=="CHECKSUM"||name=="DATASUM"||name=="END"||name=="PCOUNT"||name=="GCOUNT";}
void validateKeywords(const Raster& r){std::set<std::string> names;for(const auto& k:r.keywords){
    require(k.name.size()>0&&k.name.size()<=8&&k.name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")==k.name.npos,"Unsupported FITS keyword name in XISF");
    require(k.value.find_first_of("\r\n")==k.value.npos&&k.comment.find_first_of("\r\n")==k.comment.npos,"Invalid FITS keyword characters");
    require(k.name=="HISTORY"||k.name=="COMMENT"||names.insert(k.name).second,"Duplicate FITS keyword in XISF");
    if(k.name=="ROWORDER")require(unquote(k.value)=="TOP-DOWN","Bottom-up image metadata needs explicit orientation conversion");
    if(k.name=="BAYERPAT"&&!r.cfa.empty())require(unquote(k.value)==r.cfa,"XISF CFA and FITS Bayer pattern disagree");
    if((k.name=="XBAYROFF"||k.name=="YBAYROFF")&&!r.cfa.empty())require(k.value=="0","XISF CFA conflicts with nonzero FITS Bayer offset");
}}
Bytes decompress(xmlNode* image,const Bytes& encoded,std::size_t expected){
    const auto text=attribute(image,"compression");if(text.empty()){require(attribute(image,"subblocks").empty(),"Subblocks require compression");require(encoded.size()==expected,"XISF block length mismatch");return encoded;}
    const auto parts=split(text,':');require(parts.size()==2||parts.size()==3,"Invalid XISF compression");require(number(parts[1])==expected,"XISF uncompressed size mismatch");
    auto codec=parts[0];const bool shuffled=codec.ends_with("+sh");if(shuffled)codec.resize(codec.size()-3);
    require(parts.size()==(shuffled?3:2),"Invalid XISF shuffle declaration");const auto item=shuffled?number(parts[2]):1;
    require(item>0&&item<=16&&(!shuffled||item>=2)&&expected%item==0,"Invalid XISF shuffle size");
    std::vector<std::pair<std::size_t,std::size_t>> blocks;
    auto sub=attribute(image,"subblocks");if(sub.empty())blocks.emplace_back(encoded.size(),expected);
    else {for(const auto& entry:split(sub,':')){auto sizes=split(entry,',');require(sizes.size()==2&&blocks.size()<65536,"Invalid XISF subblocks");blocks.emplace_back(number(sizes[0]),number(sizes[1]));}}
    Bytes decoded(expected);std::size_t src=0,dst=0;
    for(const auto [compressed,raw]:blocks){require(compressed&&raw&&compressed<=encoded.size()-src&&raw<=expected-dst,"XISF subblock outside image");
        if(codec=="zlib"){
            z_stream stream{};require(inflateInit(&stream)==Z_OK,"Cannot initialize zlib");stream.next_in=const_cast<Bytef*>(encoded.data()+src);stream.avail_in=uInt(compressed);stream.next_out=decoded.data()+dst;stream.avail_out=uInt(raw);
            const auto status=inflate(&stream,Z_FINISH);const bool valid=status==Z_STREAM_END&&stream.total_in==compressed&&stream.total_out==raw;inflateEnd(&stream);require(valid,"Invalid zlib XISF block");
        }
#ifdef PHOTONSTACK_XISF_LZ4
        else if(codec=="lz4"||codec=="lz4hc")require(LZ4_decompress_safe(reinterpret_cast<const char*>(encoded.data()+src),reinterpret_cast<char*>(decoded.data()+dst),int(compressed),int(raw))==int(raw),"Invalid LZ4 XISF block");
#endif
#ifdef PHOTONSTACK_XISF_ZSTD
        else if(codec=="zstd"){require(ZSTD_findFrameCompressedSize(encoded.data()+src,compressed)==compressed,"Invalid Zstandard XISF frame size");require(ZSTD_decompress(decoded.data()+dst,raw,encoded.data()+src,compressed)==raw,"Invalid Zstandard XISF block");}
#endif
        else throw std::invalid_argument("XISF compression is unsupported by this build: "+codec);
        src+=compressed;dst+=raw;
    }
    require(src==encoded.size()&&dst==expected,"XISF subblocks do not cover image");
    if(shuffled){Bytes unshuffled(expected);const auto count=expected/item;for(std::size_t k=0;k<item;++k)for(std::size_t i=0;i<count;++i)unshuffled[i*item+k]=decoded[k*count+i];return unshuffled;}return decoded;
}
Raster readXisf(const fs::path& path,std::size_t index){
    std::ifstream in(path,std::ios::binary);auto prefix=read(in,0,16);require(std::memcmp(prefix.data(),"XISF0100",8)==0,"Not a monolithic XISF 1.0 file");
    const std::size_t length=std::size_t(prefix[8])|(std::size_t(prefix[9])<<8)|(std::size_t(prefix[10])<<16)|(std::size_t(prefix[11])<<24);
    require(length&&length<=maxXML,"XISF XML header exceeds 4 MiB limit");require(std::all_of(prefix.begin()+12,prefix.end(),[](auto c){return c==0;}),"Unsupported XISF reserved flags");
    auto xmlBytes=read(in,16,length);Raster r;r.xml.assign(xmlBytes.begin(),xmlBytes.end());
    require(r.xml.find('\0')==std::string::npos&&r.xml.find("<!DOCTYPE")==std::string::npos&&r.xml.find("<!ENTITY")==std::string::npos,"XISF external entities and DTDs are not supported");
    std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)> doc(xmlReadMemory(r.xml.data(),int(r.xml.size()),nullptr,"UTF-8",XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING),xmlFreeDoc);
    require(bool(doc)&&!doc->intSubset&&!doc->extSubset,"Invalid XISF XML");auto* root=xmlDocGetRootElement(doc.get());require(root&&tag(root,"xisf")&&attribute(root,"version")=="1.0","Unsupported XISF XML version");
    std::vector<xmlNode*> images;for(auto* n=root->children;n;n=n->next)if(tag(n,"Image"))images.push_back(n);
    require(index<images.size()&&images.size()<=1024,"XISF image index outside file");auto* image=images[index];auto geometry=split(attribute(image,"geometry"),':');require(geometry.size()==3,"Only 2D XISF images are supported");
    r.width=number(geometry[0]);r.height=number(geometry[1]);r.channels=number(geometry[2]);r.format=attribute(image,"sampleFormat");r.color=attribute(image,"colorSpace","Gray");dimensions(r);
    require(attribute(image,"orientation","0")=="0","Rotated or flipped XISF orientation needs explicit conversion");
    const auto location=split(attribute(image,"location"),':');require(location.size()==3&&location[0]=="attachment","Only attached monolithic XISF pixel blocks are supported");
    const auto offset=number(location[1]),stored=number(location[2]),size=fs::file_size(path);
    require(offset>=16+length&&offset<=size&&stored<=size-offset,"XISF pixel block outside file");auto encoded=read(in,offset,stored);
    auto checksum=attribute(image,"checksum");if(!checksum.empty()){auto fields=split(checksum,':');require(fields.size()==2&&digest(encoded,fields[0])==fields[1],"XISF pixel checksum mismatch");}
    r.pixels=decompress(image,encoded,r.width*r.height*r.channels*r.bytes);
    const auto endian=attribute(image,"byteOrder","little");require(endian=="little"||endian=="big","Invalid XISF byte order");
    if(endian=="big"&&r.bytes>1)for(std::size_t p=0;p<r.pixels.size();p+=r.bytes)std::reverse(r.pixels.begin()+p,r.pixels.begin()+p+r.bytes);
    auto storage=attribute(image,"pixelStorage","Planar");
    if(storage=="Planar")storage="planar";else if(storage=="Normal")storage="normal";require(storage=="planar"||storage=="normal","Unsupported XISF pixel storage");
    if(storage=="normal"&&r.channels>1){Bytes planar(r.pixels.size());const auto plane=r.width*r.height;for(std::size_t p=0;p<plane;++p)for(std::size_t c=0;c<r.channels;++c)std::memcpy(planar.data()+(c*plane+p)*r.bytes,r.pixels.data()+(p*r.channels+c)*r.bytes,r.bytes);r.pixels.swap(planar);}
    for(auto* node=image->children;node;node=node->next){
        if(tag(node,"FITSKeyword"))r.keywords.push_back({attribute(node,"name"),attribute(node,"value"),attribute(node,"comment")});
        if(tag(node,"ColorFilterArray")){require(r.cfa.empty()&&attribute(node,"width")=="2"&&attribute(node,"height")=="2","Unsupported or duplicate XISF CFA");r.cfa=attribute(node,"pattern");require(!r.cfa.empty(),"Empty XISF CFA");}
    }
    checkCfa(r);validateKeywords(r);return r;
}
std::string quote(const std::string& s){std::string out="'";for(char c:s){out+=c;if(c=='\'')out+=c;}return out+"'";}
std::string card(const Keyword& k){
    require(k.name.size()<=8,"FITS keyword too long");std::string result=k.name;result.resize(8,' ');
    if(k.name=="HISTORY"||k.name=="COMMENT")result+="  "+k.value;
    else {result+="= ";if(!k.value.starts_with("'")&&k.value.size()<20)result+=std::string(20-k.value.size(),' ');result+=k.value;}
    require(result.size()<=80,"FITS value exceeds one card; retained XISF XML can be used by other readers");
    if(!k.comment.empty()&&result.size()+3<80)result+=" / "+k.comment.substr(0,80-result.size()-3);
    require(std::all_of(result.begin(),result.end(),[](unsigned char c){return c>=32&&c<=126;}),"FITS header requires ASCII; source metadata is retained in XML");result.resize(80,' ');return result;
}
void writeFits(const fs::path& path,const Raster& r){
    int bitpix=r.format=="UInt8"?8:r.format=="UInt16"?16:r.format=="UInt32"?32:r.format=="Float32"?-32:-64;
    std::vector<Keyword> header{{"SIMPLE","T",{}},{"BITPIX",std::to_string(bitpix),{}},{"NAXIS",r.channels==1?"2":"3",{}},{"NAXIS1",std::to_string(r.width),{}},{"NAXIS2",std::to_string(r.height),{}}};
    if(r.channels>1)header.push_back({"NAXIS3",std::to_string(r.channels),{}});
    if(bitpix==16||bitpix==32){header.push_back({"BSCALE","1",{}});header.push_back({"BZERO",bitpix==16?"32768":"2147483648",{}});}
    std::set<std::string> names;for(const auto& k:r.keywords)if(!structural(k.name)){header.push_back(k);names.insert(k.name);}
    if(!names.contains("ROWORDER"))header.push_back({"ROWORDER",quote("TOP-DOWN"),{}});
    if(!r.cfa.empty()&&!names.contains("BAYERPAT"))header.push_back({"BAYERPAT",quote(r.cfa),{}});
    std::string text;for(const auto& k:header)text+=card(k);text+="END";text.resize(((text.size()+2879)/2880)*2880,' ');
    std::ofstream out(path,std::ios::binary);out.write(text.data(),text.size());
    // Repack samples without floating point conversion: preserve UInt32 and Float64 exactly.
    Bytes chunk(1024*1024);std::size_t used=0;
    for(std::size_t p=0;p<r.pixels.size();p+=r.bytes){for(std::size_t b=0;b<r.bytes;++b)chunk[used+b]=r.pixels[p+r.bytes-1-b];if(bitpix==16||bitpix==32)chunk[used]^=0x80;used+=r.bytes;if(used==chunk.size()){out.write(reinterpret_cast<char*>(chunk.data()),used);used=0;}}
    if(used)out.write(reinterpret_cast<char*>(chunk.data()),used);const auto padding=(2880-r.pixels.size()%2880)%2880;std::string zeros(padding,'\0');out.write(zeros.data(),zeros.size());out.close();require(bool(out),"Cannot save converted FITS");
}
Raster readFits(const fs::path& path){
    std::ifstream in(path,std::ios::binary);Raster r;std::map<std::string,std::string> values;bool ended=false;std::size_t offset=0;
    while(!ended){require(offset<maxXML,"FITS header exceeds 4 MiB");const auto block=read(in,offset,2880);offset+=2880;
        for(std::size_t i=0;i<2880;i+=80){const std::string text(block.begin()+i,block.begin()+i+80);auto name=trim(text.substr(0,8));if(name=="END"){ended=true;break;}if(name.empty())continue;
            if(text[8]!='='){if(name=="COMMENT"||name=="HISTORY")r.keywords.push_back({name,trim(text.substr(10)),{}});continue;}
            bool quoted=false;std::size_t slash=80;for(std::size_t j=10;j<80;++j){if(text[j]=='\''){if(quoted&&j+1<80&&text[j+1]=='\''){++j;continue;}quoted=!quoted;}if(text[j]=='/'&&!quoted){slash=j;break;}}
            auto value=trim(text.substr(10,slash-10));require(values.emplace(name,value).second,"Duplicate FITS header keyword");r.keywords.push_back({name,value,slash<80?trim(text.substr(slash+1)):""});
        }
    }
    require(values["SIMPLE"]=="T"&&(values["NAXIS"]=="2"||values["NAXIS"]=="3"),"Only primary 2D FITS images are supported");
    r.width=number(values["NAXIS1"]);r.height=number(values["NAXIS2"]);r.channels=values["NAXIS"]=="2"?1:number(values["NAXIS3"]);r.color=r.channels==1?"Gray":"RGB";
    const auto bitpix=values["BITPIX"],scale=values.contains("BSCALE")?values["BSCALE"]:"1",zero=values.contains("BZERO")?values["BZERO"]:"0";
    auto numeric=[](const std::string& s,double wanted){try {std::size_t n=0;const auto v=std::stod(s,&n);return n==s.size()&&v==wanted;}catch(...){return false;}};
    require(numeric(scale,1),"FITS BSCALE must be 1 for lossless XISF exchange");require(!values.contains("BLANK"),"Integer FITS BLANK needs explicit floating point conversion before XISF export");
    if(bitpix=="8"&&numeric(zero,0))r.format="UInt8";
    else if(bitpix=="16"&&numeric(zero,32768))r.format="UInt16";
    else if(bitpix=="32"&&numeric(zero,2147483648.))r.format="UInt32";
    else if(bitpix=="-32"&&numeric(zero,0))r.format="Float32";
    else if(bitpix=="-64"&&numeric(zero,0))r.format="Float64";
    else throw std::invalid_argument("FITS sample type/scaling requires explicit conversion before lossless XISF export");
    dimensions(r);r.pixels=read(in,offset,r.width*r.height*r.channels*r.bytes);
    if(r.format=="UInt16"||r.format=="UInt32")for(std::size_t p=0;p<r.pixels.size();p+=r.bytes)r.pixels[p]^=0x80;
    for(std::size_t p=0;p<r.pixels.size();p+=r.bytes)std::reverse(r.pixels.begin()+p,r.pixels.begin()+p+r.bytes);
    if(values.contains("BAYERPAT"))r.cfa=unquote(values["BAYERPAT"]);checkCfa(r);validateKeywords(r);return r;
}
std::string escape(const std::string& text){std::string out;for(unsigned char c:text){require(c>=32||c=='\n'||c=='\t',"Invalid XML metadata character");switch(c){case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;case '"':out+="&quot;";break;default:out+=c;}}return out;}
void writeXisf(const fs::path& path,Raster& r,bool compressed){
    Bytes encoded=r.pixels;std::string compression;
    if(compressed){uLongf size=compressBound(r.pixels.size());encoded.resize(size);require(compress2(encoded.data(),&size,r.pixels.data(),r.pixels.size(),6)==Z_OK,"Cannot compress XISF");encoded.resize(size);compression=" compression=\"zlib:"+std::to_string(r.pixels.size())+"\"";}
    const auto checksum=digest(encoded,"sha256");std::size_t offset=4096;
    for(int pass=0;pass<8;++pass){std::ostringstream xml;xml<<"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xisf version=\"1.0\" xmlns=\"http://www.pixinsight.com/xisf\"><Image geometry=\""<<r.width<<':'<<r.height<<':'<<r.channels<<"\" sampleFormat=\""<<r.format<<"\" colorSpace=\""<<r.color<<"\" pixelStorage=\"Planar\" byteOrder=\"little\" location=\"attachment:"<<offset<<':'<<encoded.size()<<"\" checksum=\"sha-256:"<<checksum<<"\""<<compression;
        if(r.format.starts_with("Float"))xml<<" bounds=\"0:1\"";xml<<">";
        if(!r.cfa.empty())xml<<"<ColorFilterArray width=\"2\" height=\"2\" pattern=\""<<r.cfa<<"\"/>";
        for(const auto& k:r.keywords)if(!structural(k.name))xml<<"<FITSKeyword name=\""<<escape(k.name)<<"\" value=\""<<escape(k.value)<<"\" comment=\""<<escape(k.comment)<<"\"/>";
        xml<<"</Image><Metadata><Property id=\"XISF:CreatorApplication\" type=\"String\">PhotonStack</Property><Property id=\"XISF:ChecksumAlgorithms\" type=\"String\">sha-256</Property></Metadata></xisf>";r.xml=xml.str();const auto next=((16+r.xml.size()+4095)/4096)*4096;if(next==offset)break;offset=next;
    }
    require(r.xml.size()<=maxXML&&offset>=16+r.xml.size(),"XISF header is too large");Bytes prefix(offset,0);std::memcpy(prefix.data(),"XISF0100",8);for(int i=0;i<4;++i)prefix[8+i]=(r.xml.size()>>(8*i))&255;std::memcpy(prefix.data()+16,r.xml.data(),r.xml.size());
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(prefix.data()),prefix.size());out.write(reinterpret_cast<char*>(encoded.data()),encoded.size());out.close();require(bool(out),"Cannot save XISF");
}
}
int runXisf(const std::vector<std::string>& args){
    fs::path input,output;std::size_t index=0;std::string compression="zlib";std::set<std::string> seen;const bool importing=args[0]=="xisf-import";
    for(std::size_t n=1;n<args.size();n+=2){require(n+1<args.size()&&seen.insert(args[n]).second,"Missing or duplicate XISF option");const auto& key=args[n];const auto& value=args[n+1];
        if(key=="--input")input=fs::canonical(value);else if(key=="--output-dir")output=value;else if(importing&&key=="--image")index=number(value);else if(!importing&&key=="--compression")compression=value;else throw std::invalid_argument("Unknown XISF option: "+key);}
    require(!input.empty()&&!output.empty(),"XISF exchange needs --input and a new --output-dir");require(compression=="none"||compression=="zlib","XISF export compression must be none or zlib");
    const auto sha=fileSHA256(input);auto raster=importing?readXisf(input,index):readFits(input);NewProductDirectory product(output);
    const std::string filename=importing?"image.fits":"image.xisf";if(importing)writeFits(product.stage/filename,raster);else writeXisf(product.stage/filename,raster,compression=="zlib");
    write(product.stage/"metadata.xml",Bytes(raster.xml.begin(),raster.xml.end()));require(fileSHA256(input)==sha,"Source changed during XISF conversion");
    std::ostringstream manifest;manifest<<"{\"version\":1,\"source\":"<<channelJSON(input.string())<<",\"sourceSHA256\":"<<channelJSON(sha)<<",\"command\":"<<channelJSON(args[0])<<",\"image\":"<<index<<",\"width\":"<<raster.width<<",\"height\":"<<raster.height<<",\"channels\":"<<raster.channels<<",\"sampleFormat\":"<<channelJSON(raster.format)<<",\"product\":"<<channelJSON(filename)<<",\"productSHA256\":"<<channelJSON(fileSHA256(product.stage/filename))<<",\"metadataSHA256\":"<<channelJSON(fileSHA256(product.stage/"metadata.xml"))<<",\"pixelValuesPreserved\":true,\"displayFunctionsApplied\":false}\n";
    const auto text=manifest.str();write(product.stage/"exchange.json",Bytes(text.begin(),text.end()));product.publish();
    std::cout<<"{\"type\":\"complete\",\"command\":"<<channelJSON(args[0])<<",\"directory\":"<<channelJSON(fs::absolute(output).string())<<",\"product\":"<<channelJSON(filename)<<"}\n";return 0;
}
