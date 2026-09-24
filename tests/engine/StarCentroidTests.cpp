#include "photonstack/Registration.hpp"
#include "photonstack/StarCentroid.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace photonstack;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

ImageBuffer blank(int w, int h, int channels = 1) {
    ImageBuffer im; im.width=w; im.height=h; im.channels=channels;
    im.pixels.assign(im.sampleCount(),0); return im;
}
void addStar(ImageBuffer& im, double x, double y, double sx, double sy, double rho, double amplitude) {
    for (int yy=std::max(0,int(y)-12); yy<std::min(int(im.height),int(y)+13); ++yy)
        for (int xx=std::max(0,int(x)-12); xx<std::min(int(im.width),int(x)+13); ++xx) {
            const double u=(xx-x)/sx, v=(yy-y)/sy;
            im.pixels[yy*im.width+xx]+=amplitude*std::exp(-.5*(u*u+v*v-2*rho*u*v)/(1-rho*rho));
        }
}
void testCentroidTruth() {
    double maximumError=0;
    for (double sigma : {.8,1.,1.5,2.}) for (int phase=0;phase<20;++phase) {
        auto im=blank(41,41); const double x=20+phase*.05, y=20.37;
        for (int yy=0;yy<41;++yy) for(int xx=0;xx<41;++xx) im.pixels[yy*41+xx]=1000+2*xx-3*yy;
        addStar(im,x,y,sigma,sigma*1.1,.2,10000);
        auto f=fitStarCentroid(im,std::round(x),std::round(y));
        require(f.ok,"Known isolated Gaussian rejected");
        const double error=std::hypot(f.x-x,f.y-y); maximumError=std::max(maximumError,error);
        require(error<.001,"Subpixel centroid truth mismatch");
        require(std::abs(f.covarianceXX-sigma*sigma)<.001 &&
                std::abs(f.covarianceYY-sigma*sigma*1.21)<.001 &&
                std::abs(f.covarianceXY-sigma*sigma*.22)<.001,
                "Native Gaussian covariance truth mismatch");
        for (auto& p:im.pixels) p=float(p*.001-5);
        auto scaled=fitStarCentroid(im,std::round(x),std::round(y));
        require(scaled.ok && std::hypot(scaled.x-f.x,scaled.y-f.y)<.001,"Centroid changed with ADU scale/offset");
        require(std::abs(scaled.covarianceXX-f.covarianceXX)<.001 &&
                std::abs(scaled.covarianceYY-f.covarianceYY)<.001 &&
                std::abs(scaled.covarianceXY-f.covarianceXY)<.001,
                "PSF covariance changed with ADU scale/offset");
    }
    auto half=blank(41,41);addStar(half,20.5,20.5,1.5,1.5,0,1000);
    require(fitStarCentroid(half,20,20).ok,"Half-pixel symmetry mistaken for clipping");
    std::cout<<"Centroid maximum truth error: "<<maximumError<<" px\n";
}
void testBadSources() {
    auto im=blank(41,41);addStar(im,20.3,20.4,1.4,1.4,0,1000);
    auto masked=blank(41,41,4);
    for (int p=0;p<41*41;++p) {
        for(int c=0;c<3;++c) masked.pixels[p*4+c]=im.pixels[p];
        masked.pixels[p*4+3]=1;
    }
    require(fitStarCentroid(masked,20,20).ok,"Valid RGBA source rejected");
    masked.pixels[(22*41+21)*4+3]=.8;
    require(!fitStarCentroid(masked,20,20).ok,"Partial coverage source accepted");
    require(!fitStarCentroid(im,3,20).ok,"Truncated edge source accepted");
    im.pixels[20*41+20]=std::numeric_limits<float>::quiet_NaN();
    require(!fitStarCentroid(im,20,20).ok,"Nonfinite source accepted");
    im=blank(41,41);addStar(im,20,20,1.8,1.8,0,1000);
    for(auto& p:im.pixels) p=std::min(p,400.F);
    require(!fitStarCentroid(im,20,20).ok,"Clipped plateau accepted");
    im=blank(41,41);im.pixels[20*41+20]=1000;
    require(!fitStarCentroid(im,20,20).ok,"Single-pixel defect accepted");
}
void testRegistrationTruth(bool affine = false, unsigned seed = 73) {
    auto ref=blank(640,480), moving=blank(640,480);
    std::mt19937 rng(seed);std::normal_distribution<float> noise(0,.4);
    for (int y=0;y<480;++y) for(int x=0;x<640;++x) {
        ref.pixels[y*640+x]=50+.05*x-.02*y+noise(rng);
        moving.pixels[y*640+x]=300-.03*x+.04*y+noise(rng);
    }
    const double scale=1.001,angle=.006,dx=1.37,dy=-2.43;
    const double a=scale*std::cos(angle), b=scale*std::sin(angle);
    const double aa=a+(affine?.001:0), bb=-b+(affine?.0015:0), cc=b, dd=a-(affine?.001:0);
    std::vector<std::array<double,4>> truth;
    for (int row=0;row<8;++row) for(int col=0;col<10;++col) {
        // Irregular integer spacing removes the ambiguous lattice translations,
        // while retaining a common subpixel phase to expose centroid bias.
        const double rx=35+col*61+(int(rng()%21)-10)+.33+.03*std::sin(col*3+row);
        const double ry=30+row*60+(int(rng()%21)-10)+.37+.04*std::cos(col+row*2);
        const double mx=(dd*(rx-dx)-bb*(ry-dy))/(aa*dd-bb*cc);
        const double my=(-cc*(rx-dx)+aa*(ry-dy))/(aa*dd-bb*cc);
        const double width=1.+.07*((row+col)%7),amp=1000+31*((row*3+col)%11);
        addStar(ref,rx,ry,width,width*1.15,.1,amp);
        addStar(moving,mx,my,width*1.2,width*1.3,-.2,amp*.8);
        truth.push_back({rx,ry,mx,my});
    }
    RegistrationOptions opts;opts.minimumMatches=20;
    auto coarse=Registration{}.estimateSimilarity(ref,moving,opts);
    opts.refineSimilarityCentroids=true;
    auto refined=Registration{}.estimateSimilarity(ref,moving,opts);
    auto selected=Registration{}.estimateAffine(ref,moving,opts);
    require(coarse.ok&&refined.ok&&refined.usedCentroidRefinement,"Centroid registration did not refine");
    const auto rms=[&](const RegistrationResult& result) {
        const auto& t=result.affine; double sum=0;
        for (auto p:truth) {
            const double xx=t.a*p[2]+t.b*p[3]+t.dx;
            const double yy=t.c*p[2]+t.d*p[3]+t.dy;
            sum+=(xx-p[0])*(xx-p[0])+(yy-p[1])*(yy-p[1]);
        }
        return std::sqrt(sum/truth.size());
    };
    std::cout<<"Registration truth (affine="<<affine<<", seed="<<seed<<") RMS: "<<rms(coarse)<<" -> "<<rms(refined)
             <<" -> "<<rms(selected)<<" px, CV ratio "<<selected.centroidAffineValidationRatio<<"\n";
    require(selected.ok && selected.usedCentroidRefinement,"Adaptive affine registration failed");
    require(selected.usedCentroidAffine==affine,"Incorrect geometry model selected");
    require(rms(selected)<.01 && rms(selected)<rms(coarse)*.2,"Registration precision did not improve");
    if (affine) require(rms(selected)<rms(refined)*.05,"True affine geometry was not recovered");
    else require(selected.affine.a==refined.affine.a && selected.affine.b==refined.affine.b &&
                 selected.affine.c==refined.affine.c && selected.affine.d==refined.affine.d &&
                 selected.affine.dx==refined.affine.dx && selected.affine.dy==refined.affine.dy,
                 "Retained similarity transform was changed");
    // Too few suitable sources must explicitly retain the coarse solution.
    auto tiny=blank(50,50);
    for(auto xy: {std::array<double,2>{12,12},{35,12},{23,35}}) addStar(tiny,xy[0],xy[1],1,1,0,1000);
    opts.minimumMatches=3;
    auto fallback=Registration{}.estimateSimilarity(tiny,tiny,opts);
    require(fallback.ok&&!fallback.usedCentroidRefinement,"Sparse-field refinement fallback failed");
    fallback=Registration{}.estimateAffine(tiny,tiny,opts);
    require(fallback.ok&&!fallback.usedCentroidRefinement&&!fallback.usedCentroidAffine,
            "Sparse affine fallback failed");
    // Collinear support must not produce an apparently precise 2D correction.
    auto line=blank(640,480);
    for (int j=0;j<20;++j) addStar(line,30+29*j,240,1,1,0,1000+10*j);
    fallback=Registration{}.estimateAffine(line,line,opts);
    require(fallback.ok&&!fallback.usedCentroidRefinement&&!fallback.usedCentroidAffine,
            "Collinear geometry was accepted for native refinement");
}
int main(){
    testCentroidTruth(); testBadSources();
    for (unsigned seed : {73,107,233}) { testRegistrationTruth(false,seed); testRegistrationTruth(true,seed); }
}
