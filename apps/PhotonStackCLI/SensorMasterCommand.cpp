#include "SensorMasterCommand.hpp"
#include "ChannelRecipe.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/SensorMasterBuilder.hpp"
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <system_error>
#include <unistd.h>
#ifdef __linux__
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace {
using namespace photonstack;
using namespace photonstack::cli;
namespace fs = std::filesystem;
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) {
    interrupted = 1;
}
struct Signals {
    struct sigaction oldInt{}, oldTerm{};
    Signals() {
        interrupted = 0;
        struct sigaction a{};
        a.sa_handler = interrupt;
        sigemptyset(&a.sa_mask);
        sigaction(SIGINT, &a, &oldInt);
        sigaction(SIGTERM, &a, &oldTerm);
    }
    ~Signals() {
        sigaction(SIGINT, &oldInt, nullptr);
        sigaction(SIGTERM, &oldTerm, nullptr);
    }
};
struct Recipe {
    SensorMasterOptions options;
    std::vector<SensorMasterSource> frames;
    std::map<std::string, std::string> products;
};
void check() {
    if (interrupted)
        throw std::runtime_error("Sensor master construction cancelled");
}
double number(const std::string& s) {
    std::size_t n = 0;
    const auto v = std::stod(s, &n);
    if (n != s.size() || !std::isfinite(v))
        throw std::invalid_argument("Invalid numeric option");
    return v;
}
std::size_t integer(const std::string& s, std::size_t maximum) {
    const auto v = number(s);
    if (v < 1 || v > double(maximum) || std::floor(v) != v)
        throw std::invalid_argument("Integer option is out of range");
    return std::size_t(v);
}
Recipe read(const fs::path& path, bool saved) {
    if (fs::file_size(path) > 1024 * 1024)
        throw std::invalid_argument("Sensor manifest exceeds 1 MiB");
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line) || line != "PhotonStack-SensorMaster-1")
        throw std::invalid_argument("Invalid sensor manifest version");
    Recipe r;
    std::set<std::string> settings;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        std::istringstream row(line);
        std::string key, value, extra;
        row >> key;
        if (key == "frame" || key == "bias" || key == "dark-flat") {
            SensorMasterSource source;
            std::string pathText;
            if (!(row >> std::quoted(pathText) >> source.sha) ||
                pathText.find_first_of("\r\n\0", 0, 3) != std::string::npos)
                throw std::invalid_argument("Invalid sensor source record");
            source.path = pathText;
            if (!source.path.is_absolute() || (!channelSHA(source.sha) && (saved || source.sha != "-")))
                throw std::invalid_argument("Source requires an absolute path and SHA-256");
            if (source.sha == "-")
                source.sha.clear();
            if (key == "frame") {
                if (r.frames.size() >= 256)
                    throw std::invalid_argument("Too many sensor inputs");
                r.frames.push_back(source);
            } else {
                if (!settings.insert(key).second)
                    throw std::invalid_argument("Duplicate correction master");
                if (key == "bias")
                    r.options.bias = source;
                else {
                    r.options.darkFlat = source;
                    if (!(row >> r.options.darkFlatBiasState))
                        throw std::invalid_argument("Declare dark-flat included/removed bias state");
                }
            }
        } else if (key == "product") {
            std::string sha;
            if (!saved || !(row >> value >> sha) ||
                !std::set<std::string>{"master.fits", "accepted.fits", "inputs.txt", "report.json"}.contains(value) ||
                !channelSHA(sha) || !r.products.emplace(value, sha).second)
                throw std::invalid_argument("Invalid master product record");
        } else {
            if (!(row >> value) || !settings.insert(key).second)
                throw std::invalid_argument("Duplicate or incomplete sensor option");
            if (key == "role")
                r.options.role = value;
            else if (key == "method")
                r.options.method = value;
            else if (key == "temperature-tolerance")
                r.options.temperatureTolerance = number(value);
            else if (key == "minimum-samples")
                r.options.minimumSamples = integer(value, 256);
            else if (key == "memory-mib")
                r.options.memoryBytes = integer(value, 8192) * 1024 * 1024;
            else if (key == "tile-pixels")
                r.options.tilePixels = integer(value, 1048576);
            else if (key == "threads")
                r.options.workers = integer(value, 32);
            else
                throw std::invalid_argument("Unknown sensor option: " + key);
        }
        if (row >> extra)
            throw std::invalid_argument("Unexpected sensor record suffix");
    }
    if (!in.eof() || !settings.contains("role") || r.frames.size() < 2 || (saved && r.products.size() != 4))
        throw std::invalid_argument("Incomplete sensor recipe");
    return r;
}
void bind(SensorMasterSource& s) {
    check();
    s.path = fs::canonical(s.path);
    const auto sha = fileSHA256(s.path);
    if (!s.sha.empty() && s.sha != sha)
        throw std::runtime_error("Sensor source changed: " + s.path.string());
    s.sha = sha;
}
void bind(Recipe& r) {
    for (auto& s : r.frames)
        bind(s);
    if (r.options.bias)
        bind(*r.options.bias);
    if (r.options.darkFlat)
        bind(*r.options.darkFlat);
}
void save(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.close();
    if (!out)
        throw std::runtime_error("Cannot save sensor product");
}
std::string recipeText(const Recipe& r) {
    const auto& o = r.options;
    std::ostringstream out;
    out << std::setprecision(17) << "PhotonStack-SensorMaster-1\nrole " << o.role << "\nmethod " << o.method
        << "\ntemperature-tolerance " << o.temperatureTolerance << "\nminimum-samples " << o.minimumSamples
        << "\nmemory-mib " << o.memoryBytes / (1024 * 1024) << "\ntile-pixels " << o.tilePixels << "\nthreads "
        << o.workers << '\n';
    auto source = [&](const std::string& kind, const SensorMasterSource& s, const std::string& suffix = "") {
        out << kind << ' ' << std::quoted(s.path.string()) << ' ' << s.sha << suffix << '\n';
    };
    for (const auto& s : r.frames)
        source("frame", s);
    if (o.bias)
        source("bias", *o.bias);
    if (o.darkFlat)
        source("dark-flat", *o.darkFlat, " " + o.darkFlatBiasState);
    for (const auto& [name, sha] : r.products)
        out << "product " << name << ' ' << sha << '\n';
    return out.str();
}
std::string report(const SensorMasterResult& r, const Recipe& recipe) {
    std::ostringstream out;
    out << std::setprecision(17)
        << "{\"schema\":1,\"command\":\"sensor-master-build\",\"domain\":\"sensor-linear\",\"role\":"
        << channelJSON(r.role) << ",\"biasState\":" << channelJSON(r.biasState)
        << ",\"method\":" << channelJSON(r.method) << ",\"width\":" << r.master.width
        << ",\"height\":" << r.master.height << ",\"sampling\":" << channelJSON(r.acquisition.text.at("sampling"))
        << ",\"minimumSamples\":" << recipe.options.minimumSamples << ",\"invalidPixels\":" << r.invalidPixels
        << ",\"missingSamples\":" << r.missingSamples << ",\"workingBytes\":" << r.workingBytes
        << ",\"tilePixels\":" << r.tilePixels << ",\"workers\":" << r.workers
        << ",\"temperatureTolerance\":" << recipe.options.temperatureTolerance
        << ",\"temperatureMin\":" << r.acquisition.numeric.at("sensorTemperatureMin")
        << ",\"temperatureMax\":" << r.acquisition.numeric.at("sensorTemperatureMax") << ",\"normalization\":"
        << channelJSON(r.role == "flat" ? "corrected-phase-positive-mean-to-first-frame" : "none") << ",\"sources\":[";
    auto source = [&](const SensorMasterSource& s) {
        out << "{\"path\":" << channelJSON(s.path.string()) << ",\"sha\":" << channelJSON(s.sha) << '}';
    };
    for (std::size_t i = 0; i < r.sources.size(); ++i) {
        if (i)
            out << ',';
        source(r.sources[i]);
    }
    out << "],\"corrections\":{";
    if (recipe.options.bias) {
        out << "\"bias\":";
        source(*recipe.options.bias);
    }
    if (recipe.options.darkFlat) {
        if (recipe.options.bias)
            out << ',';
        out << "\"darkFlat\":";
        source(*recipe.options.darkFlat);
        out << ",\"darkFlatBiasState\":" << channelJSON(recipe.options.darkFlatBiasState);
    }
    out << "},\"factors\":[";
    for (std::size_t i = 0; i < r.normalization.size(); ++i) {
        if (i)
            out << ',';
        out << '[';
        for (unsigned p = 0; p < 4; ++p) {
            if (p)
                out << ',';
            out << r.normalization[i][p];
        }
        out << ']';
    }
    out << "],\"products\":{";
    bool first = true;
    for (const auto& [name, sha] : recipe.products) {
        out << (first ? "" : ",") << channelJSON(name) << ':' << channelJSON(sha);
        first = false;
    }
    out << "}}\n";
    return out.str();
}
void publish(const fs::path& stage, const fs::path& output) {
#ifdef __APPLE__
    const int status = renamex_np(stage.c_str(), output.c_str(), RENAME_EXCL);
#elif defined(__linux__)
    const int status = syscall(SYS_renameat2, AT_FDCWD, stage.c_str(), AT_FDCWD, output.c_str(), RENAME_NOREPLACE);
#else
    throw std::runtime_error("Atomic sensor master publication is unavailable on this platform");
#endif
#if defined(__APPLE__) || defined(__linux__)
    if (status)
        throw std::system_error(errno, std::generic_category(), "Cannot publish to new sensor output directory");
#endif
}
} // namespace
int runSensorMaster(const std::vector<std::string>& args) {
    Signals signals;
    const bool fresh = args[0] == "sensor-master-build", inspect = args[0] == "sensor-master-inspect";
    std::map<std::string, std::string> options;
    for (std::size_t i = 1; i < args.size(); i += 2) {
        if (i + 1 >= args.size() || !options.emplace(args[i], args[i + 1]).second)
            throw std::invalid_argument("Incomplete or repeated sensor option");
    }
    for (const auto& [key, value] : options)
        if (key != (fresh ? "--manifest" : "--recipe") && key != "--output-dir" &&
            (inspect || (key != "--threads" && key != "--memory-mib" && key != "--tile-pixels")))
            throw std::invalid_argument("Unknown sensor option: " + key);
    const auto source = options.find(fresh ? "--manifest" : "--recipe");
    if (source == options.end())
        throw std::invalid_argument("Specify a sensor manifest or saved recipe");
    const fs::path recipePath = fs::canonical(source->second);
    const auto recipeSHA = fileSHA256(recipePath);
    auto r = read(recipePath, !fresh);
    bind(r);
    if (fileSHA256(recipePath) != recipeSHA)
        throw std::runtime_error("Sensor recipe changed while reading");
    if (!fresh)
        for (const auto& [name, sha] : r.products)
            if (fileSHA256(recipePath.parent_path() / name) != sha)
                throw std::runtime_error("Saved sensor product changed: " + name);
    if (!fresh) {
        auto config = r;
        config.products.clear();
        if (fs::file_size(recipePath.parent_path() / "inputs.txt") > 1024 * 1024 ||
            fs::file_size(recipePath.parent_path() / "report.json") > 2 * 1024 * 1024)
            throw std::invalid_argument("Saved sensor record exceeds size bound");
        std::ifstream input(recipePath.parent_path() / "inputs.txt");
        const std::string text{std::istreambuf_iterator<char>(input), {}};
        if (text != recipeText(config))
            throw std::runtime_error("Saved sensor configuration changed");
    }
    if (inspect) {
        if (options.contains("--output-dir"))
            throw std::invalid_argument("Inspect does not write output");
        std::ifstream in(recipePath.parent_path() / "report.json");
        std::cout << in.rdbuf();
        return 0;
    }
    if (!options.contains("--output-dir"))
        throw std::invalid_argument("Choose a new output directory");
    const fs::path output = fs::absolute(options.at("--output-dir")).lexically_normal();
    if (fs::exists(output) || fs::is_symlink(output) || !fs::is_directory(output.parent_path()))
        throw std::invalid_argument("Choose a new output directory with an existing parent");
    if (options.contains("--threads"))
        r.options.workers = integer(options.at("--threads"), 32);
    if (options.contains("--memory-mib"))
        r.options.memoryBytes = integer(options.at("--memory-mib"), 8192) * 1024 * 1024;
    if (options.contains("--tile-pixels"))
        r.options.tilePixels = integer(options.at("--tile-pixels"), 1048576);
    r.options.checkCancelled = check;
    const auto begin = std::chrono::steady_clock::now();
    const auto built = buildSensorMaster(r.frames, r.options);
    r.frames = built.sources;
    std::string pattern = (output.parent_path() / ".sensor-master-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back(0);
    const char* path = mkdtemp(buffer.data());
    if (!path)
        throw std::runtime_error("Cannot create master staging directory");
    const fs::path stage = path;
    try {
        check();
        writeSensorMaster(built, stage / "master.fits");
        const auto written = FitsCodec().write(built.accepted, stage / "accepted.fits");
        if (!written.ok)
            throw std::runtime_error(written.message);
        r.products.clear();
        save(stage / "inputs.txt", recipeText(r));
        for (const auto* name : {"master.fits", "accepted.fits", "inputs.txt"})
            r.products[name] = fileSHA256(stage / name);
        save(stage / "report.json", report(built, r));
        r.products["report.json"] = fileSHA256(stage / "report.json");
        save(stage / "recipe.txt", recipeText(r));
        bind(r);
        check();
        publish(stage, output);
    } catch (...) {
        std::error_code ec;
        fs::remove_all(stage, ec);
        throw;
    }
    std::cout << "{\"type\":\"complete\",\"command\":\"sensor-master-build\",\"directory\":"
              << channelJSON(output.string()) << ",\"frames\":" << built.sources.size()
              << ",\"invalidPixels\":" << built.invalidPixels << ",\"elapsedSeconds\":"
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() << "}\n";
    return 0;
}
