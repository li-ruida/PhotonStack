#include "PsfRestorationCommand.hpp"
#include "PsfMeasurementRecipe.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/MeasuredDeconvolution.hpp"
#include <limits>

namespace {
using namespace photonstack;using namespace photonstack::cli;namespace fs=std::filesystem;
struct Recipe {
    fs::path measurement;std::string sha;bool spatial=false;
    MeasuredDeconvolutionOptions options;
    DeconvolutionChannelMetrics metrics;
    std::size_t covered=0,protectedPixels=0;
    unsigned fw=0,fh=0;
    std::map<std::string,std::string> products;
};
std::string noiseName(PsfNoiseModel m) {
    switch(m){case PsfNoiseModel::Constant:return "constant";case PsfNoiseModel::Poisson:return "poisson";case PsfNoiseModel::PoissonGaussian:return "poisson-gaussian";}
    throw std::invalid_argument("Invalid PSF noise model");
}
PsfNoiseModel noiseModel(const std::string& name) {
    if(name=="constant")return PsfNoiseModel::Constant;if(name=="poisson")return PsfNoiseModel::Poisson;
    if(name=="poisson-gaussian")return PsfNoiseModel::PoissonGaussian;
    throw std::invalid_argument("PSF noise model must be constant, poisson or poisson-gaussian");
}
std::vector<SpatialPsfNode> fieldNodes(const PsfMeasurement& m) {
    struct Average {std::size_t n=0;double xx=0,yy=0,xy=0;};std::array<Average,4> quadrants{};
    for(const auto& star:m.stars)if(star.status=="accepted") {
        auto& q=quadrants[(star.y>=m.height*.5?2:0)+(star.x>=m.width*.5?1:0)];
        ++q.n;q.xx+=star.xx;q.yy+=star.yy;q.xy+=star.xy;
    }
    std::vector<SpatialPsfNode> nodes;
    for(std::size_t i=0;i<4;++i){const auto& q=quadrants[i];
        if(q.n<3)throw std::invalid_argument("Spatial PSF requires at least three accepted stars in every quadrant; review measurement exclusions or use the global kernel");
        nodes.push_back({i%2?.75:.25,i/2?.75:.25,gaussianPsfKernel(q.xx/q.n,q.yy/q.n,q.xy/q.n)});
    }
    return nodes;
}
const std::set<std::string> filenames{"channel.fits","restored.fits","difference.fits","residual.fits","protection.fits"};
void validate(const Recipe& r){
    const auto& o=r.options;const auto& m=r.metrics;
    (void)noiseName(o.noiseModel);
    if(!std::isfinite(o.electronsPerUnit)||o.electronsPerUnit<=0||!std::isfinite(o.readNoiseElectrons)||o.readNoiseElectrons<0)
        throw std::invalid_argument("Invalid noise gain/read noise");
    if(!r.measurement.is_absolute()||!channelSHA(r.sha)||o.iterations<1||o.iterations>100||o.threads<1||o.threads>16||
       !std::isfinite(o.amount)||o.amount<0||o.amount>1||!std::isfinite(o.pedestal)||o.pedestal<0||!std::isfinite(o.noiseSigma)||o.noiseSigma<0||!std::isfinite(o.protectAbove)||o.protectAbove<0)
        throw std::invalid_argument("Invalid PSF restoration recipe/options");
    if(!r.covered||r.covered>16*1024*1024||r.protectedPixels>r.covered||!r.fw||!r.fh||r.fw>32768||r.fh>32768||
       !std::isfinite(m.beforeRms)||m.beforeRms<0||!std::isfinite(m.afterRms)||m.afterRms<0||!std::isfinite(m.beforeFlux)||!std::isfinite(m.afterFlux)||r.products.size()!=5)
        throw std::invalid_argument("Invalid PSF restoration statistics");
    for(const auto& [name,sha]:r.products)if(!filenames.contains(name)||!channelSHA(sha))throw std::invalid_argument("Invalid PSF restoration product");
}
PsfRecipe verify(const Recipe& r){
    if(fileSHA256(r.measurement)!=r.sha)throw std::runtime_error("PSF measurement recipe changed");
    return readPsfRecipe(r.measurement);
}
void save(const fs::path& path,const Recipe& r){
    validate(r);(void)verify(r);std::ofstream out(path);const auto& o=r.options;const auto& m=r.metrics;
    out<<std::setprecision(17)<<"PhotonStack-PSF-Restoration-2\n"<<std::quoted(r.measurement.string())<<' '<<r.sha<<'\n'
       <<o.iterations<<' '<<o.threads<<' '<<o.amount<<' '<<o.pedestal<<' '<<o.noiseSigma<<' '<<o.protectAbove<<'\n'
       <<r.spatial<<' '<<noiseName(o.noiseModel)<<' '<<o.electronsPerUnit<<' '<<o.readNoiseElectrons<<'\n'
       <<r.covered<<' '<<r.protectedPixels<<' '<<r.fw<<' '<<r.fh<<' '<<m.beforeRms<<' '<<m.afterRms<<' '<<m.beforeFlux<<' '<<m.afterFlux<<'\n';
    for(const auto& [name,sha]:r.products)out<<name<<' '<<sha<<'\n';
    out.flush();if(!out)throw std::runtime_error("Cannot save PSF restoration recipe");
}
Recipe read(const fs::path& path,bool products){
    if(fs::file_size(path)>16384)throw std::invalid_argument("PSF restoration recipe too large");
    Recipe r;std::ifstream in(path);std::string token,measurement;auto& o=r.options;auto& m=r.metrics;
    if(!(in>>token)||(token!="PhotonStack-PSF-Restoration-1"&&token!="PhotonStack-PSF-Restoration-2"))throw std::invalid_argument("Invalid PSF restoration header");
    const bool version2=token=="PhotonStack-PSF-Restoration-2";
    in>>std::quoted(measurement)>>r.sha>>o.iterations>>o.threads>>o.amount>>o.pedestal>>o.noiseSigma>>o.protectAbove;
    if(version2){std::string name;in>>r.spatial>>name>>o.electronsPerUnit>>o.readNoiseElectrons;o.noiseModel=noiseModel(name);}
    in>>r.covered>>r.protectedPixels>>r.fw>>r.fh>>m.beforeRms>>m.afterRms>>m.beforeFlux>>m.afterFlux;r.measurement=measurement;
    for(int i=0;i<5;++i){std::string name,sha;if(!(in>>name>>sha)||!r.products.emplace(name,sha).second)throw std::invalid_argument("Invalid PSF restoration products");}
    if(!in||in>>token||!in.eof())throw std::invalid_argument("Invalid PSF restoration records");validate(r);(void)verify(r);
    if(products)for(const auto& [name,sha]:r.products)if(fileSHA256(path.parent_path()/name)!=sha)throw std::runtime_error("PSF restoration product changed: "+name);
    return r;
}
void print(const Recipe& r,const fs::path& directory){
    const auto p=verify(r);const auto& o=r.options;const auto& m=r.metrics;
    std::cout<<std::setprecision(17)<<"{\"type\":\"complete\",\"command\":\"psf-restore\",\"directory\":"<<channelJSON(fs::absolute(directory).string())
      <<",\"measurement\":"<<channelJSON(r.measurement.string())<<",\"source\":"<<channelJSON(p.source.string())<<",\"channel\":"<<channelJSON(p.options.channel)
      <<",\"iterations\":"<<o.iterations<<",\"threads\":"<<o.threads<<",\"amount\":"<<o.amount<<",\"pedestal\":"<<o.pedestal<<",\"noiseSigma\":"<<o.noiseSigma<<",\"protectAbove\":"<<o.protectAbove
      <<",\"spatialMode\":"<<channelJSON(r.spatial?"quadrants":"global")<<",\"noiseModel\":"<<channelJSON(noiseName(o.noiseModel))
      <<",\"electronsPerUnit\":"<<o.electronsPerUnit<<",\"readNoiseElectrons\":"<<o.readNoiseElectrons
      <<",\"coveredPixels\":"<<r.covered<<",\"protectedPixels\":"<<r.protectedPixels<<",\"fftWidth\":"<<r.fw<<",\"fftHeight\":"<<r.fh
      <<",\"beforeRms\":"<<m.beforeRms<<",\"afterRms\":"<<m.afterRms<<",\"beforeFlux\":"<<m.beforeFlux<<",\"afterFlux\":"<<m.afterFlux
      <<",\"spatiallyVariable\":"<<(p.measurement.spatiallyVariable?"true":"false")<<",\"backend\":\"CPU pocketfft double\"}\n";
}
}
int runPsfRestoration(const std::vector<std::string>& args){
    Recipe r;fs::path output,recipe;const bool fresh=args[0]=="psf-restore",inspect=args[0]=="psf-restoration-inspect";std::set<std::string> seen;
    for(std::size_t i=1;i<args.size();i+=2){
        const auto& key=args[i];if(i+1>=args.size()||!seen.insert(key).second)throw std::invalid_argument("Missing or duplicate PSF restoration option");const auto& value=args[i+1];
        if(key=="--measurement"&&fresh)r.measurement=fs::canonical(value);
        else if(key=="--recipe"&&!fresh)recipe=value;
        else if(key=="--output-dir"&&!inspect)output=value;
        else if(key=="--spatial-mode"&&fresh){if(value!="global"&&value!="quadrants")throw std::invalid_argument("Spatial mode must be global or quadrants");r.spatial=value=="quadrants";}
        else if(key=="--noise-model"&&fresh)r.options.noiseModel=noiseModel(value);
        else if(fresh&&std::set<std::string>{"--iterations","--threads","--amount","--pedestal","--noise-sigma","--protect-above","--electrons-per-unit","--read-noise-electrons"}.contains(key)){
            std::size_t used=0;const double v=std::stod(value,&used);if(used!=value.size()||!std::isfinite(v)||v<0)throw std::invalid_argument("Invalid PSF restoration number");
            auto& o=r.options;
            if(key=="--iterations"||key=="--threads"){
                if(v!=std::floor(v)||v>100)throw std::invalid_argument("Invalid PSF restoration integer");
                (key=="--iterations"?o.iterations:o.threads)=unsigned(v);
            }else if(key=="--amount")o.amount=v;else if(key=="--pedestal")o.pedestal=v;else if(key=="--noise-sigma")o.noiseSigma=v;else if(key=="--electrons-per-unit")o.electronsPerUnit=v;else if(key=="--read-noise-electrons")o.readNoiseElectrons=v;else o.protectAbove=v;
        }else throw std::invalid_argument("Unknown PSF restoration option: "+key);
    }
    if(!fresh){if(recipe.empty())throw std::invalid_argument("Specify --recipe");r=read(recipe,inspect);if(inspect){print(r,recipe.parent_path());return 0;}}
    else{if(r.measurement.empty())throw std::invalid_argument("Specify --measurement recipe.txt");r.sha=fileSHA256(r.measurement);}
    if(output.empty()||fs::exists(output))throw std::invalid_argument("Choose a new PSF restoration output directory");
    const auto measured=verify(r);auto source=readLinearScienceImage(measured.source);
    const auto& channel=measured.options.channel;
    if(source.image.width!=measured.measurement.width||source.image.height!=measured.measurement.height)throw std::invalid_argument("PSF measurement geometry changed");
    for(std::size_t p=0;p<source.image.pixelCount();++p){auto* v=&source.image.pixels[p*4];
        const double mono=channel=="luminance"?.2126*double(v[0])+.7152*double(v[1])+.0722*double(v[2]):v[channel=="red"?0:channel=="green"?1:2];
        for(unsigned c=0;c<3;++c)v[c]=float(mono);
    }
    const auto kernel=readLinearMonoMaster(r.measurement.parent_path()/"psf.fits");
    if(r.spatial)r.options.spatialNodes=fieldNodes(measured.measurement);
    auto restored=restoreMeasuredPsf(source.image,kernel.image,r.options);r.metrics=restored.channels.front();r.covered=restored.coveredPixels;r.protectedPixels=restored.protectedPixels;r.fw=restored.fftWidth;r.fh=restored.fftHeight;
    if(!fs::create_directory(output))throw std::invalid_argument("Choose a new PSF restoration output directory");
    try{
        r.products.clear();
        for(const auto& [name,im]:std::array<std::pair<const char*,const ImageBuffer*>,5>{{{"channel.fits",&source.image},{"restored.fits",&restored.image},{"difference.fits",&restored.difference},{"residual.fits",&restored.residual},{"protection.fits",&restored.protection}}}){
            const auto saved=FitsCodec().write(*im,output/name,source.sky?&source.sky->grid:nullptr,source.sky?&source.sky->distortion:nullptr);
            if(!saved.ok)throw std::runtime_error(saved.message);r.products[name]=fileSHA256(output/name);
        }
        save(output/"recipe.txt",r);print(r,output);
    }catch(...){std::error_code ec;for(const auto& name:filenames)fs::remove(output/name,ec);fs::remove(output/"recipe.txt",ec);fs::remove(output,ec);throw;}
    return 0;
}
