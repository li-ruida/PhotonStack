#include "CalibrationJobCommand.hpp"
#include "ChannelRecipe.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/SensorCalibration.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace {
using namespace photonstack;
using namespace photonstack::cli;
namespace fs = std::filesystem;
volatile std::sig_atomic_t stopped = 0;
std::string activeCommand;
void progress(const std::string& stage, double fraction, std::size_t step, std::size_t total) {
    const char* enabled = std::getenv("PHOTONSTACK_PROGRESS");
    if (!enabled || std::string(enabled) != "1")
        return;
    std::cout << "{\"type\":\"progress\",\"task\":" << channelJSON(activeCommand)
              << ",\"command\":" << channelJSON(activeCommand) << ",\"stage\":" << channelJSON(stage)
              << ",\"progress\":" << fraction << ",\"step\":" << step << ",\"total\":" << total << "}\n"
              << std::flush;
}
void signalStop(int) {
    stopped = 1;
}
void check() {
    if (stopped)
        throw std::runtime_error("Calibration job cancelled; completed frames can be resumed");
}
struct Signals {
    struct sigaction oldInt{}, oldTerm{};
    Signals() {
        stopped = 0;
        struct sigaction a{};
        a.sa_handler = signalStop;
        sigemptyset(&a.sa_mask);
        sigaction(SIGINT, &a, &oldInt);
        sigaction(SIGTERM, &a, &oldTerm);
    }
    ~Signals() {
        sigaction(SIGINT, &oldInt, nullptr);
        sigaction(SIGTERM, &oldTerm, nullptr);
    }
};
void require(bool ok, const std::string& why) {
    if (!ok)
        throw std::invalid_argument(why);
}
bool roleOK(const std::string& role) {
    return role == "bias" || role == "dark" || role == "flat";
}
void regular(const fs::path& p) {
    require(fs::is_regular_file(p) && !fs::is_symlink(p), "Missing or linked job file: " + p.string());
}
void directory(const fs::path& p) {
    require(fs::is_directory(p) && !fs::is_symlink(p), "Missing or linked job directory: " + p.string());
}
std::string readText(const fs::path& p, std::size_t limit) {
    regular(p);
    require(fs::file_size(p) <= limit, "Job record exceeds size limit");
    std::ifstream in(p);
    std::string text(limit + 1, '\0');
    in.read(text.data(), std::streamsize(text.size()));
    text.resize(std::size_t(in.gcount()));
    require(!in.bad() && text.size() <= limit, "Cannot read bounded job record");
    return text;
}
void writeText(const fs::path& p, const std::string& text) {
    auto* file = std::fopen(p.c_str(), "wbx");
    require(file, "Choose a new job record");
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size() && std::fflush(file) == 0 &&
                    fsync(fileno(file)) == 0;
    const int closed = std::fclose(file);
    require(ok && !closed, "Cannot persist job record");
}
void publish(const fs::path& from, const fs::path& to) {
#ifdef __APPLE__
    const int status = renamex_np(from.c_str(), to.c_str(), RENAME_EXCL);
#elif defined(__linux__)
    const int status = int(syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE));
#else
#error Calibration job publication requires exclusive rename
#endif
    if (status)
        throw std::system_error(errno, std::generic_category(), "Cannot publish new calibration product");
}
struct Stage {
    fs::path path;
    explicit Stage(const fs::path& parent) {
        auto pattern = (parent / ".calibration-stage-XXXXXX").string();
        std::vector<char> data(pattern.begin(), pattern.end());
        data.push_back(0);
        const auto* p = mkdtemp(data.data());
        if (!p)
            throw std::runtime_error("Cannot create calibration staging directory");
        path = p;
    }
    ~Stage() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
struct Lock {
    int fd = -1;
    explicit Lock(const fs::path& root, bool readOnly) {
        fd = open((root / ".lock").c_str(), (readOnly ? O_RDONLY : O_RDWR) | O_NOFOLLOW);
        struct stat s{};
        if (fd < 0 || fstat(fd, &s) || !S_ISREG(s.st_mode) || flock(fd, (readOnly ? LOCK_SH : LOCK_EX) | LOCK_NB)) {
            if (fd >= 0)
                close(fd);
            fd = -1;
            throw std::runtime_error("Calibration job is busy or has an invalid lock");
        }
    }
    ~Lock() {
        if (fd >= 0) {
            flock(fd, LOCK_UN);
            close(fd);
        }
    }
};
struct Master {
    std::string role;
    SensorCalibrationMaster value;
};
struct Light {
    SensorMasterSource source;
    std::array<std::string, 3> choices;
    std::size_t group = 0;
};
struct Job {
    SensorCalibrationOptions options;
    std::map<std::string, Master> masters;
    std::vector<Light> lights;
};
double number(const std::string& value) {
    std::size_t n = 0;
    const double v = std::stod(value, &n);
    require(n == value.size() && std::isfinite(v), "Invalid job number");
    return v;
}
std::size_t integer(const std::string& value, std::size_t low, std::size_t high) {
    const double v = number(value);
    require(v >= double(low) && v <= double(high) && std::floor(v) == v, "Invalid job integer");
    return std::size_t(v);
}
bool identifier(const std::string& s) {
    return !s.empty() && s.size() <= 64 &&
           s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
               std::string::npos &&
           s != "-";
}
SensorMasterSource source(std::istream& row, bool saved) {
    std::string path, sha;
    require((row >> std::ws).peek() == '"' && bool(row >> std::quoted(path) >> sha) && fs::path(path).is_absolute() &&
                path.find_first_of("\r\n\0", 0, 3) == std::string::npos && (channelSHA(sha) || (!saved && sha == "-")),
            "Invalid job source");
    return {path, sha == "-" ? "" : sha};
}
Job parse(const fs::path& path, bool saved) {
    std::istringstream in(readText(path, 2 * 1024 * 1024));
    std::string line;
    require(bool(std::getline(in, line)) && line == "PhotonStack-CalibrationJob-1", "Invalid calibration job version");
    Job job;
    std::set<std::string> settings;
    std::map<std::size_t, std::size_t> groups;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        std::istringstream row(line);
        std::string key, value, extra;
        row >> key;
        if (key == "bad-pixels") {
            require(!job.options.badPixels, "Duplicate defect map setting");
            job.options.badPixels = source(row, saved);
        } else if (key == "master") {
            std::string id;
            Master master;
            require(bool(row >> id >> master.role) && identifier(id) && roleOK(master.role) &&
                        !job.masters.contains(id) && job.masters.size() < 256,
                    "Invalid or repeated calibration master");
            master.value.source = source(row, saved);
            require(bool(row >> master.value.biasState) &&
                        (master.value.biasState == "included" || master.value.biasState == "removed") &&
                        (master.role != "bias" || master.value.biasState == "included"),
                    "Declare a valid master preparation state");
            job.masters.emplace(id, master);
        } else if (key == "light") {
            require(job.lights.size() < 4096, "At most 4096 lights per calibration job");
            Light light;
            light.source = source(row, saved);
            require(bool(row >> light.choices[0] >> light.choices[1] >> light.choices[2]),
                    "Each light must select bias, dark and flat IDs or -");
            job.lights.push_back(light);
        } else if (key == "group") {
            std::string a, b;
            require(saved && bool(row >> a >> b), "Group records are generated by calibration-run");
            const auto index = integer(a, 0, 4095), group = integer(b, 0, 4095);
            require(groups.emplace(index, group).second, "Duplicate group assignment");
        } else {
            require(bool(row >> value) && settings.insert(key).second, "Missing or repeated job setting");
            if (key == "temperature-tolerance")
                job.options.temperatureTolerance = number(value);
            else if (key == "memory-mib")
                job.options.memoryBytes = integer(value, 4, 8192) * 1024 * 1024;
            else if (key == "tile-pixels")
                job.options.tilePixels = integer(value, 1, 1048576);
            else if (key == "threads")
                job.options.workers = unsigned(integer(value, 1, 32));
            else
                throw std::invalid_argument("Unknown calibration job setting");
        }
        require(!(row >> extra), "Unexpected job record fields");
    }
    require(!job.lights.empty() && !job.masters.empty() && job.options.temperatureTolerance >= 0 &&
                job.options.temperatureTolerance <= 10,
            "A calibration job needs lights, masters and a valid temperature tolerance");
    std::set<std::string> used;
    const std::array<std::string, 3> roles{"bias", "dark", "flat"};
    for (std::size_t i = 0; i < job.lights.size(); ++i) {
        auto& light = job.lights[i];
        for (std::size_t r = 0; r < 3; ++r)
            if (light.choices[r] != "-") {
                require(job.masters.contains(light.choices[r]) && job.masters.at(light.choices[r]).role == roles[r],
                        "Master choice has the wrong role or is missing");
                used.insert(light.choices[r]);
            }
        require(light.choices[0] != "-" || light.choices[1] != "-", "Each light requires bias or dark correction");
        if (saved) {
            require(groups.contains(i) && groups.at(i) < job.lights.size(), "Missing or invalid light group");
            light.group = groups.at(i);
        }
    }
    require(used.size() == job.masters.size() && (!saved || groups.size() == job.lights.size()),
            "Unused masters or invalid group count");
    return job;
}
SensorCalibrationOptions selection(const Job& job, const Light& light) {
    auto o = job.options;
    o.checkCancelled = check;
    const std::array<std::optional<SensorCalibrationMaster>*, 3> targets{&o.bias, &o.dark, &o.flat};
    for (std::size_t r = 0; r < 3; ++r)
        if (light.choices[r] != "-")
            *targets[r] = job.masters.at(light.choices[r]).value;
    return o;
}
void bind(Job& job, bool saved) {
    std::set<fs::path> paths;
    std::set<std::string> hashes;
    std::size_t checked = 0;
    const auto total = job.masters.size() + job.lights.size() + (job.options.badPixels ? 1 : 0);
    std::map<fs::path, CalibrationAcquisition> measurements;
    const auto snapshot = [&](SensorMasterSource& source) {
        check();
        source.path = fs::canonical(source.path);
        regular(source.path);
        auto verified = inspectVerifiedAcquisition(source.path);
        const auto& sha = verified.sha;
        measurements.emplace(source.path, std::move(verified.metadata));
        require(source.sha.empty() || source.sha == sha, "Calibration input changed: " + source.path.string());
        source.sha = sha;
        require(paths.insert(source.path).second && hashes.insert(sha).second, "Duplicate job source path or content");
        ++checked;
        progress("preflight", .1 * double(checked) / double(total), checked, total);
    };
    for (auto& [id, master] : job.masters)
        snapshot(master.value.source);
    if (job.options.badPixels) snapshot(*job.options.badPixels);
    std::vector<CalibrationAcquisition> acquisitions;
    for (auto& light : job.lights) {
        snapshot(light.source);
        const auto options = selection(job, light);
        acquisitions.push_back(measurements.at(light.source.path));
        validateSensorCalibration(acquisitions.back(), options, measurements);
    }
    const auto groups = groupCalibrationAcquisitions(acquisitions);
    for (std::size_t g = 0; g < groups.size(); ++g)
        for (const auto index : groups[g].members) {
            require(!saved || job.lights[index].group == g, "Saved grouping no longer matches source metadata");
            job.lights[index].group = g;
        }
    // Bind the acquisition/group decisions to the same bytes inspected above.
    if (job.options.badPixels) require(fileSHA256(job.options.badPixels->path) == job.options.badPixels->sha, "Defect map changed during preflight");
    for (const auto& [id, master] : job.masters) {
        check();
        require(fileSHA256(master.value.source.path) == master.value.source.sha, "Master changed during preflight");
    }
    for (const auto& light : job.lights) {
        check();
        require(fileSHA256(light.source.path) == light.source.sha, "Light changed during preflight");
    }
}
std::string canonical(const Job& job) {
    std::ostringstream out;
    out << std::setprecision(17) << "PhotonStack-CalibrationJob-1\ntemperature-tolerance "
        << job.options.temperatureTolerance << "\nmemory-mib " << job.options.memoryBytes / (1024 * 1024)
        << "\ntile-pixels " << job.options.tilePixels << "\nthreads " << job.options.workers << '\n';
    if (job.options.badPixels)
        out << "bad-pixels " << std::quoted(job.options.badPixels->path.string()) << ' ' << job.options.badPixels->sha << '\n';
    for (const auto& [id, m] : job.masters)
        out << "master " << id << ' ' << m.role << ' ' << std::quoted(m.value.source.path.string()) << ' '
            << m.value.source.sha << ' ' << m.value.biasState << '\n';
    for (const auto& l : job.lights)
        out << "light " << std::quoted(l.source.path.string()) << ' ' << l.source.sha << ' ' << l.choices[0] << ' '
            << l.choices[1] << ' ' << l.choices[2] << '\n';
    for (std::size_t i = 0; i < job.lights.size(); ++i)
        out << "group " << i << ' ' << job.lights[i].group << '\n';
    return out.str();
}
std::string frameName(std::size_t i) {
    std::ostringstream out;
    out << "frame-" << std::setfill('0') << std::setw(6) << i + 1;
    return out.str();
}
const std::set<std::string> products{"calibrated.fits", "valid.fits", "report.json"};
std::map<std::string, std::string> inspectFrame(const fs::path& dir, const std::string& jobSHA, std::size_t i,
                                                const Light& light) {
    directory(dir);
    const auto path = dir / "receipt.txt";
    regular(path);
    const auto sha = fileSHA256(path);
    std::istringstream in(readText(path, 4096));
    std::string token, value, name;
    std::size_t index, group;
    require(bool(in >> token) && token == "PhotonStack-CalibratedFrame-1" && bool(in >> token >> value) &&
                token == "job" && value == jobSHA && bool(in >> token >> index) && token == "frame" && index == i &&
                bool(in >> token >> group) && token == "group" && group == light.group && bool(in >> token >> value) &&
                token == "source" && value == light.source.sha,
            "Calibrated receipt does not match the job");
    std::map<std::string, std::string> files;
    while (in >> token) {
        require(token == "product" && bool(in >> name >> value) && products.contains(name) && channelSHA(value) &&
                    files.emplace(name, value).second,
                "Invalid calibrated product record");
        regular(dir / name);
        check();
        require(fileSHA256(dir / name) == value, "Calibrated product changed: " + name);
    }
    require(in.eof() && files.size() == products.size() && fileSHA256(path) == sha,
            "Incomplete or changed calibrated receipt");
    return files;
}
using ProductDigests = std::map<std::string, std::string>;
struct ProductStamp {
    dev_t device;
    ino_t inode;
    off_t size;
    time_t modified, changed;
    long modifiedNanos, changedNanos;
    bool operator==(const ProductStamp&) const = default;
};
using FrameStamps = std::array<ProductStamp, 4>;

FrameStamps frameStamps(const fs::path& dir) {
    directory(dir);
    FrameStamps stamps{};
    std::size_t i = 0;
    for (const char* name : {"receipt.txt", "calibrated.fits", "valid.fits", "report.json"}) {
        struct stat st{};
        require(lstat((dir / name).c_str(), &st) == 0 && S_ISREG(st.st_mode), "Expected regular calibrated product");
#ifdef __APPLE__
        const auto mt = st.st_mtimespec, ct = st.st_ctimespec;
#else
        const auto mt = st.st_mtim, ct = st.st_ctim;
#endif
        stamps[i++] = {st.st_dev, st.st_ino, st.st_size, mt.tv_sec, ct.tv_sec, mt.tv_nsec, ct.tv_nsec};
    }
    return stamps;
}
std::vector<bool> inspectFrames(const fs::path& root, const Job& job, const std::string& sha,
                                std::vector<ProductDigests>& files, std::vector<std::optional<FrameStamps>>& verified) {
    std::vector<bool> completed(job.lights.size());
    for (std::size_t i = 0; i < job.lights.size(); ++i) {
        const auto path = root / frameName(i);
        if (fs::exists(path) || fs::is_symlink(path)) {
            check();
            const auto before = frameStamps(path);
            if (!verified[i] || before != *verified[i]) {
                files[i] = inspectFrame(path, sha, i, job.lights[i]);
                require(before == frameStamps(path), "Completed frame changed during verification");
                verified[i] = before;
            }
            completed[i] = true;
        }
    }
    return completed;
}
std::string summary(const Job& job, const std::string& sha, const std::vector<bool>& completed,
                    const std::vector<ProductDigests>& files, bool sourcesVerified) {
    const auto done = std::count(completed.begin(), completed.end(), true);
    std::ostringstream out;
    out << "{\"schema\":1,\"command\":\"calibration-job\",\"domain\":\"sensor-linear\",\"jobSHA\":" << channelJSON(sha)
        << ",\"status\":" << channelJSON(std::size_t(done) == completed.size() ? "complete" : "partial")
        << ",\"sourcesVerified\":" << (sourcesVerified ? "true" : "false")
        << ",\"productsVerified\":true,\"total\":" << completed.size() << ",\"completed\":" << done << ",\"masters\":{";
    bool first = true;
    for (const auto& [id, master] : job.masters) {
        out << (first ? "" : ",") << channelJSON(id) << ":{\"role\":" << channelJSON(master.role)
            << ",\"path\":" << channelJSON(master.value.source.path.string())
            << ",\"sha\":" << channelJSON(master.value.source.sha)
            << ",\"biasState\":" << channelJSON(master.value.biasState) << '}';
        first = false;
    }
    out << "},\"badPixels\":";
    if (job.options.badPixels) out << "{\"path\":" << channelJSON(job.options.badPixels->path.string()) << ",\"sha\":" << channelJSON(job.options.badPixels->sha) << '}';
    else out << "null";
    out << ",\"dateGrouping\":\"UTC-calendar-date\",\"frames\":[";
    for (std::size_t i = 0; i < completed.size(); ++i) {
        const auto& l = job.lights[i];
        out << (i ? "," : "") << "{\"index\":" << i << ",\"group\":" << l.group
            << ",\"source\":" << channelJSON(l.source.path.string()) << ",\"sourceSHA\":" << channelJSON(l.source.sha)
            << ",\"complete\":" << (completed[i] ? "true" : "false") << ",\"directory\":" << channelJSON(frameName(i))
            << ",\"masters\":{";
        const std::array<std::string, 3> roles{"bias", "dark", "flat"};
        for (std::size_t r = 0; r < 3; ++r)
            out << (r ? "," : "") << channelJSON(roles[r]) << ':'
                << (l.choices[r] == "-" ? "null" : channelJSON(l.choices[r]));
        out << "},\"products\":{";
        first = true;
        for (const auto& [name, digest] : files[i]) {
            out << (first ? "" : ",") << channelJSON(name) << ':' << channelJSON(digest);
            first = false;
        }
        out << "}}";
    }
    out << "]}";
    return out.str();
}
std::string frameReport(const SensorCalibrationResult& r, std::size_t index, std::size_t group, double seconds) {
    std::ostringstream out;
    out << std::setprecision(17)
        << "{\"schema\":1,\"command\":\"sensor-calibrate\",\"domain\":\"sensor-linear\",\"frame\":" << index
        << ",\"group\":" << group << ",\"width\":" << r.acquisition.width << ",\"height\":" << r.acquisition.height
        << ",\"sampling\":" << channelJSON(r.acquisition.text.at("sampling"))
        << ",\"invalidPixels\":" << r.invalidPixels << ",\"negativePixels\":" << r.negativePixels
        << ",\"biasOnLight\":" << (r.biasOnLight ? "true" : "false")
        << ",\"biasOnFlat\":" << (r.biasOnFlat ? "true" : "false")
        << ",\"darkApplied\":" << (r.options.dark ? "true" : "false")
        << ",\"flatApplied\":" << (r.options.flat ? "true" : "false")
        << ",\"badPixelsApplied\":" << (r.options.badPixels ? "true" : "false")
        << ",\"flatMinimumRelativeResponse\":0.000001,\"flatMeans\":[";
    for (unsigned p = 0; p < 4; ++p)
        out << (p ? "," : "") << r.flatMeans[p];
    out << "],\"tilePixels\":" << r.tilePixels << ",\"workingBytes\":" << r.workingBytes << ",\"workers\":" << r.workers
        << ",\"backend\":\"cpu\",\"calibrateAndWriteSeconds\":" << seconds << '}';
    return out.str();
}
} // namespace

int runCalibrationJob(const std::vector<std::string>& args) {
    Signals signals;
    activeCommand = args[0];
    const bool fresh = args[0] == "calibration-run", inspect = args[0] == "calibration-job-inspect";
    std::map<std::string, std::string> options;
    for (std::size_t i = 1; i < args.size(); i += 2)
        require(i + 1 < args.size() && options.emplace(args[i], args[i + 1]).second,
                "Repeated or incomplete job option");
    for (const auto& [key, value] : options)
        require(key == (fresh ? "--manifest" : "--job") || (fresh && key == "--output-dir") ||
                    (!inspect && key == "--max-frames"),
                "Unknown calibration job option");
    require(options.contains(fresh ? "--manifest" : "--job") && (!fresh || options.contains("--output-dir")),
            "Specify a manifest and new output directory, or a saved job");
    const auto maximum = options.contains("--max-frames") ? integer(options.at("--max-frames"), 1, 4096) : 4096;
    Job job;
    fs::path root;
    if (fresh) {
        const auto manifest = fs::canonical(options.at("--manifest"));
        const auto before = fileSHA256(manifest);
        job = parse(manifest, false);
        bind(job, false);
        require(fileSHA256(manifest) == before, "Manifest changed during preflight");
        root = fs::absolute(options.at("--output-dir")).lexically_normal();
        if (root.filename().empty()) root = root.parent_path();
        directory(root.parent_path());
        require(!fs::exists(root) && !fs::is_symlink(root), "Choose a new calibration job directory");
        Stage stage(root.parent_path());
        writeText(stage.path / "job.txt", canonical(job));
        writeText(stage.path / "job.sha256", fileSHA256(stage.path / "job.txt") + "\n");
        writeText(stage.path / ".lock", "");
        check();
        publish(stage.path, root);
    } else {
        root = fs::absolute(options.at("--job")).lexically_normal();
        directory(root);
    }
    Lock lock(root, inspect);
    regular(root / "job.txt");
    const auto sha = fileSHA256(root / "job.txt");
    require(readText(root / "job.sha256", 65) == sha + "\n", "Saved job configuration changed");
    job = parse(root / "job.txt", true);
    require(canonical(job) == readText(root / "job.txt", 2 * 1024 * 1024), "Saved job is not canonical");
    std::vector<ProductDigests> files(job.lights.size());
    std::vector<std::optional<FrameStamps>> verified(job.lights.size());
    auto completed = inspectFrames(root, job, sha, files, verified);
    if (!inspect) {
        if (!fresh)
            bind(job, true);
        SensorCalibrationSession calibrationSession;
        std::size_t computed = 0;
        for (std::size_t i = 0; i < job.lights.size() && computed < maximum; ++i)
            if (!completed[i]) {
                check();
                Stage stage(root);
                const auto begin = std::chrono::steady_clock::now();
                const auto result = calibrationSession.calibrate(job.lights[i].source, selection(job, job.lights[i]));
                writeCalibratedSensor(result, stage.path / "calibrated.fits");
                const auto mask = FitsCodec().write(result.valid, stage.path / "valid.fits");
                require(mask.ok, mask.message);
                writeText(stage.path / "report.json",
                          frameReport(result, i, job.lights[i].group,
                                      std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()));
                std::ostringstream receipt;
                receipt << "PhotonStack-CalibratedFrame-1\njob " << sha << "\nframe " << i << "\ngroup "
                        << job.lights[i].group << "\nsource " << job.lights[i].source.sha << '\n';
                for (const auto& name : products)
                    receipt << "product " << name << ' ' << fileSHA256(stage.path / name) << '\n';
                writeText(stage.path / "receipt.txt", receipt.str());
                check();
                require(fileSHA256(root / "job.txt") == sha, "Job configuration changed during calibration");
                publish(stage.path, root / frameName(i));
                completed[i] = true;
                ++computed;
                const auto done = std::size_t(std::count(completed.begin(), completed.end(), true));
                progress("calibrate", .1 + .9 * double(done) / double(completed.size()), done, completed.size());
                std::cerr << "Calibrated " << std::count(completed.begin(), completed.end(), true) << '/'
                          << completed.size() << '\n';
            }
    }
    require(fileSHA256(root / "job.txt") == sha && readText(root / "job.sha256", 65) == sha + "\n",
            "Job changed during inspection");
    if (!inspect) {
        const auto done = std::size_t(std::count(completed.begin(), completed.end(), true));
        progress("verify-products", .1 + .9 * double(done) / double(completed.size()), done, completed.size());
        for (const auto& [id, master] : job.masters) {
            check();
            require(fileSHA256(master.value.source.path) == master.value.source.sha, "Master changed during the job");
        }
        for (const auto& light : job.lights) {
            check();
            require(fileSHA256(light.source.path) == light.source.sha, "Light changed during the job");
        }
        completed = inspectFrames(root, job, sha, files, verified);
    }
    // The filesystem's committed per-frame receipts are the journal. A crash
    // needs no mutable global completion bit to recover correctly.
    std::cout << summary(job, sha, completed, files, !inspect) << '\n';
    return 0;
}
