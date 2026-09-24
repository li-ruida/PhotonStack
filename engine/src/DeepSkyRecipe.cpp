#include "photonstack/DeepSkyRecipe.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace photonstack {
namespace {
template <class T> std::string number(T value) {
    char buffer[128];
    const auto r = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (r.ec != std::errc()) throw std::invalid_argument("Recipe number cannot be represented");
    return {buffer, r.ptr};
}
template <class T> T parsedNumber(const std::string& text) {
    T value{};
    bool valid = false;
    if constexpr (std::is_integral_v<T>) {
        const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
        valid = r.ec == std::errc() && r.ptr == text.data() + text.size();
    } else {
        // Floating from_chars is unavailable at the App's macOS 14 deployment
        // target. Classic locale keeps the recipe independent of user locale.
        std::istringstream stream(text); stream.imbue(std::locale::classic());
        stream >> std::noskipws >> value;
        valid = !stream.fail() && stream.eof();
    }
    if (!valid || !std::isfinite(double(value))) throw std::invalid_argument("Expected a finite numeric recipe value: " + text);
    return value;
}
std::vector<std::string> split(const std::string& s, char separator) {
    std::vector<std::string> values;
    if (s.empty()) return values;
    std::size_t start = 0;
    for (;;) {
        const auto end = s.find(separator, start);
        values.push_back(s.substr(start, end == std::string::npos ? end : end - start));
        if (values.back().empty()) throw std::invalid_argument("Empty item in recipe list");
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return values;
}
struct Binding {
    DeepSkyRecipeField field;
    std::function<std::string()> get;
    std::function<void(const std::string&)> set;
};
struct Fields {
    std::vector<Binding> bindings;
    template <class T> void numeric(const char* key, const char* label, const char* zh, const char* group,
                                    T& value, double minimum, double maximum) {
        bindings.push_back({{key, label, zh, group, std::is_integral_v<T> ? "integer" : "number", number(value), minimum, maximum, {}},
            [&value] { return number(value); }, [&value, minimum, maximum, key](const std::string& s) {
                const auto v = parsedNumber<T>(s);
                if (double(v) < minimum || double(v) > maximum) throw std::invalid_argument(std::string(key) + " is outside its supported range");
                value = v;
            }});
    }
    void boolean(const char* key, const char* label, const char* zh, const char* group, bool& value) {
        bindings.push_back({{key, label, zh, group, "bool", value ? "on" : "off", 0, 1, {"off", "on"}},
            [&value] { return value ? "on" : "off"; }, [&value](const std::string& s) {
                if (s != "on" && s != "off") throw std::invalid_argument("Boolean recipe values must be on or off");
                value = s == "on";
            }});
    }
    template <class T> void choice(const char* key, const char* label, const char* zh, const char* group,
                                   T& value, std::vector<std::pair<std::string, T>> choices) {
        DeepSkyRecipeField field{key, label, zh, group, "choice", "", 0, 0, {}};
        for (const auto& [name, v] : choices) { field.choices.push_back(name); if (v == value) field.defaultValue = name; }
        if (field.defaultValue.empty()) throw std::invalid_argument("Recipe contains unsupported enum value");
        bindings.push_back({field, [&value, choices] {
            for (const auto& [name, v] : choices) if (v == value) return name;
            throw std::invalid_argument("Recipe contains unsupported enum value");
        }, [&value, choices](const std::string& s) {
            for (const auto& [name, v] : choices) if (name == s) { value = v; return; }
            throw std::invalid_argument("Unsupported recipe choice: " + s);
        }});
    }
    void text(const char* key, const char* label, const char* zh, const char* group,
              std::function<std::string()> get, std::function<void(const std::string&)> set) {
        bindings.push_back({{key, label, zh, group, "text", get(), 0, 0, {}}, std::move(get), std::move(set)});
    }
};

std::vector<Binding> fields(DeepSkyWorkflowOptions& o) {
    Fields f;
    const auto pathList = [&](const char* key, const char* en, const char* zh, std::vector<std::filesystem::path>& paths) {
        f.text(key, en, zh, "calibration", [&paths] {
            std::ostringstream out;
            for (std::size_t i = 0; i < paths.size(); ++i) { if (i) out << ' '; out << std::quoted(paths[i].string()); }
            return out.str();
        }, [&paths](const std::string& text) {
            std::istringstream in(text); paths.clear();
            while (in >> std::ws && !in.eof()) {
                std::string path;
                if (in.peek() != '"' || !(in >> std::quoted(path)) || path.empty() || path.find_first_of("\r\n") != std::string::npos)
                    throw std::invalid_argument("Calibration paths require quoted paths separated by spaces");
                paths.emplace_back(path);
                if (paths.size() > 2000) throw std::invalid_argument("Too many calibration inputs");
            }
        });
    };
    pathList("calibration.darks", "Dark paths (quoted list)", "暗场路径（逐项双引号）", o.darks);
    pathList("calibration.biases", "Bias paths (quoted list)", "偏置路径（逐项双引号）", o.biases);
    pathList("calibration.flats", "Flat paths (quoted list)", "平场路径（逐项双引号）", o.flats);
    f.choice<MasterFrameMethod>("calibration.master-method", "Master method", "校准主帧合成方式", "calibration", o.masterMethod,
        {{"median", MasterFrameMethod::Median}, {"average", MasterFrameMethod::Average}});
    f.choice<CalibrationBiasState>("calibration.dark-bias", "Dark bias state", "暗场偏置状态", "calibration", o.darkBiasState,
        {{"unknown", CalibrationBiasState::Unknown}, {"included", CalibrationBiasState::Included}, {"removed", CalibrationBiasState::Removed}});
    f.choice<CalibrationBiasState>("calibration.flat-bias", "Flat bias state", "平场偏置状态", "calibration", o.flatBiasState,
        {{"unknown", CalibrationBiasState::Unknown}, {"included", CalibrationBiasState::Included}, {"removed", CalibrationBiasState::Removed}});
    f.boolean("selection.measure-trails", "Detect sampled transient lines", "检测采样中的瞬态线条", "selection", o.quality.measureTrails);
    f.choice<FitsDemosaic>("decode.demosaic", "FITS demosaic", "FITS 去马赛克", "decode", o.stack.fitsDemosaic,
        {{"bilinear", FitsDemosaic::Bilinear}, {"malvar", FitsDemosaic::Malvar}, {"menon", FitsDemosaic::Menon}, {"ratio", FitsDemosaic::Ratio}});
    f.boolean("decode.debayer", "Decode FITS CFA", "解码 FITS 彩色阵列", "decode", o.stack.debayerFits);
    f.choice<RawWhiteBalanceMode>("raw.white-balance", "RAW white balance", "RAW 白平衡", "decode", o.stack.raw.whiteBalanceMode,
        {{"camera", RawWhiteBalanceMode::Camera}, {"auto", RawWhiteBalanceMode::Auto}, {"daylight", RawWhiteBalanceMode::Daylight}, {"manual", RawWhiteBalanceMode::Manual}});
    f.numeric("raw.temperature", "RAW temperature", "RAW 色温", "decode", o.stack.raw.manualWhiteBalanceTemperature, 2000, 50000);
    f.numeric("raw.tint", "RAW tint", "RAW 色调", "decode", o.stack.raw.manualWhiteBalanceTint, -150, 150);
    f.numeric("raw.exposure", "RAW exposure", "RAW 曝光补偿", "decode", o.stack.raw.exposureBias, -10, 10);
    f.choice<RawBlackLevelMode>("raw.black-level", "RAW black level", "RAW 黑电平", "decode", o.stack.raw.blackLevelMode,
        {{"camera", RawBlackLevelMode::Camera}, {"auto", RawBlackLevelMode::Auto}, {"manual", RawBlackLevelMode::Manual}});
    f.numeric("raw.manual-black", "RAW manual black", "RAW 手动黑电平", "decode", o.stack.raw.manualBlackLevel, 0, 1);
    f.choice<RawDemosaicQuality>("raw.demosaic", "RAW demosaic", "RAW 去马赛克质量", "decode", o.stack.raw.demosaicQuality,
        {{"fast", RawDemosaicQuality::Fast}, {"balanced", RawDemosaicQuality::Balanced}, {"high", RawDemosaicQuality::High}});
    for (std::size_t c = 0; c < 3; ++c) {
        static const char* keys[]{"decode.cfa-red", "decode.cfa-green", "decode.cfa-blue"};
        static const char* en[]{"CFA red gain", "CFA green gain", "CFA blue gain"};
        static const char* zh[]{"CFA 红增益", "CFA 绿增益", "CFA 蓝增益"};
        f.numeric(keys[c], en[c], zh[c], "decode", o.stack.cfaInterpolationGains[c], .1, 10);
    }
    f.text("registration.mode", "Alignment model", "配准模型", "registration", [&o] {
        return o.stack.alignDistortion ? "distortion" : o.stack.alignAffine ? "affine" : o.stack.alignSimilarity ? "similarity" : o.stack.alignTranslation ? "translation" : "none";
    }, [&o](const std::string& s) {
        if (s != "none" && s != "translation" && s != "similarity" && s != "affine" && s != "distortion") throw std::invalid_argument("Unsupported alignment model");
        o.stack.alignTranslation = s == "translation"; o.stack.alignSimilarity = s == "similarity";
        o.stack.alignAffine = s == "affine"; o.stack.alignDistortion = s == "distortion";
    });
    f.bindings.back().field.type = "choice";
    f.bindings.back().field.choices = {"none", "translation", "similarity", "affine", "distortion"};
    f.choice<RegistrationInterpolation>("registration.interpolation", "Interpolation", "插值方式", "registration", o.stack.interpolation,
        {{"bilinear", RegistrationInterpolation::Bilinear}, {"bicubic", RegistrationInterpolation::Bicubic}});
    f.numeric("registration.reference", "Reference index (-1 auto)", "参考帧序号（-1 自动）", "registration", o.referenceIndex, -1, 1999);
    f.boolean("calibration.sensor-pattern", "Sensor fixed-pattern correction", "固定纹理校正（需视场位移，实验性）", "calibration", o.sensorPattern);
    f.numeric("registration.minimum-matches", "Minimum star matches", "最少匹配星点", "registration", o.stack.registration.minimumMatches, 3, 10000);
    f.numeric("registration.match-tolerance", "Match tolerance", "星点匹配容差", "registration", o.stack.registration.matchTolerance, .1, 100);
    f.boolean("registration.refine-centroids", "Refine centroids", "细化星点中心", "registration", o.stack.registration.refineSimilarityCentroids);
    f.choice<StackMethod>("stack.method", "Stack method", "合成方法", "stack", o.stack.method,
        {{"average", StackMethod::Average}, {"weighted", StackMethod::WeightedAverage}, {"median", StackMethod::Median},
         {"sigma", StackMethod::SigmaClip}, {"winsorized", StackMethod::WinsorizedSigmaClip}, {"percentile", StackMethod::PercentileClip}});
    f.boolean("stack.normalize-background", "Normalize frame sky", "逐帧背景归一化", "stack", o.stack.normalizeBackground);
    f.boolean("stack.median-mad", "Robust iterative MAD (sigma)", "迭代 MAD 剔除（Sigma）", "stack", o.stack.sigma.medianMad);
    f.numeric("stack.sigma-low", "Negative rejection sigma", "负向剔除阈值", "stack", o.stack.sigma.sigmaLow, .1, 20);
    f.numeric("stack.sigma-high", "Positive rejection sigma", "正向剔除阈值", "stack", o.stack.sigma.sigmaHigh, .1, 20);
    f.numeric("stack.iterations", "Rejection iterations", "剔除迭代次数", "stack", o.stack.sigma.iterations, 1, 10);
    f.numeric("stack.percentile-low", "Lower percentile", "低端百分位", "stack", o.stack.percentile.low, 0, 1);
    f.numeric("stack.percentile-high", "Upper percentile", "高端百分位", "stack", o.stack.percentile.high, 0, 1);
    f.boolean("stack.rejection-maps", "Save signed rejection maps", "保存正负剔除图", "stack", o.stack.collectRejectionMaps);
    f.text("stack.weights", "Frame weights (comma-separated, empty = equal)", "逐帧权重（逗号分隔，留空等权）", "stack", [&o] {
        std::string out; for (auto v : o.stack.frameWeights) { if (!out.empty()) out += ','; out += number(v); } return out;
    }, [&o](const std::string& s) {
        o.stack.frameWeights.clear(); for (const auto& item : split(s, ',')) {
            const float v = parsedNumber<float>(item); if (v <= 0) throw std::invalid_argument("Frame weights must be positive"); o.stack.frameWeights.push_back(v);
        }
    });
    f.boolean("selection.apply", "Apply recommended frame selection", "应用建议筛片结果", "selection", o.quality.applySelection);
    f.numeric("selection.sample-side", "Analysis grid side", "分析采样边长", "selection", o.quality.sampleSide, 8, 512);
    f.boolean("selection.measure-stars", "Measure stellar quality", "测量星点质量", "selection", o.quality.measureStars);
    f.numeric("selection.residual-sigma", "Residual significance", "残差显著性阈值", "selection", o.quality.residualSigma, 2, 20);
    f.numeric("selection.signal-tolerance", "Signal-dependent tolerance", "亮信号残差容差", "selection", o.quality.signalTolerance, 0, 1);
    f.numeric("selection.minimum-coverage", "Minimum valid coverage", "最低有效覆盖率", "selection", o.quality.minimumCoverage, 0, 1);
    f.numeric("selection.maximum-noise-ratio", "Maximum relative noise", "最大相对噪声", "selection", o.quality.maximumNoiseRatio, 1, 100);
    f.numeric("selection.maximum-fwhm-ratio", "Maximum relative FWHM", "最大相对星点宽度", "selection", o.quality.maximumFwhmRatio, 1, 100);
    f.numeric("selection.maximum-eccentricity", "Maximum eccentricity", "最大星点偏心率", "selection", o.quality.maximumEccentricity, 0, 1);
    f.numeric("selection.minimum-star-ratio", "Minimum relative star count", "最低相对星点数量", "selection", o.quality.minimumStarRatio, 0, 1);
    f.numeric("selection.maximum-residual-fraction", "Broad residual fraction", "大范围残差比例阈值", "selection", o.quality.maximumResidualFraction, 0, 1);
    f.numeric("selection.minimum-peers", "Minimum temporal peers", "最少时间比较帧", "selection", o.quality.minimumTemporalPeers, 4, 1999);
    f.numeric("selection.minimum-kept", "Minimum retained frames", "最少保留帧数", "selection", o.quality.minimumKeptFrames, 3, 2000);
    f.numeric("selection.minimum-kept-fraction", "Minimum retained fraction", "最低保留比例", "selection", o.quality.minimumKeptFraction, 0, 1);
    f.text("selection.overrides", "Per-frame auto/keep/reject", "逐帧 auto/keep/reject 覆盖", "selection", [&o] {
        std::string out; for (auto v : o.quality.overrides) { if (!out.empty()) out += ','; out += v == FrameSelectionOverride::Keep ? "keep" : v == FrameSelectionOverride::Reject ? "reject" : "auto"; } return out;
    }, [&o](const std::string& s) {
        o.quality.overrides.clear(); for (const auto& item : split(s, ',')) {
            if (item != "auto" && item != "keep" && item != "reject") throw std::invalid_argument("Selection override must be auto, keep or reject");
            o.quality.overrides.push_back(item == "keep" ? FrameSelectionOverride::Keep : item == "reject" ? FrameSelectionOverride::Reject : FrameSelectionOverride::Automatic);
        }
    });
    f.choice<DeepSkyBackground>("background.model", "Background model", "背景模型", "background", o.background,
        {{"none", DeepSkyBackground::None}, {"polynomial", DeepSkyBackground::Polynomial}, {"grid", DeepSkyBackground::Grid}, {"polynomial-grid", DeepSkyBackground::PolynomialAndGrid}});
    f.numeric("background.polynomial-strength", "Polynomial strength", "多项式校正强度", "background", o.polynomial.extraction.strength, 0, 1);
    f.numeric("background.polynomial-columns", "Polynomial sample columns", "多项式采样列数", "background", o.polynomial.columns, 4, 512);
    f.numeric("background.polynomial-rows", "Polynomial sample rows", "多项式采样行数", "background", o.polynomial.rows, 4, 512);
    f.numeric("background.grid-strength", "Grid strength", "网格校正强度", "background", o.grid.extraction.strength, 0, 1);
    f.numeric("background.grid-columns", "Grid columns", "背景网格列数", "background", o.grid.columns, 2, 128);
    f.numeric("background.grid-rows", "Grid rows", "背景网格行数", "background", o.grid.rows, 2, 128);
    f.boolean("background.extrapolate-edges", "Continue background to edges", "延伸背景模型至边缘", "background", o.grid.extrapolateEdges);
    f.boolean("background.protect-targets", "Prefer darker background samples", "优先用较暗像素估计背景", "background", o.grid.protectBrightTargets);
    f.text("background.exclusions", "Target ellipses: x,y,a,b,angle;...", "目标保护椭圆：x,y,半轴a,半轴b,角度；多组用英文分号", "background", [&o] {
        std::string out; for (const auto& e : o.polynomial.exclusions) { if (!out.empty()) out += ';'; out += number(e.x) + ',' + number(e.y) + ',' + number(e.major) + ',' + number(e.minor) + ',' + number(e.angleDegrees); } return out;
    }, [&o](const std::string& s) {
        o.polynomial.exclusions.clear(); for (const auto& item : split(s, ';')) {
            const auto v = split(item, ','); if (v.size() != 5) throw std::invalid_argument("Each exclusion ellipse needs five numbers");
            BackgroundExclusionEllipse e{parsedNumber<float>(v[0]), parsedNumber<float>(v[1]), parsedNumber<float>(v[2]), parsedNumber<float>(v[3]), parsedNumber<float>(v[4])};
            if (e.major <= 0 || e.minor <= 0) throw std::invalid_argument("Ellipse semiaxes must be positive"); o.polynomial.exclusions.push_back(e);
        }
    });
    f.numeric("crop.coverage", "Crop to minimum coverage (0 off)", "按覆盖率裁边（0 关闭）", "crop", o.cropCoverage, 0, 1);
    f.numeric("crop.inset", "Additional border inset (pixels)", "额外内缩边距（像素）", "crop", o.cropInset, 0, 10000);
    f.numeric("denoise.h", "Linear NLM h (-1 auto, 0 off)", "线性非局部降噪（-1 自动，0 关闭）", "denoise", o.denoiseH, -1, 1000000);
    f.choice<DeepSkyNoiseModel>("denoise.noise-model", "Luminance noise estimate", "亮度噪声估计（独立估计需关闭非局部降噪）", "denoise", o.noiseModel,
        {{"scene", DeepSkyNoiseModel::Scene}, {"independent-luminance", DeepSkyNoiseModel::IndependentLuminance}});
    f.numeric("denoise.blend", "Maximum denoise blend", "降噪最大混合比例", "denoise", o.denoiseBlend, 0, 1);
    f.numeric("denoise.multiscale-luminance", "Multiscale luminance denoise", "多尺度亮度降噪", "denoise", o.multiscale.luminance, 0, 1);
    f.numeric("denoise.multiscale-chroma", "Multiscale color denoise", "多尺度彩色降噪", "denoise", o.multiscale.chroma, 0, 1);
    f.numeric("denoise.multiscale-scales", "Denoising scales", "降噪尺度数", "denoise", o.multiscale.scales, 1, 6);
    f.numeric("denoise.background-strength", "Residual background denoise", "背景纹理降噪", "denoise", o.backgroundDenoiseStrength, 0, 1);
    f.choice<AstroToneCurve>("develop.curve", "Tone curve", "显影曲线", "develop", o.develop.toneCurve,
        {{"asinh", AstroToneCurve::Asinh}, {"rational", AstroToneCurve::Rational}});
    f.boolean("develop.align-channels", "Automatic RGB channel alignment", "自动对齐 RGB 通道（可靠星点才启用）", "develop", o.alignChannels);
    f.numeric("develop.shadow-neutralization", "Low-SNR background color suppression", "低信噪比背景彩色抑制", "develop", o.develop.shadowNeutralization, 0, 1);
    f.boolean("develop.stellar-balance", "Statistical stellar balance", "恒星统计白平衡", "develop", o.develop.stellarBalance);
    f.numeric("develop.red-gain", "Red gain", "红色增益", "develop", o.develop.gains[0], .1, 10);
    f.numeric("develop.green-gain", "Green gain", "绿色增益", "develop", o.develop.gains[1], .1, 10);
    f.numeric("develop.blue-gain", "Blue gain", "蓝色增益", "develop", o.develop.gains[2], .1, 10);
    f.numeric("develop.background", "Display background", "显示背景亮度", "develop", o.develop.background, 0, .3);
    f.numeric("develop.tone-scale", "Tone scale (0 auto)", "拉伸尺度（0 自动）", "develop", o.develop.toneScale, 0, 1000000000);
    f.numeric("develop.white-point", "Highlight reference (0 auto)", "高光参考（0 自动）", "develop", o.develop.whitePoint, 0, 1000000000);
    f.numeric("develop.brightness", "Brightness", "亮度", "develop", o.develop.brightness, .1, 10);
    f.numeric("develop.saturation", "Saturation", "色彩浓度", "develop", o.develop.saturation, 0, 2);
    f.numeric("develop.star-exposure", "Star exposure", "恒星曝光", "develop", o.develop.starExposure, .05, 1);
    f.numeric("develop.star-threshold", "Bright-star threshold", "亮星门槛", "develop", o.develop.starPeakThreshold, 0, 1);
    f.boolean("output.save-denoise-stages", "Save denoising stage images", "保存各阶段降噪过程图", "output", o.saveDenoiseStages);
    return std::move(f.bindings);
}
} // namespace

std::vector<DeepSkyRecipeField> DeepSkyRecipe::schema() {
    DeepSkyWorkflowOptions defaults;
    std::vector<DeepSkyRecipeField> result;
    for (const auto& binding : fields(defaults)) result.push_back(binding.field);
    return result;
}

DeepSkyRecipe DeepSkyRecipe::parse(const std::string& text, const std::filesystem::path& base) {
    if (text.size() > 4 * 1024 * 1024 || text.find('\0') != std::string::npos) throw std::invalid_argument("Recipe exceeds size limit or contains NUL");
    std::istringstream stream(text);
    std::string line;
    if (!std::getline(stream, line) || line != "PHOTONSTACK_DEEP_SKY_RECIPE 1") throw std::invalid_argument("Unsupported deep-sky recipe version");
    DeepSkyRecipe recipe;
    auto bindings = fields(recipe.options);
    std::map<std::string, Binding*> lookup;
    for (auto& binding : bindings) lookup[binding.field.key] = &binding;
    std::set<std::string> seen;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string key, value, trailing;
        if (!(row >> key)) continue;
        row >> std::ws;
        if (row.peek() != '"' || !(row >> std::quoted(value)) || (row >> trailing)) throw std::invalid_argument("Expected one quoted value for " + key);
        if (key == "input") {
            if (value.empty()) throw std::invalid_argument("Input path cannot be empty");
            recipe.inputs.push_back(std::filesystem::absolute(base / value).lexically_normal());
            if (recipe.inputs.size() > 2000) throw std::invalid_argument("Too many recipe inputs");
        } else {
            if (!seen.insert(key).second) throw std::invalid_argument("Duplicate recipe field: " + key);
            const auto found = lookup.find(key);
            if (found == lookup.end()) throw std::invalid_argument("Unknown recipe field: " + key);
            found->second->set(value);
        }
    }
    auto& o = recipe.options;
    for (auto* paths : {&o.darks, &o.biases, &o.flats})
        for (auto& path : *paths) path = std::filesystem::absolute(base / path).lexically_normal();
    if ((!o.darks.empty() && !o.biases.empty() && o.darkBiasState == CalibrationBiasState::Unknown) ||
        (!o.flats.empty() && !o.biases.empty() && o.flatBiasState == CalibrationBiasState::Unknown))
        throw std::invalid_argument("Calibration with bias requires explicit dark/flat bias state");
    if (!o.flats.empty() && o.biases.empty() && o.flatBiasState == CalibrationBiasState::Included)
        throw std::invalid_argument("Bias-included flat requires bias frames");
    if (o.sensorPattern && (!o.darks.empty() || !o.biases.empty() || !o.flats.empty() || !o.stack.debayerFits))
        throw std::invalid_argument("Sensor pattern correction requires uncalibrated CFA FITS with debayer enabled");
    o.stack.winsorizedSigma = {o.stack.sigma.sigmaLow, o.stack.sigma.sigmaHigh};
    if (o.denoiseH < 0 && o.denoiseH != -1) throw std::invalid_argument("Denoise h must be -1, zero, or positive");
    if (o.stack.sigma.medianMad && o.stack.method != StackMethod::SigmaClip) throw std::invalid_argument("MAD rejection requires sigma method");
    if (o.stack.collectRejectionMaps && (o.stack.method == StackMethod::Average || o.stack.method == StackMethod::WeightedAverage)) throw std::invalid_argument("Average stacking has no rejection maps");
    if (o.stack.percentile.low >= o.stack.percentile.high) throw std::invalid_argument("Lower percentile must be smaller than upper percentile");
    if (o.stack.registration.refineSimilarityCentroids && !(o.stack.alignSimilarity || o.stack.alignAffine)) throw std::invalid_argument("Centroid refinement requires similarity or affine registration");
    if (!o.stack.frameWeights.empty() && o.stack.frameWeights.size() != recipe.inputs.size()) throw std::invalid_argument("One weight is required per input");
    if (!o.quality.overrides.empty() && o.quality.overrides.size() != recipe.inputs.size()) throw std::invalid_argument("One selection override is required per input");
    return recipe;
}

std::string DeepSkyRecipe::serialize() const {
    auto copy = options;
    std::ostringstream out;
    out << "PHOTONSTACK_DEEP_SKY_RECIPE 1\n";
    for (const auto& binding : fields(copy)) out << binding.field.key << ' ' << std::quoted(binding.get()) << '\n';
    for (const auto& input : inputs) {
        const auto path = input.string();
        if (path.find_first_of("\r\n") != std::string::npos || path.find('\0') != std::string::npos) throw std::invalid_argument("Recipe paths cannot contain line breaks or NUL");
        out << "input " << std::quoted(path) << '\n';
    }
    return out.str();
}
} // namespace photonstack
