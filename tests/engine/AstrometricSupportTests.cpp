#include "photonstack/AstrometricValidation.hpp"
#include "photonstack/FitsNumericTable.hpp"
#include "photonstack/FitsCodec.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace photonstack;namespace fs=std::filesystem;
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
template<class F> void rejects(F f,const char* reason){bool failed=false;try{f();}catch(const std::exception&){failed=true;}require(failed,reason);}
std::string bytes(const fs::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
std::string card(std::string key,const std::string& value){key.resize(8,' ');key+="= "+value;key.resize(80,' ');return key;}
int main(){const auto root=fs::temp_directory_path()/("photonstack-astrometry-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));try{
    fs::create_directory(root);
    const FitsNumericTable columns{{"X",{1,2.25,-3}},{"Y",{3,5,7}},{"FLUX",{1e10,1e-10,-.5}}};
    writeFitsNumericTable(root/"stars.fits",columns);require(readFitsNumericTable(root/"stars.fits",{"X","Y","FLUX"})==columns,"table round trip");
    rejects([&]{writeFitsNumericTable(root/"stars.fits",columns);},"replaced existing table");
    rejects([&]{readFitsNumericTable(root/"stars.fits",{"missing"});},"missing column accepted");
    auto broken=bytes(root/"stars.fits");broken.resize(5760+5);std::ofstream(root/"short.fits",std::ios::binary)<<broken;
    rejects([&]{readFitsNumericTable(root/"short.fits",{"X"});},"truncated table accepted");
    SolvedPanel sky{{101,121,359.9,35,{-0.001,.00002,.00001,.001}},{{2,0,2e-6,-1e-6},{1,1,-1e-6,2e-6}}};
    validateAstrometricGrid(sky,200,240);
    auto bad=sky;bad.distortion={{2,0,.02,0}};rejects([&]{validateAstrometricGrid(bad,200,240);},"folded SIP accepted");
    for(double y:{0.,100.,239.})for(double x:{0.,100.,199.}){const auto world=pixelToSky(sky,x,y);const auto pixel=SkyProjection(sky,{1,1,world.x,world.y,{1,0,0,1}}).toPanel(0,0);require(std::hypot(pixel.x-x,pixel.y-y)<1e-6,"sky pixel round trip");}
    // Preserve signed scaled integer samples, negative values, HISTORY and a
    // following extension byte-for-byte. Remove stale coordinate/checksum cards.
    std::string header=card("SIMPLE","T")+card("BITPIX","16")+card("NAXIS","2")+card("NAXIS1","200")+card("NAXIS2","240")+
        card("BSCALE","2.5")+card("BZERO","-100")+card("EXPTIME","180")+card("DATE-OBS","'2026-09-09T21:12:10'")+card("FILTER","'Ha'")+
        card("CTYPE1","'OLD'")+card("CTYPE1A","'OLDALT'")+card("AP_ORDER","5")+card("CHECKSUM","'invalid'");
    std::string history="HISTORY acquisition preserved";history.resize(80,' ');header+=history;header+="END";header.resize((header.size()+2879)/2880*2880,' ');
    std::string payload(200*240*2,'\0');for(std::size_t i=0;i<payload.size();++i)payload[i]=char(i%256);payload.resize((payload.size()+2879)/2880*2880,'\0');
    std::string extension=card("XTENSION","'IMAGE'")+card("BITPIX","8")+card("NAXIS","0")+card("PCOUNT","0")+card("GCOUNT","1")+"END";extension.resize(2880,' ');payload+=extension;
    std::ofstream(root/"source.fits",std::ios::binary)<<header<<payload;
    copyFitsWithWcs(root/"source.fits",root/"solved.fits",sky);const auto saved=bytes(root/"solved.fits");
    require(saved.substr(saved.size()-payload.size())==payload,"FITS payload or extensions changed");
    for(const auto& key:{card("EXPTIME","180"),card("DATE-OBS","'2026-09-09T21:12:10'"),card("FILTER","'Ha'"),history})require(saved.find(key)!=saved.npos,"acquisition cards changed");
    require(saved.find("OLDALT")==saved.npos&&saved.find("AP_ORDER")==saved.npos&&saved.find("CHECKSUM")==saved.npos,"stale WCS retained");
    require(readSolvedPanel(root/"solved.fits").grid.ra==sky.grid.ra,"new WCS missing");
    require(bytes(root/"source.fits")==header+payload,"original file changed");
    rejects([&]{copyFitsWithWcs(root/"source.fits",root/"solved.fits",sky);},"existing WCS output overwritten");
    rejects([&]{copyFitsWithWcs(root/"source.fits",root/"source.fits",sky);},"source overwritten");
    auto invalid=sky;invalid.grid.cd={0,0,0,0};rejects([&]{copyFitsWithWcs(root/"source.fits",root/"invalid.fits",invalid);},"singular output WCS accepted");
    require(!fs::exists(root/"invalid.fits"),"invalid WCS left a product");
    fs::remove_all(root);std::cout<<"Astrometric table, geometry and lossless WCS attachment passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<" at "<<root<<'\n';return 1;}}
