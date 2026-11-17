#pragma once
#include "photonstack/FileDigest.hpp"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace photonstack::cli {
struct NarrowbandFile { std::filesystem::path path; std::string sha; };
struct NarrowbandRecipe {
    std::string operation;
    double green = 0;
    std::vector<NarrowbandFile> inputs, outputs;
};
inline bool narrowbandSHA(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
inline void verifyNarrowbandInputs(const NarrowbandRecipe& r) {
    for (const auto& f : r.inputs)
        if (fileSHA256(f.path) != f.sha) throw std::runtime_error("Narrowband source changed: " + f.path.string());
}
inline void validateNarrowbandRecipe(const NarrowbandRecipe& r) {
    if (!std::isfinite(r.green) || r.green < 0 || r.green > 1 ||
        !((r.operation == "extract" && r.inputs.size() == 1) ||
          (r.operation == "compose" && r.inputs.size() == 2) || (r.operation == "stack" && r.inputs.size() >= 3)))
        throw std::invalid_argument("Invalid narrowband recipe operation or input count");
    std::set<std::filesystem::path> paths;
    for (const auto& f : r.inputs)
        if (!f.path.is_absolute() || !narrowbandSHA(f.sha) || !paths.insert(f.path.lexically_normal()).second)
            throw std::invalid_argument("Invalid or duplicate narrowband source");
    paths.clear();
    const std::set<std::string> names{"Ha.fits", "OIII.fits", "HOO.fits", "Ha-valid.fits", "OIII-valid.fits",
        "Ha-weight.fits", "OIII-weight.fits", "geometric-coverage.fits", "validity.fits"};
    for (const auto& f : r.outputs)
        if (!names.contains(f.path.string()) || !narrowbandSHA(f.sha) || !paths.insert(f.path).second)
            throw std::invalid_argument("Invalid narrowband output record");
    const auto require = [&](const char* name) {
        if (!paths.contains(name)) throw std::invalid_argument("Incomplete narrowband output manifest");
    };
    if (r.operation == "compose") { require("HOO.fits"); require("validity.fits"); }
    else {
        require("Ha.fits"); require("OIII.fits");
        if (r.operation == "extract") { require("Ha-valid.fits"); require("OIII-valid.fits"); }
        else { require("HOO.fits"); require("Ha-weight.fits"); require("OIII-weight.fits"); require("geometric-coverage.fits"); }
    }
}
inline NarrowbandRecipe readNarrowbandRecipe(const std::filesystem::path& path, bool verifyOutputs) {
    std::ifstream in(path); std::string token; NarrowbandRecipe r;
    if (!(in >> token) || token != "PhotonStack-Narrowband-1") throw std::invalid_argument("Unsupported narrowband recipe");
    if (!(in >> token) || token != "operation" || !(in >> std::quoted(r.operation))) throw std::invalid_argument("Missing recipe operation");
    if (!(in >> token) || token != "green-ha" || !(in >> r.green)) throw std::invalid_argument("Missing HOO mapping");
    while (in >> token) {
        std::string pathText, sha;
        if ((token != "input" && token != "output") || !(in >> std::quoted(pathText) >> sha))
            throw std::invalid_argument("Malformed narrowband recipe record");
        (token == "input" ? r.inputs : r.outputs).push_back({pathText, sha});
        if (r.inputs.size() > 100000 || r.outputs.size() > 16) throw std::invalid_argument("Recipe exceeds supported size");
    }
    if (!in.eof()) throw std::invalid_argument("Cannot read narrowband recipe");
    validateNarrowbandRecipe(r); verifyNarrowbandInputs(r);
    if (verifyOutputs) for (const auto& f : r.outputs)
        if (fileSHA256(path.parent_path() / f.path) != f.sha) throw std::runtime_error("Narrowband result changed: " + f.path.string());
    return r;
}
inline void writeNarrowbandRecipe(const std::filesystem::path& path, const NarrowbandRecipe& r) {
    validateNarrowbandRecipe(r);
    std::ofstream out(path);
    out << "PhotonStack-Narrowband-1\noperation " << std::quoted(r.operation) << "\ngreen-ha " << std::setprecision(17) << r.green << '\n';
    for (const auto& f : r.inputs) out << "input " << std::quoted(f.path.string()) << ' ' << f.sha << '\n';
    for (const auto& f : r.outputs) out << "output " << std::quoted(f.path.string()) << ' ' << f.sha << '\n';
    out.flush(); if (!out) throw std::runtime_error("Cannot write narrowband recipe");
}
inline std::string narrowbandJSON(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    out << '"'; return out.str();
}
inline void printNarrowbandRecipe(std::ostream& out, const NarrowbandRecipe& r, const std::filesystem::path& directory) {
    out << "{\"type\":\"complete\",\"command\":" << narrowbandJSON("narrowband-" + r.operation) << ",\"operation\":" << narrowbandJSON(r.operation)
        << ",\"directory\":" << narrowbandJSON(std::filesystem::absolute(directory).string())
        << ",\"greenHydrogen\":" << std::setprecision(17) << r.green << ",\"frames\":" << r.inputs.size() << ",\"outputs\":[";
    for (std::size_t i = 0; i < r.outputs.size(); ++i) { if (i) out << ','; out << narrowbandJSON(r.outputs[i].path.string()); }
    out << "]}\n";
}
}
