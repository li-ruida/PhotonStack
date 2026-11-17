#pragma once
#include "photonstack/ChannelComposer.hpp"
#include "photonstack/FileDigest.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace photonstack::cli {
struct ChannelSource { std::string name; std::filesystem::path path; std::string sha; };
struct ChannelRecipe {
    std::vector<ChannelSource> sources;
    std::array<std::string, 3> expressions;
    bool aligned = false;
    std::map<std::string, std::string> products;
    std::vector<std::string> names() const {
        std::vector<std::string> result; for (const auto& s : sources) result.push_back(s.name); return result;
    }
    std::array<ChannelExpression, 3> compile() const {
        const auto n = names(); return {ChannelExpression(expressions[0], n), ChannelExpression(expressions[1], n), ChannelExpression(expressions[2], n)};
    }
};
inline std::string channelJSON(const std::string& text) {
    std::ostringstream out; out << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << int(c);
        else out << c;
    }
    out << '"'; return out.str();
}
inline bool channelSHA(const std::string& sha) { return sha.size() == 64 && sha.find_first_not_of("0123456789abcdef") == std::string::npos; }
inline void verifyChannelSources(const ChannelRecipe& r) {
    if (!r.aligned || r.sources.empty() || r.sources.size() > 16) throw std::invalid_argument("Confirm that 1..16 masters have been registered (--aligned)");
    std::set<std::string> names;
    for (const auto& s : r.sources) {
        if (!ChannelExpression::validName(s.name) || !names.insert(s.name).second || !s.path.is_absolute() || !channelSHA(s.sha))
            throw std::invalid_argument("Invalid channel master record");
        if (fileSHA256(s.path) != s.sha) throw std::runtime_error("Channel master changed: " + s.path.string());
    }
    (void)r.compile();
}
inline void writeChannelRecipe(const std::filesystem::path& path, const ChannelRecipe& r) {
    std::ofstream out(path);
    out << "PhotonStack-Channels-1\naligned 1\n";
    for (const auto& s : r.sources) out << "source " << std::quoted(s.name) << ' ' << std::quoted(s.path.string()) << ' ' << s.sha << '\n';
    for (std::size_t c = 0; c < 3; ++c) out << "expression " << c << ' ' << std::quoted(r.expressions[c]) << '\n';
    for (const auto& [name, sha] : r.products) out << "product " << std::quoted(name) << ' ' << sha << '\n';
    out.flush(); if (!out) throw std::runtime_error("Cannot write channel recipe");
}
inline ChannelRecipe readChannelRecipe(const std::filesystem::path& path, bool outputs) {
    if (std::filesystem::file_size(path) > 128*1024) throw std::invalid_argument("Channel recipe too large");
    std::ifstream in(path); std::string token; ChannelRecipe r;
    if (!(in >> token) || token != "PhotonStack-Channels-1" || !(in >> token) || token != "aligned" || !(in >> token) || token != "1")
        throw std::invalid_argument("Invalid channel recipe header");
    r.aligned = true; std::set<unsigned> channels;
    while (in >> token) {
        if (token == "source") {
            ChannelSource s; std::string p;
            if (!(in >> std::quoted(s.name) >> std::quoted(p) >> s.sha) || r.sources.size() >= 16) throw std::invalid_argument("Invalid master record");
            s.path = p; r.sources.push_back(s);
        } else if (token == "expression") {
            unsigned c; std::string value;
            if (!(in >> c >> std::quoted(value)) || c > 2 || !channels.insert(c).second) throw std::invalid_argument("Invalid channel expression record");
            r.expressions[c] = value;
        } else if (token == "product") {
            std::string name, sha;
            if (!(in >> std::quoted(name) >> sha) || (name != "RGB.fits" && name != "validity.fits") || !channelSHA(sha) || !r.products.emplace(name, sha).second)
                throw std::invalid_argument("Invalid channel product record");
        } else throw std::invalid_argument("Unknown channel recipe field");
    }
    if (!in.eof() || channels.size() != 3 || r.products.size() != 2) throw std::invalid_argument("Incomplete channel recipe");
    verifyChannelSources(r);
    if (outputs) for (const auto& [name, sha] : r.products)
        if (fileSHA256(path.parent_path()/name) != sha) throw std::runtime_error("Channel product changed: " + name);
    return r;
}
inline void printChannelRecipe(const ChannelRecipe& r, const std::filesystem::path& directory, std::ostream& out,
                               std::size_t invalid = 0, std::size_t workers = 0, bool sky = false) {
    out << "{\"type\":\"complete\",\"command\":\"channel-compose\",\"directory\":" << channelJSON(std::filesystem::absolute(directory).string())
        << ",\"aligned\":true,\"sources\":[";
    for (std::size_t i = 0; i < r.sources.size(); ++i) {
        if (i) out << ',';
        const auto& s = r.sources[i]; out << "{\"name\":" << channelJSON(s.name) << ",\"path\":" << channelJSON(s.path.string()) << ",\"sha\":" << channelJSON(s.sha) << '}';
    }
    out << "],\"expressions\":[";
    for (std::size_t c = 0; c < 3; ++c) { if (c) out << ','; out << channelJSON(r.expressions[c]); }
    out << "],\"outputs\":[\"RGB.fits\",\"validity.fits\"]";
    // Inspect describes the saved mapping only, never fabricates run statistics.
    if (workers) out << ",\"invalidPixels\":" << invalid << ",\"workers\":" << workers << ",\"wcsPreserved\":" << (sky ? "true" : "false");
    out << "}\n";
}
} // namespace photonstack::cli
