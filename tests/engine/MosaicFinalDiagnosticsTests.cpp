#include "photonstack/MosaicDiagnostics.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace photonstack;
void require(bool b,const char* reason){if(!b)throw std::runtime_error(reason);}
int main(){try{
    const std::array<double,4> bounds{0,0,96,96};
    const auto evaluate=[&](int mode){return detail::measureFinalMosaicSeam(bounds,3,[&](double x,double y)->std::optional<detail::MosaicFinalSample>{
        if(x<0||y<0||x>=96||y>=96||(mode==4&&x<90))return std::nullopt;
        detail::MosaicFinalSample p{};
        const double filament=20*std::exp(-.5*std::pow((x-48-6*std::sin(y*.08))/1.2,2));
        for(unsigned c=0;c<3;++c){
            const double sky=-40.+10*c+.1*x+.03*y;
            p[0][c]=p[1][c]=sky+filament;
            p[2][c]=mode==1?sky+.3*filament:mode==2?p[0][c]+(x>=48?5:0):p[0][c];
            if(mode==3)p[2][c]+=(y>=32&&y<64?8:0);
        }
        return p;
    });};
    auto unchanged=evaluate(0);require(unchanged.status=="no-large-excess","unchanged science flagged");
    require(unchanged.backgroundCandidates>=32&&unchanged.structuredSamples>=16,"no background/structure samples");
    for(auto v:unchanged.structureExcessP95)require(v==0,"unchanged filament altered");
    auto lost=evaluate(1);require(lost.status=="review","erased filament not detected");
    require(lost.structureExcessP95[0]>.1&&lost.structureExcessFraction[0]>.1,"filament high-pass loss not measured");
    auto step=evaluate(2);require(step.status=="review","introduced seam not flagged");
    require(std::abs(step.rangeExcessP95[0]-5)<1e-10,"actual output not sampled");
    require(evaluate(3).status=="review","broad halo change not flagged");
    require(evaluate(4).status=="insufficient-samples","missing coverage declared clean");
    auto flat=detail::measureFinalMosaicSeam(bounds,1,[](double,double)->std::optional<detail::MosaicFinalSample>{
        detail::MosaicFinalSample p{};for(auto& v:p)v[0]=-5;return p;});
    require(flat.status=="no-large-excess"&&flat.structuredSamples==0,"constant signed sky failed");
    std::cout<<"Final mosaic output, signed background, filament loss, halo and coverage checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
