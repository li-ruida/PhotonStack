#include "StarLayersCommand.hpp"
#include "NewProductDirectory.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/FileDigest.hpp"
#include "photonstack/AstroDevelop.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace {
using namespace photonstack;
namespace fs=std::filesystem;
using Args=std::map<std::string,std::string>;
void require(bool v,const char* message) { if(!v) throw std::invalid_argument(message); }
const std::string& required(const Args& a,const std::string& key) {
    if(!a.contains(key) || a.at(key).empty()) throw std::invalid_argument("Missing "+key);
    return a.at(key);
}
void keys(const Args& a,const std::set<std::string>& allowed) {
    for(const auto& [k,v]:a) if(!allowed.contains(k)) throw std::invalid_argument("Unknown linear star-layer option: "+k);
}
bool valid(const ImageBuffer& im,std::size_t p) {
    const auto* v=&im.pixels[p*4];
    return std::isfinite(v[3]) && v[3]>0 && v[3]<=1 && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
void mask(ImageBuffer& im,std::size_t p) {
    for(unsigned c=0;c<3;++c) im.pixels[p*4+c]=std::numeric_limits<float>::quiet_NaN();
    im.pixels[p*4+3]=0;
}
void write(const ImageBuffer& im,const fs::path& p,const std::optional<SolvedPanel>& sky) {
    const auto r=FitsCodec().write(im,p,sky?&sky->grid:nullptr,sky?&sky->distortion:nullptr);
    if(!r.ok) throw std::runtime_error(r.message);
}
void text(const fs::path& p,const std::string& value) {
    std::ofstream out(p);out<<value;out.close();if(!out) throw std::runtime_error("Cannot save linear star-layer record");
}
std::vector<std::string> lines(const fs::path& p) {
    require(fs::is_regular_file(p) && !fs::is_symlink(p) && fs::file_size(p)<=4096,"Invalid linear star-layer manifest");
    std::ifstream in(p);std::vector<std::string> values;std::string line;
    while(std::getline(in,line)) values.push_back(line);
    return values;
}
void verify(const fs::path& p,const std::string& hash) {
    require(hash.size()==64 && fs::is_regular_file(p) && !fs::is_symlink(p) && fileSHA256(p)==hash,
            "Linear star-layer source or product changed");
}
void preview(const ImageBuffer& im,const ImageBuffer& reference,const fs::path& p) {
    AstroDevelopOptions o;o.stellarBalance=false;o.skyPercentile=.05F;o.toneCurve=AstroToneCurve::Asinh;
    auto display=im,statistics=reference;
    for(auto* image:{&display,&statistics}) for(std::size_t p=0;p<image->pixelCount();++p) {
        if(!valid(*image,p)) mask(*image,p);
        else image->pixels[p*4+3]=1; // Display only; science retains fractional validity.
    }
    auto result=AstroDevelop().apply(display,o,&statistics);
    if(!result.ok) throw std::runtime_error("Cannot preview linear layer: "+result.message);
    ImageWriteOptions w;w.bitDepth=ImageWriteBitDepth::Sixteen;w.colorSpace=ImageWriteColorSpace::SRGB;
    const auto written=ImageCodec().write(result.image,p,w);
    if(!written.ok) throw std::runtime_error(written.message);
}
void compatible(const LinearScienceImage& a,const LinearScienceImage& b) {
    require(a.image.width==b.image.width && a.image.height==b.image.height,"Linear layers must have identical dimensions and alignment");
    // When both files declare a sky solution, do not accept shifted or rotated data.
    // Missing starless WCS is allowed only under the caller's same-grid contract.
    if(a.sky && b.sky) {
        const SkyProjection projection(*b.sky,a.sky->grid);
        const SkyProjection reference(*a.sky,a.sky->grid);
        for(double y:{0.,double(a.image.height)/2,double(a.image.height)-1})
            for(double x:{0.,double(a.image.width)/2,double(a.image.width)-1}) {
                const auto p=projection.toCanvas(x,y),q=reference.toCanvas(x,y);
                require(std::hypot(p.x-q.x,p.y-q.y)<1e-4,"Linear layer WCS does not match the source grid");
            }
    }
}
double number(const Args& a,const std::string& key,double initial,double low,double high) {
    if(!a.contains(key)) return initial;
    std::size_t n=0;const auto v=std::stod(a.at(key),&n);
    require(n==a.at(key).size() && std::isfinite(v) && v>=low && v<=high,"Invalid linear layer adjustment");return v;
}

void prepare(const Args& a) {
    keys(a,{"--input","--output-dir"});
    const fs::path path=required(a,"--input");const auto sha=fileSHA256(path);
    auto original=readLinearScienceImage(path);
    const auto& im=original.image;
    double scale=0;std::size_t good=0,invalid=0;
    std::array<std::vector<float>,3> sample;
    const auto step=std::max<std::size_t>(1,im.pixelCount()/65536);
    for(std::size_t p=0;p<im.pixelCount();++p) {
        if(!valid(im,p)) { ++invalid;continue; }
        ++good;
        for(unsigned c=0;c<3;++c) {
            scale=std::max(scale,std::abs(double(im.pixels[p*4+c])));
            if(p%step==0) sample[c].push_back(im.pixels[p*4+c]);
        }
    }
    require(good>0 && scale>0,"Linear source has no finite nonzero signal");
    std::array<float,3> fill{};
    for(unsigned c=0;c<3;++c) if(!sample[c].empty()) {
        auto& v=sample[c];auto m=v.begin()+v.size()/2;std::nth_element(v.begin(),m,v.end());fill[c]=*m;
    }
    ImageBuffer normalized;normalized.width=im.width;normalized.height=im.height;normalized.channels=3;
    normalized.pixels.resize(im.pixelCount()*3);
    for(std::size_t p=0;p<im.pixelCount();++p) for(unsigned c=0;c<3;++c)
        normalized.pixels[p*3+c]=float((valid(im,p)?im.pixels[p*4+c]:fill[c])/scale);
    cli::NewProductDirectory out(required(a,"--output-dir"));
    fs::copy_file(path,out.stage/"original.fits");verify(path,sha);verify(out.stage/"original.fits",sha);
    write(normalized,out.stage/"input.fits",original.sky);
    std::ostringstream m;m<<std::setprecision(17)<<"PhotonStack-LinearStarInput-1\n"<<sha<<'\n'
        <<fileSHA256(out.stage/"input.fits")<<'\n'<<scale<<'\n'<<invalid<<'\n';
    text(out.stage/"preparation.txt",m.str());out.publish();
    std::cout<<std::setprecision(17)<<"{\"type\":\"complete\",\"command\":\"star-layers\",\"mode\":\"linear-prepare\",\"scale\":"<<scale
        <<",\"maskedPixels\":"<<invalid<<",\"note\":\"Masked samples filled only in the neural input; original science mask is restored\"}\n";
}

void split(const Args& a,bool restore) {
    keys(a,restore?std::set<std::string>{"--prepared","--starless","--output-dir"}:
                   std::set<std::string>{"--input","--starless","--output-dir"});
    fs::path input;double scale=1;
    std::vector<std::string> preparation;
    fs::path prepared;
    if(restore) {
        prepared=required(a,"--prepared");preparation=lines(prepared/"preparation.txt");
        require(preparation.size()==5 && preparation[0]=="PhotonStack-LinearStarInput-1","Invalid linear tool preparation");
        input=prepared/"original.fits";verify(input,preparation[1]);verify(prepared/"input.fits",preparation[2]);
        Args scaleArg{{"scale",preparation[3]}};scale=number(scaleArg,"scale",1,std::numeric_limits<double>::min(),std::numeric_limits<float>::max());
    } else input=required(a,"--input");
    const fs::path removed=required(a,"--starless");
    const auto originalSha=fileSHA256(input),removedSha=fileSHA256(removed);
    auto original=readLinearScienceImage(input),starless=readLinearScienceImage(removed);
    compatible(original,starless);
    auto stars=original.image;auto& nebula=starless.image;
    double error=0;std::size_t negative=0,invalid=0;
    for(std::size_t p=0;p<stars.pixelCount();++p) {
        if(!valid(original.image,p)) { mask(stars,p);mask(nebula,p);++invalid;continue; }
        require(valid(nebula,p) && nebula.pixels[p*4+3]+1e-5>=stars.pixels[p*4+3],"Starless result lost valid source coverage");
        nebula.pixels[p*4+3]=stars.pixels[p*4+3];
        for(unsigned c=0;c<3;++c) {
            const auto i=p*4+c;
            const double restored=double(nebula.pixels[i])*scale;
            require(std::isfinite(restored) && std::abs(restored)<=std::numeric_limits<float>::max(),"Restored starless samples exceed Float32 range");
            nebula.pixels[i]=float(restored);
            stars.pixels[i]=float(double(original.image.pixels[i])-nebula.pixels[i]);
            require(std::isfinite(stars.pixels[i]),"Signed star residual exceeds Float32 range");
            negative+=stars.pixels[i]<0;
            error=std::max(error,std::abs(double(stars.pixels[i])+nebula.pixels[i]-original.image.pixels[i]));
        }
    }
    cli::NewProductDirectory out(required(a,"--output-dir"));
    fs::copy_file(input,out.stage/"original.fits");verify(out.stage/"original.fits",originalSha);
    write(stars,out.stage/"stars.fits",original.sky);write(nebula,out.stage/"starless.fits",original.sky);
    preview(stars,stars,out.stage/"stars-preview.tiff");preview(nebula,original.image,out.stage/"starless-preview.tiff");
    text(out.stage/"layers.txt","PhotonStack-StarLayers-2\nlinear\n"+originalSha+"\n"+
        fileSHA256(out.stage/"stars.fits")+"\n"+fileSHA256(out.stage/"starless.fits")+"\n"+removedSha+"\n");
    std::ostringstream record;record<<std::setprecision(17)<<"source "<<std::quoted(input.string())<<' '<<originalSha
        <<"\nstarless "<<std::quoted(removed.string())<<' '<<removedSha<<"\nrestoration-scale "<<scale
        <<"\nfloat-layer-roundtrip-max-error "<<error<<"\nidentity-render original-byte-copy\n"
        <<"external-inference not-photometric-ground-truth\n";
    text(out.stage/"provenance.txt",record.str());
    verify(input,originalSha);verify(removed,removedSha);
    if(restore) {verify(prepared/"input.fits",preparation[2]);require(lines(prepared/"preparation.txt")==preparation,"Preparation changed during restoration");}
    out.publish();
    std::cout<<std::setprecision(17)<<"{\"type\":\"complete\",\"command\":\"star-layers\",\"domain\":\"linear\",\"negativeSamples\":"
        <<negative<<",\"maskedPixels\":"<<invalid<<",\"reconstructionError\":"<<error<<"}\n";
}

void render(const Args& a) {
    keys(a,{"--layers","--output-dir","--stars-ev","--nebula-ev","--stars-saturation","--nebula-saturation"});
    const fs::path root=required(a,"--layers");const auto m=lines(root/"layers.txt");
    require(m.size()==6 && m[0]=="PhotonStack-StarLayers-2" && m[1]=="linear","Open a linear star-layer folder");
    verify(root/"original.fits",m[2]);verify(root/"stars.fits",m[3]);verify(root/"starless.fits",m[4]);
    auto original=readLinearScienceImage(root/"original.fits"),stars=readLinearScienceImage(root/"stars.fits"),nebula=readLinearScienceImage(root/"starless.fits");
    compatible(original,stars);compatible(original,nebula);
    const double evS=number(a,"--stars-ev",0,-8,4),evN=number(a,"--nebula-ev",0,-4,4);
    const double satS=number(a,"--stars-saturation",1,0,3),satN=number(a,"--nebula-saturation",1,0,3);
    const bool identity=evS==0 && evN==0 && satS==1 && satN==1;
    auto combined=original.image;
    const auto adjust=[](float* rgb,double ev,double sat) {
        const double lum=.2126*rgb[0]+.7152*rgb[1]+.0722*rgb[2];
        for(unsigned c=0;c<3;++c) {
            const auto v=(lum+sat*(rgb[c]-lum))*std::exp2(ev);
            require(std::isfinite(v) && std::abs(v)<=std::numeric_limits<float>::max(),"Linear adjustment exceeds Float32 range");rgb[c]=float(v);
        }
    };
    for(std::size_t p=0;p<combined.pixelCount();++p) {
        if(!valid(original.image,p)) {mask(combined,p);continue;}
        require(valid(stars.image,p) && valid(nebula.image,p),"Linear layer mask differs from original");
        adjust(&stars.image.pixels[p*4],evS,satS);adjust(&nebula.image.pixels[p*4],evN,satN);
        if(!identity) for(unsigned c=0;c<3;++c) {
            const auto i=p*4+c;const double v=double(stars.image.pixels[i])+nebula.image.pixels[i];
            require(std::isfinite(v) && std::abs(v)<=std::numeric_limits<float>::max(),"Recombined sample exceeds Float32 range");combined.pixels[i]=float(v);
        }
    }
    cli::NewProductDirectory out(required(a,"--output-dir"));
    if(identity) fs::copy_file(root/"original.fits",out.stage/"combined.fits");
    else write(combined,out.stage/"combined.fits",original.sky);
    preview(combined,original.image,out.stage/"combined.tiff");
    preview(stars.image,stars.image,out.stage/"stars-preview.tiff");
    preview(nebula.image,original.image,out.stage/"starless-preview.tiff");
    std::ostringstream recipe;recipe<<std::setprecision(17)<<"{\"version\":2,\"domain\":\"linear\",\"starsEV\":"<<evS
        <<",\"nebulaEV\":"<<evN<<",\"starsSaturation\":"<<satS<<",\"nebulaSaturation\":"<<satN<<",\"identity\":"<<(identity?"true":"false")<<"}\n";
    text(out.stage/"recipe.json",recipe.str());
    verify(root/"original.fits",m[2]);verify(root/"stars.fits",m[3]);verify(root/"starless.fits",m[4]);
    require(lines(root/"layers.txt")==m,"Layer manifest changed during recombination");out.publish();
    std::cout<<"{\"type\":\"complete\",\"command\":\"star-layers\",\"domain\":\"linear\",\"identity\":"<<(identity?"true":"false")<<"}\n";
}
}

int runLinearStarLayers(const std::vector<std::string>& args) {
    Args a;
    for(std::size_t i=2;i<args.size();i+=2) require(i+1<args.size() && a.emplace(args[i],args[i+1]).second,"Missing or duplicate linear star-layer option");
    if(args[1]=="linear-prepare") prepare(a);
    else if(args[1]=="linear-split") split(a,false);
    else if(args[1]=="linear-restore") split(a,true);
    else if(args[1]=="linear-render") render(a);
    else throw std::invalid_argument("Unknown linear star-layer command");
    return 0;
}
