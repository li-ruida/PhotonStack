#include "photonstack/NonlocalDenoiser.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

using namespace photonstack;
void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
// Deliberately direct patch sums, independent of the production tile/integral path.
int mirror(int p, int n) {
    if (n == 1) return 0;
    while (p < 0 || p >= n) p = p < 0 ? -p : 2 * n - 2 - p;
    return p;
}
std::array<double, 3> brute(const ImageBuffer& a, int x, int y, double m, const NonlocalDenoiseOptions& o) {
    const auto pixel = [&a](int xx, int yy, int c) {
        return double(a.pixels[(mirror(yy, a.height) * a.width + mirror(xx, a.width)) * a.channels + c]);
    };
    std::array<double, 3> sum{};
    double norm = 0;
    for (int dy = -5; dy <= 5; ++dy) for (int dx = -5; dx <= 5; ++dx) {
        double distance = 0;
        for (int ky = -3; ky <= 3; ++ky) for (int kx = -3; kx <= 3; ++kx) {
            double d = 0;
            for (int c = 0; c < 3; ++c)
                d += o.luminanceWeights[c] * (pixel(x + kx, y + ky, c) - pixel(x + kx + dx, y + ky + dy, c));
            distance += d * d;
        }
        const double w = std::exp(-distance / (49 * o.h * o.h));
        for (int c = 0; c < 3; ++c) sum[c] += w * pixel(x + dx, y + dy, c);
        norm += w;
    }
    for (int c = 0; c < 3; ++c) sum[c] = pixel(x,y,c) + m * (sum[c] / norm - pixel(x,y,c));
    return sum;
}
ImageBuffer scene(int w, int h) {
    ImageBuffer a;
    a.width = w; a.height = h; a.pixels.resize(a.sampleCount(), 1);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) for (int c = 0; c < 3; ++c)
        a.pixels[(y * w + x) * 4 + c] = float((c - 1) * 1000 + 7 * std::sin(x * .9 + y * .37 + c) + .1 * x);
    return a;
}
ImageBuffer mapFor(const ImageBuffer& a) {
    ImageBuffer m;
    m.width = a.width; m.height = a.height; m.channels = 1; m.format = PixelFormat::Float32Gray;
    m.pixels.resize(m.sampleCount());
    for (std::size_t i = 0; i < m.pixelCount(); ++i) m.pixels[i] = i % 3 ? .4F : 0;
    return m;
}
// Whole-image undirected graph with direct 49-sample patch sums. No tiles or
// integral images; catches degree halos, edges, protected holes and joins.
ImageBuffer bruteConservative(const ImageBuffer& a, const ImageBuffer& mask, const NonlocalDenoiseOptions& o) {
    struct Edge { std::size_t i, j; double weight; };
    std::vector<Edge> edges;
    std::vector<double> degree(a.pixelCount(),1);
    std::vector<std::array<double,3>> values(a.pixelCount());
    const double ws=o.luminanceWeights[0]+o.luminanceWeights[1]+o.luminanceWeights[2];
    const auto sample=[&](int x,int y,int c) { return double(a.pixels[(mirror(y,a.height)*a.width+mirror(x,a.width))*4+c]); };
    for (std::size_t i=0;i<a.pixelCount();++i) for(int c=0;c<3;++c) values[i][c]=a.pixels[i*4+c];
    for(int y=0;y<int(a.height);++y) for(int x=0;x<int(a.width);++x)
        for(int dy=0;dy<=5;++dy) for(int dx=-5;dx<=5;++dx) {
            if((dy==0&&dx<=0)||x+dx<0||x+dx>=int(a.width)||y+dy>=int(a.height))continue;
            double d=0;
            for(int py=-3;py<=3;++py)for(int px=-3;px<=3;++px)for(int c=0;c<3;++c) {
                const double delta=sample(x+px+dx,y+py+dy,c)-sample(x+px,y+py,c);
                d+=ws*o.luminanceWeights[c]*delta*delta;
            }
            const double weight=std::exp(-d/(49*o.h*o.h));
            const auto i=std::size_t(y)*a.width+x,j=std::size_t(y+dy)*a.width+x+dx;
            edges.push_back({i,j,weight});degree[i]+=weight;degree[j]+=weight;
        }
    for(const auto& e:edges) {
        const double q=std::min(mask.pixels[e.i],mask.pixels[e.j])*e.weight/std::max(degree[e.i],degree[e.j]);
        for(int c=0;c<3;++c) {
            const double d=q*(double(a.pixels[e.j*4+c])-a.pixels[e.i*4+c]);values[e.i][c]+=d;values[e.j][c]-=d;
        }
    }
    auto out=a;
    for(std::size_t i=0;i<a.pixelCount();++i)for(int c=0;c<3;++c)out.pixels[i*4+c]=float(values[i][c]);
    return out;
}
int main() {
    const NonlocalDenoiser filter;
    NonlocalDenoiseOptions options;
    options.h = 4.3;
    options.luminanceWeights = {.138, .7152, .055};
    for (const auto dimensions : {std::array{1,1}, std::array{1,9}, std::array{9,1}, std::array{11,13}, std::array{131,133}}) {
        const auto a = scene(dimensions[0], dimensions[1]), mask = mapFor(a);
        const auto result = filter.apply(a, mask, options);
        require(result.ok, "valid scientific RGB");
        for (std::size_t p = 0; p < a.pixelCount(); ++p) {
            require(result.image.pixels[p*4+3] == a.pixels[p*4+3], "alpha unchanged");
            for (int c = 0; c < 3; ++c)
                if (mask.pixels[p] == 0) require(result.image.pixels[p*4+c] == a.pixels[p*4+c], "protected sample exact");
        }
        for (int y : {0, int(a.height/2), int(a.height)-1, std::min(127,int(a.height)-1), std::min(128,int(a.height)-1)})
            for (int x : {0, int(a.width/2), int(a.width)-1, std::min(127,int(a.width)-1), std::min(128,int(a.width)-1)}) {
                const auto p = y * a.width + x;
                const auto expected = brute(a, x, y, mask.pixels[p], options);
                for (int c = 0; c < 3; ++c)
                    require(std::abs(result.image.pixels[p*4+c]-expected[c]) < .00007, "matches brute force at edges and tile joins");
            }
        auto zero = options; zero.h = 0;
        require(filter.apply(a,mask,zero).image.pixels == a.pixels, "zero h exact identity");
    }
    auto a = scene(9,8), mask = mapFor(a);
    auto flat = a;
    for (std::size_t p = 0; p < a.pixelCount(); ++p) {
        flat.pixels[p*4] = -2; flat.pixels[p*4+1] = 300; flat.pixels[p*4+2] = 70000;
    }
    require(filter.apply(flat,mask,options).image.pixels == flat.pixels, "constant colors and scientific range preserved");
    auto scaled = a; auto scaledOptions = options;
    for (std::size_t p = 0; p < a.pixelCount(); ++p) for (int c = 0; c < 3; ++c) scaled.pixels[p*4+c] *= 2;
    scaledOptions.h *= 2;
    const auto original = filter.apply(a,mask,options), twice = filter.apply(scaled,mask,scaledOptions);
    require(original.ok && twice.ok, "scaled input valid");
    for (std::size_t p = 0; p < a.pixelCount(); ++p) for (int c = 0; c < 3; ++c)
        require(twice.image.pixels[p*4+c] == original.image.pixels[p*4+c] * 2, "physical scale covariance");
    auto conservative=options;conservative.mode=NonlocalDenoiseMode::ConservativeRGB;
    for(const auto dim:{std::array{1,1},std::array{1,9},std::array{9,1},std::array{11,13},std::array{139,17},std::array{17,139}}) {
        const auto input=scene(dim[0],dim[1]), blend=mapFor(input);
        const auto output=filter.apply(input,blend,conservative);
        require(output.ok,"conservative RGB accepts valid input");
        const auto expected=bruteConservative(input,blend,conservative);
        std::array<double,3> sums{},roundingBounds{};
        for(std::size_t i=0;i<input.pixelCount();++i) {
            require(output.image.pixels[i*4+3]==1,"conservative alpha exact");
            for(int c=0;c<3;++c) {
                const float value=output.image.pixels[i*4+c];
                require(std::abs(value-expected.pixels[i*4+c])<.00007,"conservative matches whole-image direct graph at borders and joins");
                if(blend.pixels[i]==0)require(value==input.pixels[i*4+c],"conservative protection exact");
                sums[c]+=double(value)-input.pixels[i*4+c];
                roundingBounds[c]+=.51*std::max(double(std::nextafter(value,std::numeric_limits<float>::infinity()))-value,
                                               double(value)-std::nextafter(value,-std::numeric_limits<float>::infinity()));
            }
        }
        for(int c=0;c<3;++c)require(std::abs(sums[c])<=roundingBounds[c]+1e-8,"conservative channel sum within float rounding bound");
        auto bypass=conservative;bypass.h=0;
        require(filter.apply(input,blend,bypass).image.pixels==input.pixels,"conservative zero h exact");
    }
    require(filter.apply(flat,mask,conservative).image.pixels==flat.pixels,"conservative constant scientific colors exact");
    auto invalidMode=options;invalidMode.mode=static_cast<NonlocalDenoiseMode>(99);
    require(!filter.apply(a,mask,invalidMode).ok,"reject invalid mode");
    auto bad = options; bad.h = std::numeric_limits<double>::quiet_NaN();
    require(!filter.apply(a,mask,bad).ok, "reject NaN h");
    bad = options; bad.h = -1; require(!filter.apply(a,mask,bad).ok, "reject negative h");
    bad = options; bad.luminanceWeights = {0,0,0}; require(!filter.apply(a,mask,bad).ok, "reject zero weights");
    bad = options; bad.luminanceWeights[0] = -1; require(!filter.apply(a,mask,bad).ok, "reject negative weights");
    auto badMap = mask; badMap.pixels[0] = 1.1F; require(!filter.apply(a,badMap,options).ok, "reject bad blend range");
    badMap = mask; badMap.width += 1; require(!filter.apply(a,badMap,options).ok, "reject blend shape");
    auto colorMap = scene(a.width,a.height); require(!filter.apply(a,colorMap,options).ok, "reject color mask");
    auto badImage = a; badImage.pixels[3] = 0; require(!filter.apply(badImage,mask,options).ok, "reject incomplete coverage");
    badImage = a; badImage.pixels[0] = std::numeric_limits<float>::infinity();
    require(!filter.apply(badImage,mask,options).ok, "reject nonfinite scientific data");
    std::cout << "Nonlocal scientific denoising checks passed\n";
}
