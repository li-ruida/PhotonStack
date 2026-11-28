#include "PlateSolveCommand.hpp"
#include "LocalProcess.hpp"
#include "ChannelRecipe.hpp"
#include "photonstack/AstrometricValidation.hpp"
#include "photonstack/FitsNumericTable.hpp"
#include "photonstack/FitsCodec.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
namespace {
using namespace photonstack;using namespace photonstack::cli;namespace fs=std::filesystem;
std::string number(double x){std::ostringstream out;out<<std::setprecision(17)<<x;return out.str();}
struct Recipe {
    fs::path source,executable;std::string sourceSHA,executableSHA;
    std::vector<std::pair<fs::path,std::string>> indices;
    unsigned timeout=120;double low=0,high=0;std::map<std::string,std::string> products;
};
const std::set<std::string> products{"solved.fits","report.json","sources.xyls","solution.wcs","solution.corr","solution-indx.xyls"};
void validateInputs(const Recipe& r){
    if(!r.source.is_absolute()||!channelSHA(r.sourceSHA))throw std::runtime_error("Invalid plate solve source");
    if(!r.executable.is_absolute()||!channelSHA(r.executableSHA)||r.indices.empty()||r.indices.size()>128||r.timeout<1||r.timeout>600||
       !std::isfinite(r.low)||!std::isfinite(r.high)||r.low<0||r.high<0||((r.low==0)!=(r.high==0))||(r.low>0&&(r.high<=r.low||r.high>3600)))throw std::invalid_argument("Invalid plate solve recipe");
    std::set<fs::path> paths;
    for(const auto& [p,sha]:r.indices)
        if(!p.is_absolute()||!channelSHA(sha)||!paths.insert(p).second)throw std::runtime_error("Invalid index record");
}
void verify(const Recipe& r,bool dependencies){
    validateInputs(r);
    if(fileSHA256(r.source)!=r.sourceSHA)throw std::runtime_error("Plate solve source changed");
    if(!dependencies)return;
    if(fileSHA256(r.executable)!=r.executableSHA)throw std::runtime_error("Local solve-field changed");
    for(const auto& [p,sha]:r.indices)
        if(fileSHA256(p)!=sha)throw std::runtime_error("Astrometry index changed");
}
void writeRecipe(const fs::path& file,const Recipe& r){
    std::ofstream out(file);out<<std::setprecision(17)<<"PhotonStack-PlateSolve-1 ICRS\n"<<std::quoted(r.source.string())<<' '<<r.sourceSHA<<'\n'<<std::quoted(r.executable.string())<<' '<<r.executableSHA<<'\n'<<r.timeout<<' '<<r.low<<' '<<r.high<<' '<<r.indices.size()<<'\n';
    for(const auto& [p,sha]:r.indices)out<<std::quoted(p.string())<<' '<<sha<<'\n';for(const auto& [name,sha]:r.products)out<<name<<' '<<sha<<'\n';out.flush();if(!out)throw std::runtime_error("Cannot save plate solve recipe");
}
Recipe readRecipe(const fs::path& file,bool inspect){
    if(fs::file_size(file)>65536)throw std::invalid_argument("Oversized plate solve recipe");
    std::ifstream in(file);Recipe r;std::string magic,frame,path;std::size_t count=0;
    if(!(in>>magic>>frame)||magic!="PhotonStack-PlateSolve-1"||frame!="ICRS"||!(in>>std::quoted(path)>>r.sourceSHA))throw std::invalid_argument("Invalid plate solve recipe");r.source=path;
    if(!(in>>std::quoted(path)>>r.executableSHA>>r.timeout>>r.low>>r.high>>count)||count>128)throw std::invalid_argument("Invalid plate solve options");r.executable=path;
    for(std::size_t i=0;i<count;++i){std::string sha;if(!(in>>std::quoted(path)>>sha))throw std::invalid_argument("Incomplete index record");r.indices.emplace_back(path,sha);}
    std::string name,sha;while(in>>name)if(!(in>>sha)||!products.contains(name)||!channelSHA(sha)||!r.products.emplace(name,sha).second)throw std::invalid_argument("Invalid solve product record");
    if(!in.eof()||r.products.size()!=products.size())throw std::invalid_argument("Incomplete plate solve recipe");
    verify(r,!inspect);for(const auto& [n,s]:r.products)if(fileSHA256(file.parent_path()/n)!=s)throw std::runtime_error("Plate solve product changed: "+n);return r;
}
std::vector<AstrometricResidual> fittingResiduals(const std::vector<AstrometricStar>& stars,const SolvedPanel& sky,const fs::path& file){
    const auto table=readFitsNumericTable(file,{"field_x","field_y","index_ra","index_dec","match_weight"});std::vector<AstrometricResidual> result;std::set<std::size_t> used;
    for(std::size_t row=0;row<table.at("field_x").size();++row){
        const double weight=table.at("match_weight")[row];
        if(weight<0||weight>1)throw std::runtime_error("Invalid astrometric match probability");
        // The correspondence file includes ambiguous matches. This fixed gate
        // uses the solver probability, not our measured residual, so outliers
        // cannot be hidden by trimming to a passing residual threshold.
        if(weight<.9)continue;
        const double x=table.at("field_x")[row]-1,y=table.at("field_y")[row]-1,ra=table.at("index_ra")[row],dec=table.at("index_dec")[row];
        if(ra<0||ra>=360||std::abs(dec)>90)throw std::runtime_error("Invalid reference catalog coordinate");
        std::size_t nearest=stars.size();double distance=.02;
        for(std::size_t i=0;i<stars.size();++i)if(!stars[i].heldOut){const double d=std::hypot(stars[i].x-x,stars[i].y-y);if(d<distance){distance=d;nearest=i;}}
        if(nearest==stars.size()||!used.insert(nearest).second)throw std::runtime_error("Solver correspondence does not identify a unique submitted star");
        const auto predicted=SkyProjection(sky,TanGrid{1,1,ra,dec,{1,0,0,1}}).toPanel(0,0);
        const double residual=std::hypot(predicted.x-stars[nearest].x,predicted.y-stars[nearest].y);
        if(!std::isfinite(residual))throw std::runtime_error("Invalid reference projection");result.push_back({nearest,predicted.x,predicted.y,residual});
    }
    return result;
}
std::vector<AstrometricResidual> heldOutResiduals(const std::vector<AstrometricStar>& stars,const fs::path& file){
    const auto table=readFitsNumericTable(file,{"X","Y"});const auto& x=table.at("X");const auto& y=table.at("Y");
    std::vector<AstrometricResidual> result;std::set<std::size_t> used;
    for(std::size_t i=0;i<stars.size();++i)if(stars[i].heldOut){
        std::size_t nearest=x.size();double best=3,second=std::numeric_limits<double>::infinity();
        for(std::size_t j=0;j<x.size();++j){double d=std::hypot(x[j]-1-stars[i].x,y[j]-1-stars[i].y);if(d<best){second=best;best=d;nearest=j;}else second=std::min(second,d);}
        if(nearest==x.size()||second<3||used.contains(nearest))continue;
        // Mutual match with all detections avoids counting a held-out blend or
        // a training star's catalog counterpart a second time.
        bool mutual=true;for(std::size_t k=0;k<stars.size();++k)if(k!=i&&std::hypot(x[nearest]-1-stars[k].x,y[nearest]-1-stars[k].y)<=best){mutual=false;break;}
        if(mutual){used.insert(nearest);result.push_back({i,x[nearest]-1,y[nearest]-1,best});}
    }
    return result;
}
void summaryJSON(std::ostream& out,const AstrometricResidualSummary& s){out<<"{\"count\":"<<s.count<<",\"regions\":"<<s.regions<<",\"medianPixels\":"<<s.median<<",\"p95Pixels\":"<<s.p95<<",\"maximumPixels\":"<<s.maximum<<",\"spanX\":"<<s.spanX<<",\"spanY\":"<<s.spanY<<'}';}
}
int runPlateSolve(const std::vector<std::string>& args){
    Recipe r;fs::path output,recipe;std::string frame;std::vector<fs::path> indexPaths;std::set<std::string> seen;
    const bool fresh=args[0]=="plate-solve",inspect=args[0]=="plate-solve-inspect", exporting=args[0]=="plate-solve-export";
    for(std::size_t i=1;i<args.size();i+=2){const auto& k=args[i];if(i+1>=args.size()||(k!="--index"&&!seen.insert(k).second))throw std::invalid_argument("Missing or duplicate plate solve option");const auto& v=args[i+1];
        if(k=="--input"&&fresh)r.source=fs::canonical(v);else if(k=="--solver"&&fresh)r.executable=fs::canonical(v);
        else if(k=="--index"&&fresh){if(indexPaths.size()>=128)throw std::invalid_argument("Too many indices");indexPaths.push_back(fs::canonical(v));}
        else if(k=="--index-frame"&&fresh)frame=v;else if(k=="--output-dir"&&!inspect)output=fs::absolute(v);else if(k=="--recipe"&&!fresh)recipe=v;
        else if(fresh&&(k=="--scale-low"||k=="--scale-high"||k=="--timeout")){std::size_t used=0;const double n=std::stod(v,&used);if(used!=v.size()||!std::isfinite(n))throw std::invalid_argument("Invalid plate solve number");
            if(k=="--scale-low")r.low=n;else if(k=="--scale-high")r.high=n;else{if(n<1||n>600||n!=std::floor(n))throw std::invalid_argument("Timeout must be 1...600 seconds");r.timeout=unsigned(n);}}
        else throw std::invalid_argument("Unknown plate solve option: "+k);
    }
    if(fresh){if(r.source.empty()||r.executable.empty()||indexPaths.empty()||frame!="ICRS")throw std::invalid_argument("Specify --input master.fits, --solver /path/solve-field, --index /path/index.fits and --index-frame ICRS (declared index reference frame)");
        r.sourceSHA=fileSHA256(r.source);r.executableSHA=fileSHA256(r.executable);for(const auto& p:indexPaths)r.indices.emplace_back(p,fileSHA256(p));validateInputs(r);
    }else{if(recipe.empty())throw std::invalid_argument("Specify --recipe");r=readRecipe(recipe,inspect||exporting);if(inspect){std::ifstream in(recipe.parent_path()/"report.json");std::cout<<in.rdbuf();return 0;}}
    if(output.empty()||fs::exists(output)||fs::is_symlink(output))throw std::invalid_argument("Choose a new plate solve output directory");
    if(exporting){
        if(!fs::create_directory(output))throw std::invalid_argument("Choose a new plate solve output directory");
        try {for(const auto& name:products)fs::copy_file(recipe.parent_path()/name,output/name);
            for(const auto& [n,s]:r.products)if(fileSHA256(output/n)!=s)throw std::runtime_error("Plate solve product changed during export");
            verify(r,false);writeRecipe(output/"recipe.txt",r);std::ifstream in(output/"report.json");std::cout<<in.rdbuf();
        }catch(...){std::error_code ec;fs::remove_all(output,ec);throw;}return 0;
    }
    const auto start=std::chrono::steady_clock::now();if(!isFitsPath(r.source))throw std::invalid_argument("Plate solving currently requires a mono/RGB FITS master");const auto input=readDevelopableFits(r.source,false);const auto stars=astrometricSources(input.image);
    if(!fs::create_directory(output))throw std::invalid_argument("Choose a new plate solve output directory");
    try {
        FitsNumericTable table{{"X",{}},{"Y",{}},{"FLUX",{}}};for(const auto& s:stars)if(!s.heldOut){table["X"].push_back(s.x+1);table["Y"].push_back(s.y+1);table["FLUX"].push_back(s.flux);}
        writeFitsNumericTable(output/"sources.xyls",table);
        fs::create_directory(output/"indices");for(std::size_t i=0;i<r.indices.size();++i)fs::create_symlink(r.indices[i].first,output/"indices"/("index-"+std::to_string(i)+".fits"));
        {std::ofstream cfg(output/"astrometry.cfg");cfg<<"add_path indices\nautoindex\ncpulimit "<<r.timeout<<'\n';if(!cfg)throw std::runtime_error("Cannot save solver configuration");}
        std::vector<std::string> command{"--no-plots","--no-verify","--no-remove-lines","--uniformize","0","--sort-column","FLUX","--x-column","X","--y-column","Y",
            "--width",std::to_string(input.image.width),"--height",std::to_string(input.image.height),"--cpulimit",std::to_string(r.timeout),"--tweak-order","2",
            "--config","astrometry.cfg","--dir",".","--out","solution","--new-fits","none"};
        if(r.low>0){command.insert(command.end(),{"--scale-units","arcsecperpix","--scale-low",number(r.low),"--scale-high",number(r.high)});}command.push_back("sources.xyls");
        runLocalProcess(r.executable,command,output,r.timeout);
        std::ifstream solved(output/"solution.solved",std::ios::binary);if(solved.get()!=1)throw std::runtime_error("No astrometric solution found with the selected local indices");
        const auto sky=readAstrometryICRSPanel(output/"solution.wcs");validateAstrometricGrid(sky,input.image.width,input.image.height);
        const auto fit=fittingResiduals(stars,sky,output/"solution.corr"),held=heldOutResiduals(stars,output/"solution-indx.xyls");
        const auto fitted=summarizeAstrometry(stars,fit,input.image.width,input.image.height),validated=summarizeAstrometry(stars,held,input.image.width,input.image.height);
        if(fitted.count<12||fitted.regions<4||fitted.spanX<.4||fitted.spanY<.4||fitted.p95>2||fitted.maximum>5)
            throw std::runtime_error("Solution failed fitted-star validation (matches "+std::to_string(fitted.count)+", regions "+std::to_string(fitted.regions)+", span "+std::to_string(fitted.spanX)+"/"+std::to_string(fitted.spanY)+", P95 "+std::to_string(fitted.p95)+", maximum "+std::to_string(fitted.maximum)+" px)");
        if(validated.count<6||validated.regions<3||validated.spanX<.3||validated.spanY<.3||validated.p95>2)
            throw std::runtime_error("Solution lacks sufficient independent held-out star validation (matches "+std::to_string(validated.count)+", P95 "+std::to_string(validated.p95)+" px)");
        verify(r,true);copyFitsWithWcs(r.source,output/"solved.fits",sky);verify(r,false);
        const auto center=pixelToSky(sky,(input.image.width-1.)/2,(input.image.height-1.)/2);const auto& cd=sky.grid.cd;
        std::ostringstream report;report<<std::setprecision(17)<<"{\"type\":\"complete\",\"command\":\"plate-solve\",\"source\":"<<channelJSON(r.source.string())<<",\"sourceSHA\":"<<channelJSON(r.sourceSHA)
            <<",\"solver\":"<<channelJSON(r.executable.string())<<",\"timeout\":"<<r.timeout<<",\"scaleLow\":"<<r.low<<",\"scaleHigh\":"<<r.high<<",\"indices\":[";
        for(std::size_t i=0;i<r.indices.size();++i){if(i)report<<',';report<<channelJSON(r.indices[i].first.string());}
        report<<"],\"backend\":\"local-astrometry-net\",\"referenceFrame\":\"ICRS\",\"indexFrameUserDeclared\":true,\"blindPosition\":true,\"originalWcsIgnored\":true,\"pixelsPreserved\":true,\"width\":"<<input.image.width<<",\"height\":"<<input.image.height
            <<",\"detectedAccepted\":"<<stars.size()<<",\"submitted\":"<<table.at("X").size()<<",\"heldOut\":"<<stars.size()-table.at("X").size()<<",\"centerRA\":"<<center.x<<",\"centerDec\":"<<center.y
            <<",\"scaleArcsecPerPixel\":"<<3600*std::sqrt(std::abs(cd[0]*cd[3]-cd[1]*cd[2]))<<",\"minimumFitMatchWeight\":0.9,\"fitted\":";summaryJSON(report,fitted);report<<",\"validation\":";summaryJSON(report,validated);
        report<<",\"elapsedSeconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<",\"stars\":[";
        for(std::size_t i=0;i<stars.size();++i){if(i)report<<',';const auto& s=stars[i];report<<"{\"id\":"<<i<<",\"x\":"<<s.x<<",\"y\":"<<s.y<<",\"heldOut\":"<<(s.heldOut?"true":"false")<<'}';}
        report<<"],\"residuals\":[";bool comma=false;for(const auto* residuals:{&fit,&held})for(const auto& v:*residuals){if(comma)report<<',';comma=true;report<<"{\"source\":"<<v.source<<",\"predictedX\":"<<v.predictedX<<",\"predictedY\":"<<v.predictedY<<",\"distance\":"<<v.distance<<'}';}report<<"]}\n";
        {std::ofstream out(output/"report.json");out<<report.str();out.flush();if(!out)throw std::runtime_error("Cannot save plate solve report");}
        r.products.clear();for(const auto& name:products)r.products[name]=fileSHA256(output/name);writeRecipe(output/"recipe.txt",r);
        fs::remove_all(output/"indices");std::cout<<report.str();
    }catch(const std::exception& failure){
        std::ifstream log(output/"solver.log",std::ios::binary);if(log){log.seekg(0,std::ios::end);auto length=log.tellg();log.seekg(std::max<std::streamoff>(0,static_cast<std::streamoff>(length)-4096));if(length>0){const std::string tail{std::istreambuf_iterator<char>(log), {}};std::cerr<<tail<<'\n';}}
        std::error_code ec;fs::remove(output/"solved.fits",ec);fs::remove(output/"recipe.txt",ec);fs::remove_all(output/"indices",ec);
        std::ofstream failed(output/"failure.txt");failed<<failure.what()<<'\n';throw;
    }
    return 0;
}
