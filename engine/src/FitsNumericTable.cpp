#include "photonstack/FitsNumericTable.hpp"
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <set>
#include <stdexcept>
namespace photonstack {
namespace {
std::string trim(std::string s) { const auto a=s.find_first_not_of(' '),b=s.find_last_not_of(' ');return a==s.npos?"":s.substr(a,b-a+1); }
using Header=std::map<std::string,std::string>;
Header header(std::ifstream& in) {
    Header h;std::array<char,2880> b{};
    for(unsigned n=0;n<365;++n){
        in.read(b.data(),b.size());if(!in)throw std::runtime_error("Truncated FITS table header");
        for(unsigned i=0;i<2880;i+=80){
            std::string card(b.data()+i,80),key=trim(card.substr(0,8));if(key=="END")return h;
            if(card[8]!='=')continue;
            std::string value=trim(card.substr(10));
            if(!value.empty()&&value[0]=='\'') {auto end=value.find('\'',1);if(end==value.npos)throw std::runtime_error("Invalid table string");value=trim(value.substr(1,end-1));}
            else value=trim(value.substr(0,value.find('/')));
            if(!h.emplace(key,value).second)throw std::runtime_error("Duplicate FITS table keyword");
        }
    }
    throw std::runtime_error("Oversized FITS table header");
}
std::size_t integer(const Header& h,const std::string& key) {
    auto it=h.find(key);if(it==h.end()||it->second.empty()||it->second.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("Invalid table integer: "+key);
    return std::stoull(it->second);
}
std::uint64_t bits(const unsigned char* p,unsigned n) {std::uint64_t v=0;for(unsigned i=0;i<n;++i)v=(v<<8)|p[i];return v;}
void card(std::ofstream& out,const std::string& key,const std::string& value) {
    std::string c=key;c.resize(8,' ');c+="= "+std::string(value.size()<20?20-value.size():0,' ')+value;if(c.size()>80)throw std::invalid_argument("Oversized FITS card");c.resize(80,' ');out<<c;
}
void end(std::ofstream& out) {out<<"END";auto pos=static_cast<std::size_t>(out.tellp());out<<std::string((2880-pos%2880)%2880,' ');}
}
FitsNumericTable readFitsNumericTable(const std::filesystem::path& file,const std::vector<std::string>& columns) {
    if(std::filesystem::file_size(file)>128*1024*1024)throw std::runtime_error("Oversized FITS table");
    if(columns.empty()||std::set<std::string>(columns.begin(),columns.end()).size()!=columns.size())throw std::invalid_argument("Invalid requested table columns");
    std::ifstream in(file,std::ios::binary);auto h=header(in);
    if(h["SIMPLE"]!="T"||integer(h,"NAXIS")!=0)throw std::runtime_error("Expected empty primary FITS HDU");
    h=header(in);
    if(h["XTENSION"]!="BINTABLE"||integer(h,"BITPIX")!=8||integer(h,"NAXIS")!=2||integer(h,"PCOUNT")!=0||integer(h,"GCOUNT")!=1)
        throw std::runtime_error("Expected fixed-width BINTABLE");
    const auto fields=integer(h,"TFIELDS"),width=integer(h,"NAXIS1"),rows=integer(h,"NAXIS2");
    if(fields>128||!width||width>65536||rows>100000||rows>64*1024*1024/width)throw std::runtime_error("FITS table dimensions exceed limit");
    struct Field {std::string name;std::size_t offset;char type;unsigned bytes;};std::vector<Field> wanted;std::size_t offset=0;
    std::set<std::string> seen;
    for(std::size_t i=1;i<=fields;++i) {
        const auto suffix=std::to_string(i),name=h["TTYPE"+suffix],form=h["TFORM"+suffix];
        if(name.empty()||!seen.insert(name).second)throw std::runtime_error("Empty or duplicate table column");
        const auto at=form.find_first_not_of("0123456789");
        if(at==form.npos||at+1!=form.size())throw std::runtime_error("Unsupported FITS table format");
        const auto count=at?std::stoull(form.substr(0,at)):1;const char type=form[at];unsigned bytes=0;
        switch(type){case 'A':case 'B':case 'L':bytes=1;break;case 'I':bytes=2;break;case 'J':case 'E':bytes=4;break;case 'K':case 'D':case 'C':bytes=8;break;case 'M':bytes=16;break;case 'X':bytes=0;break;default:throw std::runtime_error("Unsupported variable FITS column");}
        if(count>524288)throw std::runtime_error("Oversized FITS column");
        const auto length=type=='X'?(count+7)/8:count*bytes;
        if(offset>width||length>width-offset)throw std::runtime_error("FITS column exceeds row");
        if(std::find(columns.begin(),columns.end(),name)!=columns.end()) {
            if(count!=1||std::string("BIJKED").find(type)==std::string::npos||h.contains("TSCAL"+suffix)||h.contains("TZERO"+suffix)||h.contains("TNULL"+suffix))throw std::runtime_error("Requested column must be unscaled scalar numeric");
            wanted.push_back({name,offset,type,bytes});
        }
        offset+=length;
    }
    if(offset!=width||wanted.size()!=columns.size())throw std::runtime_error("Missing columns or inconsistent table row width");
    FitsNumericTable result;for(const auto& f:wanted)result[f.name].reserve(rows);
    std::vector<unsigned char> row(width);
    for(std::size_t r=0;r<rows;++r) {
        in.read(reinterpret_cast<char*>(row.data()),static_cast<std::streamsize>(width));if(!in)throw std::runtime_error("Truncated FITS table data");
        for(const auto& f:wanted){const auto b=bits(row.data()+f.offset,f.bytes);double v=0;
            switch(f.type){case 'B':v=double(b);break;case 'I':v=std::bit_cast<std::int16_t>(std::uint16_t(b));break;case 'J':v=std::bit_cast<std::int32_t>(std::uint32_t(b));break;case 'K':v=double(std::bit_cast<std::int64_t>(b));break;case 'E':v=std::bit_cast<float>(std::uint32_t(b));break;case 'D':v=std::bit_cast<double>(b);break;}
            if(!std::isfinite(v))throw std::runtime_error("Nonfinite catalog value");result[f.name].push_back(v);
        }
    }
    return result;
}
void writeFitsNumericTable(const std::filesystem::path& file,const FitsNumericTable& columns) {
    if(columns.empty()||columns.size()>128)throw std::invalid_argument("Invalid FITS table columns");
    const auto rows=columns.begin()->second.size();if(rows>100000||rows*columns.size()*8>64*1024*1024)throw std::invalid_argument("FITS table too large");
    for(const auto& [name,values]:columns) {
        if(name.empty()||name.size()>32||name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos||values.size()!=rows)
            throw std::invalid_argument("Invalid FITS table column");
        for(double v:values)if(!std::isfinite(v))throw std::invalid_argument("Nonfinite FITS table value");
    }
    if(std::filesystem::exists(file)||std::filesystem::is_symlink(file))throw std::invalid_argument("Choose new table output");
    std::ofstream out(file,std::ios::binary);if(!out)throw std::runtime_error("Cannot create FITS table");
    card(out,"SIMPLE","T");card(out,"BITPIX","8");card(out,"NAXIS","0");card(out,"EXTEND","T");end(out);
    card(out,"XTENSION","'BINTABLE'");card(out,"BITPIX","8");card(out,"NAXIS","2");card(out,"NAXIS1",std::to_string(columns.size()*8));card(out,"NAXIS2",std::to_string(rows));
    card(out,"PCOUNT","0");card(out,"GCOUNT","1");card(out,"TFIELDS",std::to_string(columns.size()));
    unsigned i=0;for(const auto& [name,values]:columns){const auto s=std::to_string(++i);card(out,"TTYPE"+s,"'"+name+"'");card(out,"TFORM"+s,"'1D'");}end(out);
    for(std::size_t r=0;r<rows;++r)for(const auto& [name,values]:columns){const auto b=std::bit_cast<std::uint64_t>(values[r]);for(int shift=56;shift>=0;shift-=8)out.put(char((b>>shift)&255));}
    const auto pos=static_cast<std::size_t>(out.tellp());out<<std::string((2880-pos%2880)%2880,'\0');out.flush();if(!out)throw std::runtime_error("Cannot save FITS table");
}
}
