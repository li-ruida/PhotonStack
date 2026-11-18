#include "PsfMeasurementCommand.hpp"
#include <iostream>
#include "PsfMeasurementRecipe.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/ImageResizer.hpp"
#include <algorithm>
#include <limits>

namespace {
using namespace photonstack;using namespace photonstack::cli;namespace fs=std::filesystem;
void preview(const ImageBuffer& source,const PsfMeasurement& measurement,const fs::path& destination){
    const unsigned width=std::min(source.width,unsigned(std::max(1.,std::round(1200.*source.width/std::max(source.width,source.height)))));
    auto resized=ImageResizer().resizeToWidth(source,width);
    if(!resized.ok)throw std::runtime_error(resized.message);
    auto& image=resized.image;
    std::vector<float> values;
    for(std::size_t i=0;i<image.pixelCount();++i)if(image.pixels[i*4+3]>.99F)for(unsigned c=0;c<3;++c)
        if(std::isfinite(image.pixels[i*4+c]))values.push_back(image.pixels[i*4+c]);
    if(values.empty())throw std::runtime_error("No fully covered preview pixels");
    std::sort(values.begin(),values.end());const double low=values[std::size_t(.01*(values.size()-1))],high=values[std::size_t(.998*(values.size()-1))];
    const double span=std::max(high-low,1e-12);
    for(std::size_t i=0;i<image.pixelCount();++i)for(unsigned c=0;c<3;++c){
        const double value=(image.pixels[i*4+c]-low)/span;
        image.pixels[i*4+c]=float(std::clamp(std::asinh(10*std::max(0.,value))/std::asinh(10.),0.,1.));
    }
    const double sx=double(image.width)/source.width,sy=double(image.height)/source.height;
    for(const auto& s:measurement.stars){
        const int x=int(std::round((s.x+.5)*sx-.5)),y=int(std::round((s.y+.5)*sy-.5));
        const std::array<float,3> color=s.status=="accepted"?std::array<float,3>{0,1,.7F}:s.status=="excluded"?std::array<float,3>{.6F,.6F,.6F}:s.status=="saturated"?std::array<float,3>{1,.7F,0}:std::array<float,3>{1,.2F,.2F};
        for(int dy=-5;dy<=5;++dy)for(int dx=-5;dx<=5;++dx){
            const double radius=std::hypot(dx,dy);
            if(radius<4||radius>5||x+dx<0||y+dy<0||x+dx>=int(image.width)||y+dy>=int(image.height))continue;
            const auto p=(std::size_t(y+dy)*image.width+unsigned(x+dx))*4;
            for(unsigned c=0;c<3;++c)image.pixels[p+c]=color[c];image.pixels[p+3]=1;
        }
    }
    image.colorEncoding=ColorEncoding::SRGB;
    ImageWriteOptions options;options.bitDepth=ImageWriteBitDepth::Sixteen;
    const auto written=ImageCodec().write(image,destination,options);if(!written.ok)throw std::runtime_error(written.message);
}
}
int runPsfMeasurement(const std::vector<std::string>& args){
    const bool inspect=args[0]=="psf-inspect";
    std::map<std::string,std::string> values;
    for(std::size_t i=1;i<args.size();i+=2){
        if(i+1>=args.size()||!std::set<std::string>{"--input","--output-dir","--channel","--sigma","--saturation","--exclude","--expected-sha","--recipe"}.contains(args[i])||!values.emplace(args[i],args[i+1]).second)
            throw std::invalid_argument("Invalid or duplicate PSF measurement option");
    }
    if(inspect){if(values.size()!=1||!values.contains("--recipe"))throw std::invalid_argument("Use psf-inspect --recipe recipe.txt");
        const fs::path path=values.at("--recipe");printPsfRecipe(readPsfRecipe(path),path.parent_path());return 0;}
    if(!values.contains("--input")||!values.contains("--output-dir")||values.contains("--recipe"))throw std::invalid_argument("Specify PSF --input and --output-dir");
    PsfRecipe r;r.source=fs::canonical(values.at("--input"));r.sha=fileSHA256(r.source);
    if(values.contains("--expected-sha")&&values.at("--expected-sha")!=r.sha)throw std::runtime_error("PSF source changed");
    if(values.contains("--channel"))r.options.channel=values.at("--channel");
    for(const auto* name:{"--sigma","--saturation"})if(values.contains(name)){
        std::size_t n=0;const auto& text=values.at(name);const double v=std::stod(text,&n);
        if(n!=text.size()||!std::isfinite(v))throw std::invalid_argument("Invalid PSF numeric value");
        (std::string(name)=="--sigma"?r.options.sigmaThreshold:r.options.saturation)=v;
    }
    if(values.contains("--exclude")&&!values.at("--exclude").empty()){
        const auto& text=values.at("--exclude");if(text.back()==',')throw std::invalid_argument("Invalid excluded star ID list");
        std::istringstream ids(text);std::string item;
        while(std::getline(ids,item,',')){
            if(item.empty()||item.find_first_not_of("0123456789")!=std::string::npos||r.options.excluded.size()>=2048)throw std::invalid_argument("Invalid excluded star ID list");
            r.options.excluded.push_back(std::stoul(item));
        }
    }
    const fs::path directory=values.at("--output-dir");if(directory.empty()||fs::exists(directory))throw std::invalid_argument("Choose a new output directory");
    const auto source=readLinearScienceImage(r.source);
    r.measurement=measurePsf(source.image,r.options);
    const auto kernel=gaussianPsfKernel(r.measurement.xx,r.measurement.yy,r.measurement.xy);
    if(!fs::create_directory(directory))throw std::invalid_argument("Choose a new output directory");
    try{
        const auto saved=FitsCodec().write(kernel,directory/"psf.fits");if(!saved.ok)throw std::runtime_error(saved.message);
        preview(source.image,r.measurement,directory/"stars-preview.tiff");
        for(const auto* name:{"psf.fits","stars-preview.tiff"})r.products[name]=fileSHA256(directory/name);
        writePsfRecipe(directory/"recipe.txt",r);printPsfRecipe(r,directory);
    }catch(...){std::error_code ec;for(const auto* name:{"psf.fits","stars-preview.tiff","recipe.txt"})fs::remove(directory/name,ec);fs::remove(directory,ec);throw;}
    return 0;
}
