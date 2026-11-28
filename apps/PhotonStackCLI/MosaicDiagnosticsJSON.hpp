#pragma once
#include "photonstack/MosaicDiagnostics.hpp"
#include <iomanip>
#include <filesystem>
#include <ostream>

namespace photonstack::cli {
template <typename T, std::size_t N> inline void seamArray(std::ostream& out, const std::array<T, N>& values) {
    out << '[';
    for (std::size_t i = 0; i < N; ++i) { if (i) out << ','; out << values[i]; }
    out << ']';
}
inline void seamStats(std::ostream& out, const std::optional<MosaicSeamStatistics>& stats) {
    if (!stats) { out << "null"; return; }
    out << "{\"samples\":" << stats->samples << ",\"firstMedian\":"; seamArray(out, stats->firstMedian);
    out << ",\"secondMedian\":"; seamArray(out, stats->secondMedian);
    out << ",\"medianDifference\":"; seamArray(out, stats->medianDifference);
    out << ",\"rmsDifference\":"; seamArray(out, stats->rmsDifference);
    out << ",\"p95Absolute\":"; seamArray(out, stats->p95Absolute);
    out << ",\"madDifference\":"; seamArray(out, stats->madDifference); out << '}';
}
inline void seamRegion(std::ostream& out, const MosaicSeamRegion& region) {
    out << "{\"bounds\":"; seamArray(out, region.bounds);
    out << ",\"candidates\":" << region.candidates << ",\"before\":"; seamStats(out, region.before);
    out << ",\"after\":"; seamStats(out, region.after); out << '}';
}
inline void writeMosaicSeams(std::ostream& out, const std::optional<MosaicSeamDiagnostics>& seams,
    const std::vector<std::filesystem::path>& inputs, const std::function<std::string(const std::string&)>& escape) {
    if (!seams) { out << "null"; return; }
    const auto precision = out.precision(); out << std::setprecision(17);
    out << "{\"version\":1,\"width\":" << seams->width << ",\"height\":" << seams->height
        << ",\"channels\":" << seams->channels << ",\"preview\":" << (seams->preview ? "true" : "false")
        << ",\"encoding\":\"" << seams->encoding << "\",\"gridLimit\":48,\"patchRadius\":1,\"minimumCoverage\":0.99,\"inputs\":[";
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (i) out << ',';
        out << '"' << escape(std::filesystem::absolute(inputs[i]).string()) << '"';
    }
    out << "],\"corrections\":[";
    for (std::size_t i = 0; i < seams->corrections.size(); ++i) {
        if (i) out << ',';
        out << "{\"gains\":"; seamArray(out, seams->corrections[i].gains);
        out << ",\"offsets\":"; seamArray(out, seams->corrections[i].offsets); out << '}';
    }
    out << "],\"pairs\":[";
    for (std::size_t i = 0; i < seams->pairs.size(); ++i) {
        if (i) out << ',';
        const auto& pair = seams->pairs[i];
        out << "{\"first\":" << pair.first << ",\"second\":" << pair.second << ",\"summary\":";
        seamRegion(out, pair.summary); out << ",\"cells\":[";
        for (std::size_t k = 0; k < pair.cells.size(); ++k) { if (k) out << ','; seamRegion(out, pair.cells[k]); }
        out << "],\"final\":";
        if(!pair.final)out<<"null";
        else {const auto& f=*pair.final;
            out<<"{\"samples\":"<<f.samples<<",\"backgroundCandidates\":"<<f.backgroundCandidates<<",\"structuredSamples\":"<<f.structuredSamples<<",\"status\":\""<<f.status<<"\"";
            out<<",\"backgroundGradientExcessRms\":";seamArray(out,f.backgroundGradientExcessRms);
            out<<",\"backgroundGradientExcessP95\":";seamArray(out,f.backgroundGradientExcessP95);
            out<<",\"structureExcessP95\":";seamArray(out,f.structureExcessP95);
            out<<",\"structureExcessFraction\":";seamArray(out,f.structureExcessFraction);
            out<<",\"rangeExcessP95\":";seamArray(out,f.rangeExcessP95);
            out<<",\"tolerance\":";seamArray(out,f.tolerance);out<<'}';
        }
        out << '}';
    }
    out << "]}"; out.precision(precision);
}
}
