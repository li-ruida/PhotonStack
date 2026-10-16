import Foundation

public extension StackMethod {
    var isExtrema: Bool { self == .maximum || self == .minimum }
    var supportsRejectionMaps: Bool { [.median, .sigma, .winsorized, .percentile].contains(self) }

    func explanation(chinese: Bool) -> String {
        switch self {
        case .maximum:
            return chinese ? "逐通道保留最亮值，适合星轨；也会保留飞机灯线。星轨需关闭恒星对齐，去航迹需单独处理。" : "Keep the brightest value per channel for trails. Aircraft lights also remain; disable star alignment for trails and clean artifacts separately."
        case .minimum:
            return chinese ? "逐通道保留最暗值；会削弱移动亮点，也可能抹掉星轨和微弱星点。不是通用去飞机线方法。" : "Keep the darkest value per channel. Moving lights, trails and faint stars may disappear; not a general aircraft-removal method."
        case .average:
            return chinese ? "等权平均（可指定逐帧权重），降低随机噪声；不剔除异常亮线。" : "Average samples (optional explicit weights) to reduce random noise, without outlier rejection."
        case .weighted:
            return chinese ? "按图像质量加权平均；不剔除异常亮线。" : "Average with automatic quality weights, without outlier rejection."
        case .median:
            return chinese ? "取中位数，抑制少量异常值；不适合保留移动的星轨。" : "Take the median to suppress sparse outliers; not suitable for preserving moving trails."
        case .sigma:
            return chinese ? "阈值剔除异常值后平均，适合对齐后的深空序列；MAD 迭代仅此模式可用。" : "Reject outliers then average aligned deep-sky frames. Iterative MAD is available only in this mode."
        case .winsorized:
            return chinese ? "将异常值限制在 Sigma 阈值内再合成，保留样本贡献。" : "Bound outliers to Sigma thresholds before combining, retaining sample contributions."
        case .percentile:
            return chinese ? "按上下百分位剔除极端值后平均；上下界应满足低端小于高端。" : "Discard values outside the percentile interval, then average. Lower must be less than upper."
        }
    }
}

public extension DeepSkySettings {
    func effectiveValue(_ key: String, schema: DeepSkySchema) -> String {
        values[key] ?? schema.fields.first(where: { $0.key == key })?.defaultValue ?? ""
    }

    func fieldIsApplicable(_ key: String, schema: DeepSkySchema) -> Bool {
        let method = StackMethod(rawValue: effectiveValue("stack.method", schema: schema)) ?? .sigma
        let alignment = effectiveValue("registration.mode", schema: schema)
        switch key {
        case "stack.median-mad": return method == .sigma
        case "stack.iterations": return method == .sigma && effectiveValue("stack.median-mad", schema: schema) == "on"
        case "stack.sigma-low", "stack.sigma-high": return method == .sigma || method == .winsorized
        case "stack.percentile-low", "stack.percentile-high": return method == .percentile
        case "stack.rejection-maps": return method.supportsRejectionMaps
        case "stack.weights": return !method.isExtrema
        case "registration.refine-centroids": return alignment == "similarity" || alignment == "affine"
        case "registration.interpolation", "registration.minimum-matches", "registration.match-tolerance": return alignment != "none"
        case "denoise.h": return effectiveValue("denoise.noise-model", schema: schema) != "independent-luminance"
        default: return true
        }
    }

    /// Only interactive edits normalize dependent settings. Imported recipes remain
    /// strict and are validated by the engine, never silently rewritten on load.
    mutating func setCompatibleValue(_ value: String, for key: String, schema: DeepSkySchema) {
        values[key] = value
        if key == "stack.method", let method = StackMethod(rawValue: value) {
            if method != .sigma {
                values["stack.median-mad"] = "off"
                values["denoise.noise-model"] = "scene"
            }
            if !method.supportsRejectionMaps { values["stack.rejection-maps"] = "off" }
            if method.isExtrema { values["stack.weights"] = "" }
        }
        if key == "registration.mode", !["similarity", "affine"].contains(value) {
            values["registration.refine-centroids"] = "off"
        }
        if key == "denoise.noise-model", value == "independent-luminance" {
            if effectiveValue("stack.method", schema: schema) == "sigma" { values["denoise.h"] = "0" }
            else { values[key] = "scene" }
        }
    }

    /// A composition setup, not an automatic cleanup or finishing preset.
    mutating func configureStarTrailComposition(schema: DeepSkySchema) {
        setCompatibleValue("maximum", for: "stack.method", schema: schema)
        setCompatibleValue("none", for: "registration.mode", schema: schema)
        values["stack.normalize-background"] = "off"
        values["selection.apply"] = "off"
        // Explicit per-frame user decisions, calibration and finishing are retained.
    }
}
