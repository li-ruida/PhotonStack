#include "CalibrationMetadataCommand.hpp"
#include "ChannelRecipe.hpp"
#include "photonstack/CalibrationMetadata.hpp"
#include "photonstack/FitsCodec.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <tuple>

namespace {
using namespace photonstack;
using namespace photonstack::cli;
struct Snapshot {
    std::filesystem::path path;
    std::string sha;
    std::uintmax_t bytes;
    CalibrationAcquisition metadata;
};
Snapshot snapshot(const std::filesystem::path& path, const std::string& expected = "") {
    const auto absolute = std::filesystem::canonical(path);
    if (!isFitsPath(absolute) || !std::filesystem::is_regular_file(absolute))
        throw std::invalid_argument("Acquisition inspection currently requires a FITS image");
    const auto sha = fileSHA256(absolute);
    if (!expected.empty() && expected != sha)
        throw std::invalid_argument("Acquisition source changed");
    const auto metadata = inspectCalibrationAcquisition(absolute);
    if (fileSHA256(absolute) != sha)
        throw std::runtime_error("Acquisition source changed during inspection");
    return {absolute, sha, std::filesystem::file_size(absolute), metadata};
}
void json(std::ostream& out, const Snapshot& s) {
    const auto& m = s.metadata;
    out << "{\"path\":" << channelJSON(s.path.string()) << ",\"sha\":" << channelJSON(s.sha) << ",\"bytes\":" << s.bytes
        << ",\"width\":" << m.width << ",\"height\":" << m.height << ",\"channels\":" << m.channels
        << ",\"bitpix\":" << m.bitpix << ",\"text\":{";
    bool first = true;
    for (const auto& [k, v] : m.text) {
        out << (first ? "" : ",") << channelJSON(k) << ':' << channelJSON(v);
        first = false;
    }
    out << "},\"numeric\":{";
    first = true;
    for (const auto& [k, v] : m.numeric) {
        out << (first ? "" : ",") << channelJSON(k) << ':' << v;
        first = false;
    }
    out << "},\"sourceKeys\":{";
    first = true;
    for (const auto& [k, v] : m.keys) {
        out << (first ? "" : ",") << channelJSON(k) << ":[";
        first = false;
        for (std::size_t i = 0; i < v.size(); ++i)
            out << (i ? "," : "") << channelJSON(v[i]);
        out << ']';
    }
    out << "},\"issues\":[";
    for (std::size_t i = 0; i < m.issues.size(); ++i) {
        const auto& e = m.issues[i];
        out << (i ? "," : "") << "{\"field\":" << channelJSON(e.field) << ",\"code\":" << channelJSON(e.code)
            << ",\"message\":" << channelJSON(e.message) << '}';
    }
    out << "]}";
}
const std::map<std::string, CalibrationMasterRole> masterRoles{{"bias", CalibrationMasterRole::Bias},
                                                               {"dark", CalibrationMasterRole::Dark},
                                                               {"flat", CalibrationMasterRole::Flat},
                                                               {"dark-flat", CalibrationMasterRole::DarkFlat}};
void evidenceJSON(std::ostream& out, const CalibrationMatchEvidence& e) {
    out << "{\"field\":" << channelJSON(e.field) << ",\"status\":" << channelJSON(e.status)
        << ",\"input\":" << channelJSON(e.input) << ",\"master\":" << channelJSON(e.master)
        << ",\"message\":" << channelJSON(e.message) << '}';
}
using Choices = std::map<std::string, std::vector<std::size_t>>;
void boundReport(std::ostream& out) {
    if (out.tellp() > 64 * 1024 * 1024)
        throw std::invalid_argument(
            "Calibration report exceeds 64 MiB; split the input batch or reduce candidate masters");
}
void choicesJSON(std::ostream& out, const Choices& compatible, const Choices& review,
                 const std::set<std::string>& present) {
    out << '{';
    bool first = true;
    const std::vector<std::size_t> empty;
    for (const auto* role : {"bias", "dark", "flat"}) {
        const auto& candidates = compatible.contains(role) ? compatible.at(role) : empty;
        const auto& pending = review.contains(role) ? review.at(role) : empty;
        const std::string decision = candidates.size() > 1    ? "ambiguous"
                                     : candidates.size() == 1 ? "unique-compatible"
                                     : !pending.empty()       ? "review-required"
                                     : present.contains(role) ? "no-compatible-master"
                                                              : "missing-master";
        out << (first ? "" : ",") << channelJSON(role) << ":{\"status\":" << channelJSON(decision)
            << ",\"suggestedMaster\":";
        if (candidates.size() == 1)
            out << candidates.front();
        else
            out << "null";
        out << ",\"compatibleMasters\":[";
        for (std::size_t i = 0; i < candidates.size(); ++i)
            out << (i ? "," : "") << candidates[i];
        out << "],\"reviewMasters\":[";
        for (std::size_t i = 0; i < pending.size(); ++i)
            out << (i ? "," : "") << pending[i];
        out << "]}";
        first = false;
    }
    out << '}';
}
void plan(std::ostream& out, const std::vector<std::string>& args) {
    if (args.size() != 3 || args[1] != "--manifest")
        throw std::invalid_argument("Specify calibration-plan --manifest acquisition-list.txt");
    const auto manifest = std::filesystem::canonical(args[2]);
    if (!std::filesystem::is_regular_file(manifest) || std::filesystem::file_size(manifest) > 1024 * 1024)
        throw std::invalid_argument("Calibration manifest must be a regular file of at most 1 MiB");
    const auto manifestSHA = fileSHA256(manifest);
    std::ifstream stream(manifest);
    std::string line, token;
    if (!std::getline(stream, line) || line != "PhotonStack-CalibrationPlan-1")
        throw std::invalid_argument("Invalid calibration manifest version");
    double tolerance = 1;
    if (!std::getline(stream, line))
        throw std::invalid_argument("Missing temperature tolerance");
    std::istringstream settings(line);
    if (!(settings >> token >> tolerance) || token != "temperature-tolerance" || (settings >> token) ||
        !std::isfinite(tolerance) || tolerance < 0 || tolerance > 10)
        throw std::invalid_argument("Invalid temperature tolerance");
    struct Record {
        std::string role;
        std::filesystem::path path;
        std::string expected;
    };
    std::vector<Record> records;
    std::size_t lightCount = 0, masterCount = 0;
    while (std::getline(stream, line)) {
        if (line.empty())
            continue;
        std::istringstream in(line);
        std::string role, path, expected, extra;
        if (!(in >> role) || (role != "light" && role != "bias" && role != "dark" && role != "flat") ||
            (in >> std::ws).peek() != '"' || !(in >> std::quoted(path) >> expected) || (in >> extra) || path.empty() ||
            path.find_first_of("\r\n\0", 0, 3) != std::string::npos || !std::filesystem::path(path).is_absolute() ||
            (expected != "-" && !channelSHA(expected)))
            throw std::invalid_argument("Invalid calibration source record");
        role == "light" ? ++lightCount : ++masterCount;
        if (lightCount > 4096 || masterCount > 256 || lightCount * masterCount > 262144)
            throw std::invalid_argument("Plan limit: 4096 lights, 256 masters and 262144 pair comparisons");
        records.push_back({role, path, expected == "-" ? "" : expected});
    }
    if (!stream.eof() || !lightCount || fileSHA256(manifest) != manifestSHA)
        throw std::invalid_argument("Empty, unreadable or changed calibration manifest");
    std::sort(records.begin(), records.end(),
              [](const auto& a, const auto& b) { return std::tie(a.role, a.path) < std::tie(b.role, b.path); });
    struct Master {
        std::string role;
        Snapshot source;
    };
    std::vector<Snapshot> lights;
    std::vector<Master> masters;
    std::vector<CalibrationAcquisition> acquisitions;
    std::set<std::filesystem::path> paths;
    std::set<std::string> hashes;
    for (const auto& record : records) {
        auto source = snapshot(record.path, record.expected);
        const auto declaredType = calibrationFrameType(source.metadata);
        if (record.role == "light" && !declaredType.empty() && declaredType != "light")
            throw std::invalid_argument("Manifest light has a contradictory FITS frame type: " + source.path.string());
        if (!paths.insert(source.path).second || !hashes.insert(source.sha).second)
            throw std::invalid_argument("Duplicate acquisition path or content in calibration manifest: " +
                                        source.path.string());
        if (record.role == "light") {
            acquisitions.push_back(source.metadata);
            lights.push_back(std::move(source));
        } else
            masters.push_back({record.role, std::move(source)});
    }
    const auto groups = groupCalibrationAcquisitions(acquisitions);
    std::vector<Choices> perLightCompatible(lights.size()), perLightReview(lights.size());
    std::vector<std::size_t> perLightGroup(lights.size());
    std::set<std::string> presentRoles;
    for (const auto& master : masters)
        presentRoles.insert(master.role);
    out << "{\"type\":\"complete\",\"command\":\"calibration-plan\",\"schema\":1,\"scope\":\"acquisition-only\","
           "\"canApply\":false,\"pixelsValidated\":false,\"dateGrouping\":\"UTC-calendar-date\",\"manifestSHA\":"
        << channelJSON(manifestSHA) << ",\"temperatureTolerance\":" << tolerance << ",\"lights\":[";
    for (std::size_t i = 0; i < lights.size(); ++i) {
        if (i)
            out << ',';
        json(out, lights[i]);
        boundReport(out);
    }
    out << "],\"masters\":[";
    for (std::size_t i = 0; i < masters.size(); ++i) {
        out << (i ? "," : "") << "{\"role\":" << channelJSON(masters[i].role) << ",\"acquisition\":";
        json(out, masters[i].source);
        out << '}';
        boundReport(out);
    }
    out << "],\"groups\":[";
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const auto& group = groups[g];
        for (const auto member : group.members)
            perLightGroup[member] = g;
        out << (g ? "," : "") << "{\"members\":[";
        for (std::size_t j = 0; j < group.members.size(); ++j)
            out << (j ? "," : "") << group.members[j];
        out << "],\"text\":{";
        bool first = true;
        for (const auto& [k, v] : group.text) {
            out << (first ? "" : ",") << channelJSON(k) << ':' << channelJSON(v);
            first = false;
        }
        out << "},\"numeric\":{";
        first = true;
        for (const auto& [k, v] : group.numeric) {
            out << (first ? "" : ",") << channelJSON(k) << ':' << v;
            first = false;
        }
        out << "},\"issues\":[";
        for (std::size_t i = 0; i < group.issues.size(); ++i)
            out << (i ? "," : "") << channelJSON(group.issues[i]);
        out << "],\"candidates\":[";
        Choices compatible, review;
        for (std::size_t m = 0; m < masters.size(); ++m) {
            const auto& master = masters[m];
            std::map<std::string, std::size_t> counts;
            struct Issue {
                CalibrationMatchEvidence example;
                std::size_t count = 0, firstMember = 0;
            };
            std::map<std::pair<std::string, std::string>, Issue> issues;
            for (const auto member : group.members) {
                const auto match = matchCalibrationAcquisition(acquisitions[member], master.source.metadata,
                                                               masterRoles.at(master.role), tolerance);
                ++counts[match.status];
                if (match.status == "compatible")
                    perLightCompatible[member][master.role].push_back(m);
                if (match.status == "review")
                    perLightReview[member][master.role].push_back(m);
                std::set<std::pair<std::string, std::string>> seen;
                for (const auto& e : match.evidence) {
                    if (e.status != "invalid" && e.status != "conflict" && e.status != "missing")
                        continue;
                    const auto key = std::make_pair(e.field, e.status);
                    if (!seen.insert(key).second)
                        continue;
                    auto& issue = issues[key];
                    if (issue.count++ == 0) {
                        issue.example = e;
                        issue.firstMember = member;
                    }
                }
            }
            const std::string status = counts["incompatible"] ? "incompatible"
                                       : counts["review"]     ? "review"
                                                              : "compatible";
            if (status == "compatible")
                compatible[master.role].push_back(m);
            if (status == "review")
                review[master.role].push_back(m);
            out << (m ? "," : "") << "{\"master\":" << m << ",\"status\":" << channelJSON(status)
                << ",\"compatibleCount\":" << counts["compatible"] << ",\"reviewCount\":" << counts["review"]
                << ",\"incompatibleCount\":" << counts["incompatible"] << ",\"issues\":[";
            first = true;
            for (const auto& [key, issue] : issues) {
                out << (first ? "" : ",") << "{\"count\":" << issue.count << ",\"firstMember\":" << issue.firstMember
                    << ",\"example\":";
                evidenceJSON(out, issue.example);
                out << '}';
                first = false;
            }
            out << "]}";
        }
        out << "],\"roles\":";
        choicesJSON(out, compatible, review, presentRoles);
        out << '}';
        boundReport(out);
    }
    out << "],\"frameChoices\":[";
    for (std::size_t i = 0; i < lights.size(); ++i) {
        out << (i ? "," : "") << "{\"light\":" << i << ",\"group\":" << perLightGroup[i] << ",\"roles\":";
        choicesJSON(out, perLightCompatible[i], perLightReview[i], presentRoles);
        out << '}';
        boundReport(out);
    }
    out << "]}\n";
}
} // namespace

int runCalibrationMetadata(const std::vector<std::string>& args) {
    std::ostringstream out;
    out << std::setprecision(17);
    if (args[0] == "calibration-plan") {
        plan(out, args);
    } else if (args[0] == "calibration-inspect") {
        if (args.size() < 2 || args.size() > 257)
            throw std::invalid_argument("Specify 1..256 FITS inputs per inspection batch");
        out << "{\"type\":\"complete\",\"command\":\"calibration-inspect\",\"schema\":1,\"scope\":\"acquisition-only\","
               "\"entries\":[";
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (i > 1)
                out << ',';
            try {
                const auto s = snapshot(args[i]);
                out << "{\"status\":\"inspected\",\"acquisition\":";
                json(out, s);
                out << '}';
            } catch (const std::exception& e) {
                // Compose an entry only after inspection to avoid partial JSON.
                throw std::invalid_argument(args[i] + ": " + e.what());
            }
        }
        out << "]}\n";
    } else {
        const std::set<std::string> allowed{"--input",
                                            "--master",
                                            "--role",
                                            "--temperature-tolerance",
                                            "--expected-input-sha",
                                            "--expected-master-sha"};
        std::map<std::string, std::string> options;
        for (std::size_t i = 1; i < args.size(); i += 2)
            if (i + 1 >= args.size() || !allowed.contains(args[i]) || !options.emplace(args[i], args[i + 1]).second)
                throw std::invalid_argument("Invalid or duplicate calibration matching option");
        for (const auto* key : {"--input", "--master", "--role"})
            if (!options.contains(key))
                throw std::invalid_argument(std::string("Specify ") + key);
        const std::map<std::string, CalibrationMasterRole> roles{{"bias", CalibrationMasterRole::Bias},
                                                                 {"dark", CalibrationMasterRole::Dark},
                                                                 {"flat", CalibrationMasterRole::Flat},
                                                                 {"dark-flat", CalibrationMasterRole::DarkFlat}};
        if (!roles.contains(options.at("--role")))
            throw std::invalid_argument("Role must be bias, dark, flat or dark-flat");
        double tolerance = 1;
        if (options.contains("--temperature-tolerance")) {
            std::size_t used = 0;
            tolerance = std::stod(options.at("--temperature-tolerance"), &used);
            if (used != options.at("--temperature-tolerance").size())
                throw std::invalid_argument("Invalid temperature tolerance");
        }
        const auto input = snapshot(options.at("--input"), options["--expected-input-sha"]);
        const auto master = snapshot(options.at("--master"), options["--expected-master-sha"]);
        if (input.path == master.path || input.sha == master.sha)
            throw std::invalid_argument("Input and master must be distinct acquisitions");
        const auto match =
            matchCalibrationAcquisition(input.metadata, master.metadata, roles.at(options.at("--role")), tolerance);
        out << "{\"type\":\"complete\",\"command\":\"calibration-match\",\"schema\":1,\"scope\":\"acquisition-only\","
               "\"canApply\":false,\"pixelsValidated\":false,\"role\":"
            << channelJSON(options.at("--role")) << ",\"temperatureTolerance\":" << tolerance
            << ",\"status\":" << channelJSON(match.status) << ",\"input\":";
        json(out, input);
        out << ",\"master\":";
        json(out, master);
        out << ",\"evidence\":[";
        for (std::size_t i = 0; i < match.evidence.size(); ++i) {
            const auto& e = match.evidence[i];
            out << (i ? "," : "") << "{\"field\":" << channelJSON(e.field) << ",\"status\":" << channelJSON(e.status)
                << ",\"input\":" << channelJSON(e.input) << ",\"master\":" << channelJSON(e.master)
                << ",\"message\":" << channelJSON(e.message) << '}';
        }
        out << "]}\n";
    }
    std::cout << out.str();
    return 0;
}
