#include "CalibrationMetadataCommand.hpp"
#include "ChannelRecipe.hpp"
#include "NewProductDirectory.hpp"
#include "photonstack/FitsCodec.hpp"
#include <iostream>

int runAcquisitionDeclaration(const std::vector<std::string>& args) {
    using namespace photonstack;
    using namespace photonstack::cli;
    namespace fs = std::filesystem;
    if (args.size() != 5 || args[1] != "--manifest" || args[3] != "--output-dir")
        throw std::invalid_argument("Use calibration-declare --manifest declarations.txt --output-dir new-folder");
    const fs::path manifest = args[2];
    if (fs::file_size(manifest) > 1024 * 1024) throw std::invalid_argument("Declaration manifest exceeds 1 MiB");
    const auto manifestSHA = fileSHA256(manifest);
    std::ifstream in(manifest); std::string line, reason;
    if (!std::getline(in, line) || line != "PhotonStack-AcquisitionDeclaration-1") throw std::invalid_argument("Invalid declaration version");
    std::map<std::string, std::string> fields;
    std::vector<ChannelSource> sources;
    std::set<std::string> hashes;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream row(line); std::string key, name, value, extra;
        row >> key;
        if (key == "reason") {
            if (!reason.empty() || !(row >> std::quoted(value)) || value.empty() || value.size() > 1024) throw std::invalid_argument("Declare a reason once");
            reason = value;
        } else if (key == "set") {
            if (!(row >> name >> std::quoted(value)) || !fields.emplace(name, value).second || fields.size() > 32) throw std::invalid_argument("Invalid or repeated declaration");
        } else if (key == "source") {
            if (!(row >> std::quoted(name) >> value) || !channelSHA(value) || sources.size() >= 4096) throw std::invalid_argument("Declare source path and expected SHA256");
            const auto path = fs::canonical(name);
            if (!isFitsPath(path) || !fs::is_regular_file(path) || fileSHA256(path) != value || !hashes.insert(value).second)
                throw std::invalid_argument("Source changed or duplicate input");
            sources.push_back({"", path, value});
        } else throw std::invalid_argument("Unknown declaration field");
        if (row >> extra) throw std::invalid_argument("Unexpected declaration content");
    }
    if (!in.eof() || reason.empty() || sources.empty() || fields.empty() || fileSHA256(manifest) != manifestSHA)
        throw std::invalid_argument("Incomplete or changed declaration manifest");
    NewProductDirectory product(args[4]);
    std::ostringstream report;
    report << "{\"type\":\"complete\",\"command\":\"calibration-declare\",\"schema\":1,\"manualDeclarations\":true,\"reason\":" << channelJSON(reason)
           << ",\"manifestSHA\":" << channelJSON(manifestSHA) << ",\"frames\":[";
    for (std::size_t i = 0; i < sources.size(); ++i) {
        auto& source = sources[i];
        std::ostringstream name; name << "frame-" << std::setfill('0') << std::setw(6) << i + 1 << ".fits";
        source.name = name.str();
        copyFitsWithAcquisition(source.path, product.stage / source.name, fields);
        if (fileSHA256(source.path) != source.sha) throw std::runtime_error("Source changed during declaration copy");
        report << (i ? "," : "") << "{\"source\":" << channelJSON(source.path.string()) << ",\"sourceSHA\":" << channelJSON(source.sha)
               << ",\"output\":" << channelJSON(source.name) << ",\"outputSHA\":" << channelJSON(fileSHA256(product.stage / source.name)) << '}';
    }
    report << "],\"declarations\":{";
    bool first = true;
    for (const auto& [key, value] : fields) { report << (first ? "" : ",") << channelJSON(key) << ':' << channelJSON(value); first = false; }
    report << "}}\n";
    fs::copy_file(manifest, product.stage / "declarations.txt");
    if (fileSHA256(product.stage / "declarations.txt") != manifestSHA) throw std::runtime_error("Declaration changed during copying");
    std::ofstream output(product.stage / "declarations.json"); output << report.str(); output.close();
    if (!output) throw std::runtime_error("Cannot write declaration record");
    product.publish(); std::cout << report.str(); return 0;
}
