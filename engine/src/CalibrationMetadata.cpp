#include "photonstack/CalibrationMetadata.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace photonstack {
namespace {
std::string number(double x) {
    std::ostringstream out;
    out << std::setprecision(17) << x;
    return out.str();
}
std::string kind(const CalibrationAcquisition& a) {
    if (!a.text.contains("frameType"))
        return "";
    std::string s;
    for (unsigned char c : a.text.at("frameType"))
        if (std::isalnum(c))
            s += static_cast<char>(std::toupper(c));
    if (s.starts_with("MASTER"))
        s.erase(0, 6);
    if (s.ends_with("FRAME"))
        s.resize(s.size() - 5);
    if (s == "DARKFLAT" || s == "FLATDARK")
        return "dark-flat";
    if (s == "LIGHT")
        return "light";
    if (s == "DARK")
        return "dark";
    if (s == "FLAT")
        return "flat";
    if (s == "BIAS" || s == "OFFSET")
        return "bias";
    return "";
}
std::string utcDate(const CalibrationAcquisition& a) {
    if (!a.text.contains("observedAt"))
        return "";
    if (a.text.contains("timeSystem") && a.text.at("timeSystem") != "UTC")
        return "";
    const auto& s = a.text.at("observedAt");
    if (s.size() < 10 || s[4] != '-' || s[7] != '-')
        return "";
    auto digits = [&](std::size_t start, std::size_t count) {
        return start + count <= s.size() && s.substr(start, count).find_first_not_of("0123456789") == std::string::npos;
    };
    if (!digits(0, 4) || !digits(5, 2) || !digits(8, 2))
        return "";
    const int year = std::stoi(s.substr(0, 4));
    if (year < 1972 ||
        !std::chrono::year_month_day(std::chrono::year(year), std::chrono::month(std::stoi(s.substr(5, 2))),
                                     std::chrono::day(std::stoi(s.substr(8, 2))))
             .ok())
        return "";
    if (s.size() > 10) {
        if (s.size() < 19 || s[10] != 'T' || s[13] != ':' || s[16] != ':' || !digits(11, 2) || !digits(14, 2) ||
            !digits(17, 2))
            return "";
        const int h = std::stoi(s.substr(11, 2)), m = std::stoi(s.substr(14, 2)), sec = std::stoi(s.substr(17, 2));
        if (h > 23 || m > 59 || sec > 60 || (sec == 60 && (h != 23 || m != 59)))
            return "";
        // FITS datetime strings have no timezone suffix. TIMESYS carries it.
        if (s.size() > 19 && (s[19] != '.' || s.size() == 20 || !digits(20, s.size() - 20)))
            return "";
    }
    return s.substr(0, 10);
}
} // namespace
std::string calibrationFrameType(const CalibrationAcquisition& acquisition) {
    return kind(acquisition);
}
std::vector<CalibrationAcquisitionGroup>
groupCalibrationAcquisitions(const std::vector<CalibrationAcquisition>& inputs) {
    std::map<std::string, CalibrationAcquisitionGroup> groups;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto& a = inputs[i];
        CalibrationAcquisitionGroup group;
        for (const auto* key : {"object", "camera", "cameraSerial", "filter", "opticalTrain", "readoutMode", "sampling",
                                "units", "domain"})
            if (a.text.contains(key))
                group.text[key] = a.text.at(key);
        for (const auto* key : {"exposure", "gain", "iso", "offset", "binX", "binY", "originX", "originY",
                                "focalLength", "electronsPerADU"})
            if (a.numeric.contains(key))
                group.numeric[key] = a.numeric.at(key);
        group.numeric["width"] = a.width;
        group.numeric["height"] = a.height;
        group.numeric["channels"] = a.channels;
        const auto date = utcDate(a);
        if (date.empty())
            group.issues.push_back("UTC date is missing, invalid or uses an unsupported time scale");
        else
            group.text["utcDate"] = date;
        if (!a.text.contains("object") || a.text.at("object").empty())
            group.issues.push_back("Target identity is missing");
        if (!a.issues.empty())
            group.issues.push_back("Acquisition metadata contains invalid or conflicting fields");
        std::ostringstream key;
        key << std::setprecision(17);
        for (const auto& [k, v] : group.text)
            key << std::quoted(k) << ' ' << std::quoted(v) << '\n';
        for (const auto& [k, v] : group.numeric)
            key << std::quoted(k) << ' ' << v << '\n';
        // Do not merge malformed or unidentifiable inputs into a plausible group.
        if (!group.issues.empty())
            key << "unverified-member " << i;
        const auto [entry, inserted] = groups.try_emplace(key.str(), std::move(group));
        entry->second.members.push_back(i);
    }
    std::vector<CalibrationAcquisitionGroup> result;
    for (auto& [key, group] : groups)
        result.push_back(std::move(group));
    return result;
}
CalibrationMatch matchCalibrationAcquisition(const CalibrationAcquisition& input, const CalibrationAcquisition& master,
                                             CalibrationMasterRole role, double tolerance) {
    if (!std::isfinite(tolerance) || tolerance < 0 || tolerance > 10)
        throw std::invalid_argument("Temperature tolerance must be finite and within 0..10 Celsius");
    if (role != CalibrationMasterRole::Bias && role != CalibrationMasterRole::Dark &&
        role != CalibrationMasterRole::Flat && role != CalibrationMasterRole::DarkFlat)
        throw std::invalid_argument("Unknown calibration master role");
    CalibrationMatch r{"compatible", {}};
    auto add = [&](const std::string& field, const std::string& status, const std::string& a, const std::string& b,
                   const std::string& why) {
        r.evidence.push_back({field, status, a, b, why});
        if (status == "conflict" || status == "invalid")
            r.status = "incompatible";
        else if (status == "missing" && r.status == "compatible")
            r.status = "review";
    };
    for (const auto& i : input.issues)
        add(i.field, "invalid", i.code, "", "Input metadata: " + i.message);
    for (const auto& i : master.issues)
        add(i.field, "invalid", "", i.code, "Master metadata: " + i.message);
    for (const auto* source : {&input, &master})
        for (const auto& [field, value] : source->numeric) {
            bool valid = std::isfinite(value);
            if (field == "binX" || field == "binY")
                valid &= value >= 1 && value <= 65535 && std::floor(value) == value;
            if (field == "originX" || field == "originY")
                valid &= value >= 0 && value <= 1e9 && std::floor(value) == value;
            if (field == "gain" || field == "offset" || field == "exposure")
                valid &= value >= 0;
            if (field == "iso" || field == "scale" || field == "electronsPerADU")
                valid &= value > 0;
            if (field == "sensorTemperature")
                valid &= value >= -273.15 && value <= 200;
            if (!valid)
                add(field, "invalid", "", "", "Invalid acquisition value");
        }
    auto text = [](const auto& a, const std::string& field) {
        const auto it = a.text.find(field);
        return it == a.text.end() ? std::string{} : it->second;
    };
    auto compareText = [&](const std::string& field) {
        const auto a = text(input, field), b = text(master, field);
        if (a.empty() || b.empty())
            add(field, "missing", a, b, "Missing metadata is not proof of a match");
        else
            add(field, a == b ? "match" : "conflict", a, b, "Explicit acquisition identity must agree");
    };
    auto compareNumber = [&](const std::string& field, double delta = 0) {
        const auto a = input.numeric.find(field), b = master.numeric.find(field);
        if (a == input.numeric.end() || b == master.numeric.end())
            add(field, "missing", a == input.numeric.end() ? "" : number(a->second),
                b == master.numeric.end() ? "" : number(b->second), "Missing metadata is not proof of a match");
        else if (!std::isfinite(a->second) || !std::isfinite(b->second))
            add(field, "invalid", "", "", "Nonfinite acquisition value");
        else
            add(field, std::abs(a->second - b->second) <= delta ? "match" : "conflict", number(a->second),
                number(b->second),
                delta == 0 ? "Acquisition settings must agree exactly"
                           : "Allowed absolute difference: " + number(delta));
    };
    add("width", input.width == master.width && input.width > 0 ? "match" : "conflict", number(input.width),
        number(master.width), "Stored sensor width");
    add("height", input.height == master.height && input.height > 0 ? "match" : "conflict", number(input.height),
        number(master.height), "Stored sensor height");
    add("channels", input.channels == 1 && master.channels == 1 ? "match" : "conflict", number(input.channels),
        number(master.channels), "Sensor calibration requires single-plane data before interpolation");
    for (const auto& f : {"camera", "cameraSerial", "sampling", "domain", "units", "readoutMode"})
        compareText(f);
    if ((!text(input, "domain").empty() && text(input, "domain") != "linear") ||
        (!text(master, "domain").empty() && text(master, "domain") != "linear"))
        add("domain", "conflict", text(input, "domain"), text(master, "domain"),
            "Display-encoded pixels cannot be calibrated as linear data");
    for (const auto& f : {"binX", "binY", "originX", "originY", "offset"})
        compareNumber(f);
    bool gainKnown = false;
    for (const auto& f : {"gain", "iso"})
        if (input.numeric.contains(f) || master.numeric.contains(f)) {
            compareNumber(f);
            gainKnown = true;
        }
    if (!gainKnown)
        add("gainOrISO", "missing", "", "", "GAIN or ISOSPEED is required; EGAIN is not camera gain");
    if (input.numeric.contains("electronsPerADU") || master.numeric.contains("electronsPerADU"))
        compareNumber("electronsPerADU");
    if (role != CalibrationMasterRole::Flat)
        compareNumber("sensorTemperature", tolerance);
    else
        add("sensorTemperature", "not-required", "", "",
            "Flat response matching does not select by sensor temperature; dark correction of the flat is separate");
    if (role == CalibrationMasterRole::Dark || role == CalibrationMasterRole::DarkFlat) {
        compareNumber("exposure", 1e-6);
        if ((input.numeric.contains("exposure") && input.numeric.at("exposure") <= 0) ||
            (master.numeric.contains("exposure") && master.numeric.at("exposure") <= 0))
            add("exposure", "invalid", "", "", "Dark matching requires positive exposure times");
    } else
        add("exposure", "not-required", "", "",
            "Bias and flat exposure need not equal the input; no dark scaling is implied");
    if (role == CalibrationMasterRole::Flat) {
        compareText("filter");
        compareText("opticalTrain");
        if (input.numeric.contains("focalLength") || master.numeric.contains("focalLength"))
            compareNumber("focalLength");
    } else
        add("filter", "not-required", text(input, "filter"), text(master, "filter"),
            "Dark/bias matching does not require the same filter");
    const auto a = kind(input), b = kind(master);
    const std::string expected = role == CalibrationMasterRole::Bias   ? "bias"
                                 : role == CalibrationMasterRole::Flat ? "flat"
                                                                       : "dark";
    const bool masterCompatible = b.empty() || b == expected || (expected == "dark" && b == "dark-flat");
    const bool inputCompatible = a.empty() || a == "light" || (a == "flat" && role != CalibrationMasterRole::Flat);
    if (!masterCompatible || !inputCompatible || (role == CalibrationMasterRole::DarkFlat && !a.empty() && a != "flat"))
        add("frameType", "conflict", a, b, "Declared frame type contradicts the requested calibration role");
    else
        add("frameType", "declared", a, b,
            "Master role was explicitly supplied; header labels do not prove master construction or bias state");
    return r;
}
} // namespace photonstack
