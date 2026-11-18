#include "photonstack/AstrometricValidation.hpp"
#include "photonstack/StarDetector.hpp"
#include "photonstack/StarCentroid.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
namespace photonstack {
std::vector<AstrometricStar> astrometricSources(const ImageBuffer& image){
    if(image.empty()||image.channels!=4||image.pixels.size()!=image.sampleCount())throw std::invalid_argument("Plate solve requires decoded RGBA master");
    ImageBuffer gray;gray.width=image.width;gray.height=image.height;gray.channels=1;gray.format=PixelFormat::Float32Gray;gray.pixels.resize(image.pixelCount());
    std::vector<float> coverage(image.pixelCount());
    for(std::size_t p=0;p<image.pixelCount();++p){const auto* c=&image.pixels[p*4];if(c[3]<.999999||!std::isfinite(c[0])||!std::isfinite(c[1])||!std::isfinite(c[2]))continue;
        gray.pixels[p]=float(.2126*double(c[0])+.7152*double(c[1])+.0722*double(c[2]));coverage[p]=1;}
    StarDetectionOptions options;options.robustStatistics=true;options.sigmaThreshold=5;options.minPeak=std::numeric_limits<float>::lowest();options.maxStars=2000;
    const auto detected=StarDetector().detectLuminanceWithCoverage(image.width,image.height,gray.pixels,coverage,options);
    if(!detected.ok)throw std::runtime_error(detected.message);
    std::vector<AstrometricStar> result;
    for(const auto& s:detected.stars){
        const int x=int(std::round(s.x)),y=int(std::round(s.y));if(x<8||y<8||x>=int(image.width)-8||y>=int(image.height)-8)continue;
        bool valid=true;for(int dy=-7;dy<=7;++dy)for(int dx=-7;dx<=7;++dx)if(coverage[std::size_t(y+dy)*image.width+unsigned(x+dx)]<1)valid=false;
        if(!valid)continue;const auto fit=fitStarCentroid(gray,s.x,s.y);if(!fit.ok||!std::isfinite(s.flux)||s.flux<=0)continue;
        result.push_back({fit.x,fit.y,s.flux,false});
    }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.flux>b.flux;});
    if(result.size()<40)throw std::runtime_error("Plate solving needs at least 40 well-fitted stars including held-out validation stars");
    for(std::size_t i=0;i<result.size();++i)result[i].heldOut=(i%5==4);
    return result;
}
SkyPoint pixelToSky(const SolvedPanel& p,double x,double y){
    const auto& g=p.grid;double u=x+1-g.x,v=y+1-g.y,du=0,dv=0;
    for(const auto& t:p.distortion){const double power=std::pow(u,t.i)*std::pow(v,t.j);du+=t.a*power;dv+=t.b*power;}
    u+=du;v+=dv;constexpr double rad=std::numbers::pi/180;
    const double a=g.ra*rad,d=g.dec*rad,xi=(g.cd[0]*u+g.cd[1]*v)*rad,eta=(g.cd[2]*u+g.cd[3]*v)*rad;
    const double xx=std::cos(d)*std::cos(a)-xi*std::sin(a)-eta*std::sin(d)*std::cos(a);
    const double yy=std::cos(d)*std::sin(a)+xi*std::cos(a)-eta*std::sin(d)*std::sin(a),zz=std::sin(d)+eta*std::cos(d);
    double ra=std::atan2(yy,xx)/rad;if(ra<0)ra+=360;
    return {ra,std::atan2(zz,std::hypot(xx,yy))/rad};
}
AstrometricResidualSummary summarizeAstrometry(const std::vector<AstrometricStar>& sources,const std::vector<AstrometricResidual>& residuals,unsigned width,unsigned height){
    AstrometricResidualSummary s;s.count=residuals.size();if(residuals.empty())return s;
    std::set<unsigned> regions;std::set<std::size_t> ids;std::vector<double> distances;double xmin=width,xmax=0,ymin=height,ymax=0;
    for(const auto& r:residuals){if(r.source>=sources.size()||!ids.insert(r.source).second||!std::isfinite(r.distance)||r.distance<0)throw std::runtime_error("Invalid astrometric residual");
        const auto& p=sources[r.source];xmin=std::min(xmin,p.x);xmax=std::max(xmax,p.x);ymin=std::min(ymin,p.y);ymax=std::max(ymax,p.y);
        regions.insert(std::min(3U,unsigned(p.y*4/height))*4+std::min(3U,unsigned(p.x*4/width)));distances.push_back(r.distance);}
    std::sort(distances.begin(),distances.end());auto quantile=[&](double f){const double at=f*(distances.size()-1);const auto i=std::size_t(at);return distances[i]+(at-i)*(distances[std::min(i+1,distances.size()-1)]-distances[i]);};
    s.regions=regions.size();s.median=quantile(.5);s.p95=quantile(.95);s.maximum=distances.back();s.spanX=(xmax-xmin)/width;s.spanY=(ymax-ymin)/height;return s;
}
void validateAstrometricGrid(const SolvedPanel& p,unsigned width,unsigned height){
    if(width<16||height<16)throw std::runtime_error("Plate solve image is too small");
    const auto& g=p.grid;const double det=g.cd[0]*g.cd[3]-g.cd[1]*g.cd[2];
    if(!std::isfinite(det)||std::abs(det)<1e-18||!std::isfinite(g.ra)||!std::isfinite(g.dec)||std::abs(g.dec)>=90)throw std::runtime_error("Invalid solved WCS");
    SkyProjection projection(p,g);
    for(unsigned j=0;j<=16;++j)for(unsigned i=0;i<=16;++i){const double x=i*(width-1.)/16,y=j*(height-1.)/16;
        const auto a=projection.toCanvas(x,y),b=projection.toCanvas(x+.01,y),c=projection.toCanvas(x,y+.01),back=projection.toPanel(a.x,a.y);
        const double jac=((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x))/.0001;
        if(!std::isfinite(jac)||jac<.5||jac>2||!std::isfinite(back.x)||!std::isfinite(back.y)||std::hypot(back.x-x,back.y-y)>1e-4)
            throw std::runtime_error("Solved WCS folds, has excessive distortion, or fails inverse mapping");
    }
}
}
