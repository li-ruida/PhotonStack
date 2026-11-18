#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace photonstack {
struct CalibrationMetadataIssue {
    std::string field, code, message;
};
// Absent fields stay absent. The field-key map records exactly which FITS
// keywords supplied a value; no filename, pixel-value or camera-model guesses.
struct CalibrationAcquisition {
    std::uint32_t width = 0, height = 0;
    unsigned channels = 0;
    int bitpix = 0;
    std::map<std::string, std::string> text;
    std::map<std::string, double> numeric;
    std::map<std::string, std::vector<std::string>> keys;
    std::vector<CalibrationMetadataIssue> issues;
};
CalibrationAcquisition inspectCalibrationAcquisition(const std::filesystem::path& path);
std::string calibrationFrameType(const CalibrationAcquisition& acquisition);

enum class CalibrationMasterRole { Bias, Dark, Flat, DarkFlat };
struct CalibrationMatchEvidence {
    std::string field, status, input, master, message;
};
struct CalibrationMatch {
    // compatible is only acquisition compatibility, not a pixel-quality test
    // or proof that an unverified file actually is a correctly prepared master.
    std::string status;
    std::vector<CalibrationMatchEvidence> evidence;
};
CalibrationMatch matchCalibrationAcquisition(const CalibrationAcquisition& input, const CalibrationAcquisition& master,
                                             CalibrationMasterRole role, double temperatureTolerance = 1.0);

struct CalibrationAcquisitionGroup {
    std::map<std::string, std::string> text;
    std::map<std::string, double> numeric;
    std::vector<std::size_t> members;
    std::vector<std::string> issues;
};
// Groups are organizational only: every member still needs independent master
// matching. Measured temperature varies within a group and is never averaged
// away. Missing target/date leaves a separate group, without filename guesses.
std::vector<CalibrationAcquisitionGroup>
groupCalibrationAcquisitions(const std::vector<CalibrationAcquisition>& inputs);
} // namespace photonstack
