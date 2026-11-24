#include "TimeSeriesCommand.hpp"
#include "ChannelRecipe.hpp"
#include "NewProductDirectory.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/StellarPhotometry.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>

namespace {
using namespace photonstack;using namespace photonstack::cli;namespace fs=std::filesystem;
void require(bool v,const std::string& text){if(!v)throw std::invalid_argument(text);}
double number(const std::string& text){std::size_t n=0;const auto v=std::stod(text,&n);require(n==text.size()&&std::isfinite(v),"Invalid photometry number");return v;}
struct Star {std::string name;double x=0,y=0;};
struct Frame {fs::path path;std::string time,sha;double exposure=0;};
struct Recipe {std::vector<Star> stars;std::vector<Frame> frames;StellarApertureOptions aperture;double gain=0;unsigned channel=0;std::string channelName="mono",filter;bool aligned=false;};
Recipe recipe(const fs::path& path){
    require(fs::is_regular_file(path)&&fs::file_size(path)<=4*1024*1024,"Photometry recipe exceeds 4 MiB");std::ifstream input(path);std::string line;std::getline(input,line);require(line=="PhotonStack-TimeSeries-1","Invalid time-series recipe version");
    Recipe r;std::set<std::string> seen,names,paths;bool target=false;
    while(std::getline(input,line)){
        if(line.empty())continue;std::istringstream in(line);std::string key;in>>key;
        if(key=="target"||key=="comparison"){
            Star star;require(bool(in>>std::quoted(star.name)>>star.x>>star.y)&&std::isfinite(star.x)&&std::isfinite(star.y)&&star.x>=0&&star.y>=0,"Invalid photometry star coordinates");
            require(!star.name.empty()&&star.name.size()<=64&&star.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")==star.name.npos&&names.insert(star.name).second,"Star IDs must be unique alphanumeric names");
            if(key=="target"){require(!target,"Duplicate target");r.stars.insert(r.stars.begin(),star);target=true;}else r.stars.push_back(star);
        }else if(key=="frame"){
            Frame frame;std::string source;require(bool(in>>std::quoted(source))&&fs::path(source).is_absolute(),"Frame path must be absolute");frame.path=fs::canonical(source);require(paths.insert(frame.path.string()).second&&r.frames.size()<10000,"Duplicate or excessive frame count");
            if(in>>std::quoted(frame.time)){require(bool(in>>frame.exposure)&&std::isfinite(frame.exposure)&&frame.exposure>0,"Explicit time requires a positive exposure");}
            else in.clear();r.frames.push_back(frame);
        }else {
            std::string value;require(seen.insert(key).second&&bool(in>>std::quoted(value)),"Missing or duplicate photometry option");
            if(key=="aligned")r.aligned=value=="true";
            else if(key=="filter")r.filter=value;
            else if(key=="channel"){r.channelName=value;if(value=="red"||value=="mono")r.channel=0;else if(value=="green")r.channel=1;else if(value=="blue")r.channel=2;else throw std::invalid_argument("Choose mono, red, green or blue");}
            else if(key=="effective-gain")r.gain=number(value);
            else if(key=="aperture")r.aperture.radius=number(value);
            else if(key=="sky-inner")r.aperture.skyInner=number(value);
            else if(key=="sky-outer")r.aperture.skyOuter=number(value);
            else if(key=="saturation")r.aperture.saturation=number(value);
            else if(key=="min-snr")r.aperture.minSNR=number(value);
            else if(key=="max-shift")r.aperture.maxCenterOffset=number(value);
            else throw std::invalid_argument("Unknown photometry option: "+key);
        }
        std::string extra;require(!(in>>extra),"Trailing photometry recipe data");
    }
    require(target&&r.stars.size()>=3&&r.stars.size()<=17&&r.frames.size()>=3,"Choose one target, 2..16 comparison stars and 3..10000 frames");
    require(r.aligned&&r.gain>0&&r.gain<=1e9&&!r.filter.empty()&&r.filter.size()<=64&&r.filter.find_first_of("\r\n")==r.filter.npos,"Confirm common grid/filter and supply positive effective electrons per sample unit");
    for(std::size_t a=0;a<r.stars.size();++a)for(std::size_t b=a+1;b<r.stars.size();++b)
        require(std::hypot(r.stars[a].x-r.stars[b].x,r.stars[a].y-r.stars[b].y)>2*(r.aperture.skyOuter+r.aperture.maxCenterOffset+1),"Star measurement regions overlap; choose more separated comparison stars");
    return r;
}
std::string value(const std::map<std::string,std::string>& h,const std::string& key){const auto i=h.find(key);return i==h.end()?std::string():i->second;}
double utcJD(std::string text){
    if(text.ends_with("Z"))text.pop_back();require(text.size()>=19&&text[4]=='-'&&text[7]=='-'&&text[10]=='T'&&text[13]==':'&&text[16]==':',"Use UTC YYYY-MM-DDThh:mm:ss[.fraction][Z]");
    auto integer=[&](std::size_t p,std::size_t n){const auto s=text.substr(p,n);require(s.find_first_not_of("0123456789")==s.npos,"Invalid UTC timestamp");return unsigned(std::stoul(s));};
    const auto y=integer(0,4),m=integer(5,2),d=integer(8,2),hour=integer(11,2),minute=integer(14,2);const auto secondText=text.substr(17);require(secondText.size()>=2&&secondText.substr(0,2).find_first_not_of("0123456789")==std::string::npos&&(secondText.size()==2||(secondText.size()>3&&secondText[2]=='.'&&secondText.substr(3).find_first_not_of("0123456789")==std::string::npos)),"Invalid UTC seconds");const auto seconds=number(secondText);
    const std::chrono::year_month_day date{std::chrono::year(int(y)),std::chrono::month(m),std::chrono::day(d)};
    require(date.ok()&&y>=1900&&y<=2200&&hour<24&&minute<60&&seconds>=0&&seconds<60,"Invalid UTC date or unsupported leap-second sample");
    return 2440587.5+double(std::chrono::sys_days(date).time_since_epoch().count())+(hour*3600.+minute*60.+seconds)/86400.;
}
// IERS Bulletin C 72 table, valid until 2027-06-28. Leap-second days
// need a quasi-JD conversion; reject them instead of silently treating them as 86400 s.
void validateUTCInterval(double start,double duration){
    require(std::isfinite(start)&&std::isfinite(duration)&&duration>0&&duration<=86400,"Invalid exposure duration");
    const double mjd=start-2400000.5,end=mjd+duration/86400.;
    require(mjd>=41317&&end<61584,"UTC timestamp outside the verified IERS table (1972-01-01 through 2027-06-27)");
    for(double transition:{41499.,41683.,42048.,42413.,42778.,43144.,43509.,43874.,44239.,44786.,45151.,45516.,46247.,47161.,47892.,48257.,48804.,49169.,49534.,50083.,50630.,51179.,53736.,54832.,56109.,57204.,57754.})
        require(!(mjd<transition&&end>=transition-1),"Exposure touches a UTC leap-second day; convert timing with a leap-aware tool");
}
std::pair<double,double> timing(const Frame& f,const std::map<std::string,std::string>& h){
    if(!f.time.empty()){const auto start=utcJD(f.time);validateUTCInterval(start,f.exposure);return {start+f.exposure/172800.,f.exposure};}
    require(value(h,"TIMESYS").empty()||value(h,"TIMESYS")=="UTC","Non-UTC FITS timestamps need explicit UTC conversion");
    auto exposure=value(h,"EXPTIME");if(exposure.empty())exposure=value(h,"EXPOSURE");require(!exposure.empty(),"Missing exposure duration");const auto duration=number(exposure);if(!value(h,"EXPTIME").empty()&&!value(h,"EXPOSURE").empty())require(std::abs(number(value(h,"EXPTIME"))-number(value(h,"EXPOSURE")))<1e-6,"EXPTIME and EXPOSURE disagree");
    auto stamp=value(h,"DATE-OBS"),mjd=value(h,"MJD-OBS");require(!stamp.empty()||!mjd.empty(),"Missing UTC exposure start");
    const auto start=stamp.empty()?number(mjd)+2400000.5:utcJD(stamp);
    if(!stamp.empty()&&!mjd.empty())require(std::abs(start-number(mjd)-2400000.5)*86400<.1,"DATE-OBS and MJD-OBS disagree");
    validateUTCInterval(start,duration);return {start+duration/172800.,duration};
}
double median(std::vector<double> values){require(!values.empty(),"Empty photometry sample");std::sort(values.begin(),values.end());const auto n=values.size();return n%2?values[n/2]:(values[n/2-1]+values[n/2])*.5;}
struct Measurement {StellarApertureResult aperture;double error=0;};
struct Row {std::size_t index;std::optional<double> jd;double exposure=0,ratio=0,ratioError=0,ensemble=0;std::string status="measured",detail;std::vector<Measurement> stars;};
std::string csv(const std::string& text){std::string out="\"";for(char c:text){out+=c;if(c=='"')out+=c;}return out+'"';}
void validateGrid(const LinearScienceImage& ref,const LinearScienceImage& image){
    require(ref.image.width==image.image.width&&ref.image.height==image.image.height&&bool(ref.sky)==bool(image.sky),"Sequence grid differs from first frame");
    if(ref.sky){SkyProjection projection(*image.sky,ref.sky->grid),identity(*ref.sky,ref.sky->grid);for(double y:{0.,(image.image.height-1.)/2,image.image.height-1.})for(double x:{0.,(image.image.width-1.)/2,image.image.width-1.}){const auto a=projection.toCanvas(x,y),b=identity.toCanvas(x,y);require(std::hypot(a.x-b.x,a.y-b.y)<.01,"Sequence WCS grid differs from first frame");}}
}
}
int runTimeSeries(const std::vector<std::string>& args){
    fs::path path,destination;std::set<std::string> seen;
    for(std::size_t n=1;n<args.size();n+=2){require(n+1<args.size()&&seen.insert(args[n]).second,"Missing or duplicate time-series option");if(args[n]=="--recipe")path=fs::canonical(args[n+1]);else if(args[n]=="--output-dir")destination=args[n+1];else throw std::invalid_argument("Unknown time-series option");}
    require(!path.empty()&&!destination.empty(),"Time series needs a recipe and a new output directory");const auto recipeSHA=fileSHA256(path);auto r=recipe(path);
    auto reference=readLinearScienceImage(r.frames[0].path);auto refMetadata=FitsCodec().inspect(r.frames[0].path);require(refMetadata.ok,"Invalid reference FITS");
    require((r.channelName=="mono"&&refMetadata.metadata.channels==1)||(r.channelName!="mono"&&refMetadata.metadata.channels>=3),"Selected photometry channel does not match input channels");
    // Validate aperture options once, before any staged publication.
    (void)measureStellarAperture(reference.image,r.stars[0].x,r.stars[0].y,r.aperture);
    std::vector<Row> rows;std::vector<double> ratios;std::set<double> times;
    for(std::size_t i=0;i<r.frames.size();++i){auto& frame=r.frames[i];frame.sha=fileSHA256(frame.path);Row row{i};
        try {
            const auto header=readFitsPrimaryValues(frame.path);const auto [jd,exposure]=timing(frame,header);row.jd=jd;row.exposure=exposure;
            require(times.insert(jd).second,"Duplicate exposure midpoint");const auto filter=value(header,"FILTER");require(filter.empty()||filter==r.filter,"Frame filter differs from declared sequence filter");
            auto image=readLinearScienceImage(frame.path);const auto metadata=FitsCodec().inspect(frame.path);require(metadata.ok&&metadata.metadata.channels==refMetadata.metadata.channels,"Sequence channel count changed");validateGrid(reference,image);
            // Photometry of one declared channel: no hidden luminance mixing.
            for(std::size_t pixel=0;pixel<image.image.pixelCount();++pixel){const auto v=image.image.pixels[pixel*4+r.channel];for(unsigned c=0;c<3;++c)image.image.pixels[pixel*4+c]=v;}
            for(const auto& star:r.stars){Measurement m;m.aperture=measureStellarAperture(image.image,star.x,star.y,r.aperture);m.error=std::sqrt(m.aperture.backgroundNoise[0]*m.aperture.backgroundNoise[0]+m.aperture.sourceShotCoefficient[0]/r.gain);row.stars.push_back(m);if(!m.aperture.valid&&row.status=="measured"){row.status="star-rejected";row.detail=star.name+":"+m.aperture.status;}}
            if(row.status=="measured"){
                double variance=0;for(std::size_t s=1;s<row.stars.size();++s){row.ensemble+=row.stars[s].aperture.flux[0];variance+=row.stars[s].error*row.stars[s].error;}
                const auto target=row.stars[0].aperture.flux[0];row.ratio=target/row.ensemble;row.ratioError=row.ratio*std::sqrt(std::pow(row.stars[0].error/target,2)+variance/(row.ensemble*row.ensemble));
                require(std::isfinite(row.ratio)&&std::isfinite(row.ratioError)&&row.ratio>0,"Invalid differential flux");ratios.push_back(row.ratio);
            }
        }catch(const std::exception& error){row.status="frame-rejected";row.detail=error.what();}
        require(fileSHA256(frame.path)==frame.sha,"Source changed during photometry");rows.push_back(std::move(row));std::cerr<<"Photometry frame "<<i+1<<" / "<<r.frames.size()<<'\n';
    }
    require(fileSHA256(path)==recipeSHA,"Photometry recipe changed during processing");const double baseline=ratios.empty()?0:median(ratios);
    NewProductDirectory output(destination);fs::copy_file(path,output.stage/"recipe.txt");require(fileSHA256(output.stage/"recipe.txt")==recipeSHA,"Photometry recipe changed before saving");
    std::ofstream curve(output.stage/"lightcurve.csv"),stars(output.stage/"measurements.csv"),report(output.stage/"report.json");curve<<std::setprecision(17);stars<<std::setprecision(17);report<<std::setprecision(17);
    curve<<"frame,path,jd_utc_mid,exposure_seconds,status,detail,target_flux,target_flux_error,ensemble_flux,target_over_ensemble,ratio_error,relative_flux,relative_error,differential_magnitude,magnitude_error\n";
    stars<<"frame,star,role,status,x,y,center_offset,aperture_area,flux,flux_error,background_only_error,source_shot_coefficient,sky_sigma,sky_samples\n";
    report<<"{\"version\":1,\"recipeSHA256\":"<<channelJSON(recipeSHA)<<",\"frames\":"<<rows.size()<<",\"accepted\":"<<ratios.size()<<",\"channel\":"<<channelJSON(r.channelName)<<",\"filter\":"<<channelJSON(r.filter)<<",\"effectiveGain\":"<<r.gain<<",\"normalization\":";if(ratios.empty())report<<"null";else report<<baseline;report<<",\"timing\":\"JD_UTC_exposure_midpoint\",\"uncertainty\":\"empirical_background_plus_source_poisson_independent_pixels\",\"saturationKnown\":"<<(r.aperture.saturation>0?"true":"false")<<",\"sources\":[";
    for(std::size_t i=0;i<rows.size();++i){const auto& row=rows[i];const auto& frame=r.frames[i];if(i)report<<',';report<<"{\"path\":"<<channelJSON(frame.path.string())<<",\"sha256\":"<<channelJSON(frame.sha)<<",\"explicitTiming\":"<<(!frame.time.empty()?"true":"false")<<'}';
        curve<<i<<','<<csv(frame.path.string())<<',';if(row.jd)curve<<*row.jd;curve<<',';if(row.exposure>0)curve<<row.exposure;curve<<','<<csv(row.status)<<','<<csv(row.detail)<<',';
        if(row.status=="measured")curve<<row.stars[0].aperture.flux[0]<<','<<row.stars[0].error<<','<<row.ensemble<<','<<row.ratio<<','<<row.ratioError<<','<<row.ratio/baseline<<','<<row.ratioError/baseline<<','<<-2.5*std::log10(row.ratio)<<','<<(2.5/std::log(10.))*row.ratioError/row.ratio;
        else curve<<",,,,,,,,";curve<<'\n';
        for(std::size_t s=0;s<row.stars.size();++s){const auto& m=row.stars[s];const auto& a=m.aperture;stars<<i<<','<<r.stars[s].name<<','<<(s?"comparison":"target")<<','<<a.status<<','<<a.x<<','<<a.y<<','<<a.centerOffset<<','<<a.area<<',';
            if(a.valid)stars<<a.flux[0]<<','<<m.error<<','<<a.backgroundNoise[0]<<','<<a.sourceShotCoefficient[0]<<','<<a.skySigma[0]<<','<<a.skyUsed[0];else stars<<",,,,,";stars<<'\n';}
    }
    report<<"],\"points\":[";
    for(std::size_t i=0;i<rows.size();++i){const auto& row=rows[i];if(i)report<<',';report<<"{\"index\":"<<i<<",\"status\":"<<channelJSON(row.status)<<",\"detail\":"<<channelJSON(row.detail)<<",\"jdUTC\":";if(row.jd)report<<*row.jd;else report<<"null";report<<",\"relativeFlux\":";if(row.status=="measured")report<<row.ratio/baseline;else report<<"null";report<<",\"relativeError\":";if(row.status=="measured")report<<row.ratioError/baseline;else report<<"null";report<<'}';}
    report<<"],\"comparisons\":[";
    for(std::size_t s=1;s<r.stars.size();++s){std::vector<double> values;for(const auto& row:rows)if(row.status=="measured"){const auto flux=row.stars[s].aperture.flux[0];values.push_back(flux/(row.ensemble-flux));}
        if(s>1)report<<',';report<<"{\"id\":"<<channelJSON(r.stars[s].name)<<",\"samples\":"<<values.size()<<",\"leaveOneOutRelativeRMS\":";
        if(values.size()<3)report<<"null";else{const auto center=median(values);double sum=0;for(auto v:values)sum+=std::pow(v/center-1,2);report<<std::sqrt(sum/values.size());}report<<'}';}
    curve.close();stars.close();require(bool(curve)&&bool(stars),"Cannot write photometry CSV");
    report<<"],\"products\":{\"lightcurve.csv\":"<<channelJSON(fileSHA256(output.stage/"lightcurve.csv"))<<",\"measurements.csv\":"<<channelJSON(fileSHA256(output.stage/"measurements.csv"))<<"},\"limitations\":[\"No barycentric or heliocentric correction\",\"No absolute magnitude or standard-filter transformation\",\"No scintillation, flat-field systematics or resampling covariance\",\"Normalization uncertainty is not included\",\"Comparison stability requires review; no automatic variable-star exclusion\"]}\n";report.close();require(bool(report),"Cannot save photometry report");std::ofstream manifest(output.stage/"report.sha256");manifest<<fileSHA256(output.stage/"report.json")<<'\n';manifest.close();require(bool(manifest),"Cannot save photometry report digest");output.publish();
    std::cout<<"{\"type\":\"complete\",\"command\":\"time-series\",\"directory\":"<<channelJSON(fs::absolute(destination).string())<<",\"frames\":"<<rows.size()<<",\"accepted\":"<<ratios.size()<<"}\n";return 0;
}
