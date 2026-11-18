#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>
namespace photonstack {
// Small bounded scalar numeric BINTABLE interface for astrometric catalogs.
// Unrequested columns are skipped; requested arrays, scaling and null sentinels
// are rejected rather than silently reinterpreted.
using FitsNumericTable = std::map<std::string, std::vector<double>>;
FitsNumericTable readFitsNumericTable(const std::filesystem::path& file, const std::vector<std::string>& columns);
void writeFitsNumericTable(const std::filesystem::path& file, const FitsNumericTable& columns);
}
