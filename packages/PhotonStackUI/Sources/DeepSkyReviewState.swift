import Foundation
import PhotonStackAppCore

enum DeepSkyReviewFilter: String, CaseIterable {
    case all, attention, excluded, manual
    func title(chinese: Bool) -> String {
        switch self {
        case .all: chinese ? "全部" : "All"
        case .attention: chinese ? "需关注" : "Attention"
        case .excluded: chinese ? "已排除" : "Excluded"
        case .manual: chinese ? "手动标记" : "Manual"
        }
    }
}

enum DeepSkyReviewSort: String, CaseIterable {
    case attention, capture, fwhm, noise, coverage
    func title(chinese: Bool) -> String {
        switch self {
        case .attention: chinese ? "异常优先" : "Attention first"
        case .capture: chinese ? "拍摄顺序" : "Capture order"
        case .fwhm: chinese ? "星点从大到小" : "Largest stars first"
        case .noise: chinese ? "噪声从高到低" : "Noisiest first"
        case .coverage: chinese ? "覆盖率从低到高" : "Lowest coverage first"
        }
    }
}

/// Presentation of current decisions, separate from the immutable last-run report.
enum DeepSkyReview {
    static func starCountText(_ frame: DeepSkyReport.Frame) -> String {
        guard frame.starsMeasured else { return "—" }
        if frame.starCountEstimator == nil && frame.starCount >= 1000 { return "≥\(frame.starCount)" }
        return String(frame.starCount)
    }

    static func starCountHelp(_ frame: DeepSkyReport.Frame, chinese: Bool) -> String {
        if frame.starsMeasured && frame.starCountEstimator == nil && frame.starCount >= 1000 {
            return chinese ? "旧分析在 1000 个星点处停止计数；重新分析可获得完整检测数量。" : "The old analysis stopped counting at 1,000 stars. Analyze again for the complete detection count."
        }
        return chinese ? "受限尺寸分析图上的去重检测数量，用于同组曝光比较；不是原片全部恒星的数量。" : "Separated detections on the bounded analysis image, for comparing exposures; not a census of all stars in the original."
    }

    static func kept(_ frame: DeepSkyReport.Frame, settings: DeepSkySettings) -> Bool {
        guard frame.usable else { return false }
        switch settings.overrides[frame.input] {
        case "keep": return true
        case "reject": return false
        default: return settings.values["selection.apply"] == "off" || frame.recommendedKeep
        }
    }
    static func attention(_ frame: DeepSkyReport.Frame) -> Bool {
        !frame.usable || !frame.recommendedKeep || !frame.warnings.isEmpty
    }
    static func manual(_ frame: DeepSkyReport.Frame, settings: DeepSkySettings) -> Bool {
        ["keep", "reject"].contains(settings.overrides[frame.input] ?? "auto")
    }
    static func reason(_ code: String, chinese: Bool) -> String {
        guard chinese else { return code.replacingOccurrences(of: "-", with: " ") }
        return ["low-coverage": "覆盖率不足", "high-noise": "噪声偏高", "broad-stars": "星点偏大",
                "elongated-stars": "星点拉长", "few-stars": "可测星点偏少", "broad-temporal-residuals": "大范围帧间异常",
                "user-excluded": "手动排除", "RegistrationRejected": "对齐不可靠",
                "insufficient-temporal-peers": "有效比较帧不足", "stellar-measurements-unavailable": "无法测量星点",
                "stellar-shapes-unavailable": "原尺寸星形样本不足",
                "localized-transients-use-pixel-rejection": "局部异常，建议像素剔除",
                "signed-linear-transient-candidate": "瞬态线条候选"][code] ?? code
    }
    static func reasons(_ frame: DeepSkyReport.Frame, chinese: Bool) -> [String] {
        ([frame.errorCode] + frame.reasons + frame.warnings)
            .filter { !$0.isEmpty && $0 != "user-excluded" }.map { reason($0, chinese: chinese) }
    }
    static func frames(_ frames: [DeepSkyReport.Frame], settings: DeepSkySettings,
                       filter: DeepSkyReviewFilter, query: String, sort: DeepSkyReviewSort) -> [DeepSkyReport.Frame] {
        frames.filter { frame in
            let matches: Bool
            switch filter {
            case .all: matches = true
            case .attention: matches = attention(frame)
            case .excluded: matches = !kept(frame, settings: settings)
            case .manual: matches = manual(frame, settings: settings)
            }
            let searchable = ([frame.input, String(frame.index + 1)] + reasons(frame, chinese: true) + reasons(frame, chinese: false)).joined(separator: " ")
            return matches && (query.isEmpty || searchable.localizedCaseInsensitiveContains(query))
        }.sorted { a, b in
            switch sort {
            case .attention:
                let rank: (DeepSkyReport.Frame) -> Int = { !$0.usable ? 3 : !$0.recommendedKeep ? 2 : attention($0) ? 1 : 0 }
                if rank(a) != rank(b) { return rank(a) > rank(b) }
            case .fwhm: if a.fwhm != b.fwhm { return a.fwhm > b.fwhm }
            case .noise: if a.noiseRatio != b.noiseRatio { return a.noiseRatio > b.noiseRatio }
            case .coverage: if a.coverage != b.coverage { return a.coverage < b.coverage }
            case .capture: break
            }
            return a.index < b.index
        }
    }
    static func sameAnalysisRecipe(_ current: String?, _ previous: String) -> Bool {
        guard let current else { return false }
        func withoutOverrides(_ text: String) -> [Substring] {
            text.split(separator: "\n").filter {
                !$0.hasPrefix("selection.overrides ") && $0 != "calibration.sensor-pattern \"off\""
                    && !$0.hasPrefix("output.save-denoise-stages ")
                    && !["background.", "crop.", "denoise.", "develop."].contains(where: $0.hasPrefix)
            }
        }
        return withoutOverrides(current) == withoutOverrides(previous)
    }
    /// An analysis can remain valid while its developed result needs updating.
    static func sameResultRecipe(_ current: String?, _ previous: String) -> Bool {
        guard let current else { return false }
        func resultLines(_ text: String) -> [Substring] {
            text.split(separator: "\n").filter { line in
                // Older recipes omit these default-valued options. Output
                // retention never changes the rendered image.
                if line == "calibration.sensor-pattern \"off\"" || line == "denoise.noise-model \"scene\""
                    || line.hasPrefix("output.save-denoise-stages ") { return false }
                if line.hasPrefix("selection.overrides ") {
                    let value = line.dropFirst("selection.overrides ".count).trimmingCharacters(in: CharacterSet(charactersIn: " \""))
                    if value.isEmpty || value.split(separator: ",").allSatisfy({ $0 == "auto" }) { return false }
                }
                return true
            }
        }
        return resultLines(current) == resultLines(previous)
    }
    /// Only finishing settings may change when reusing a scientific stack.
    static func sameStackRecipe(_ current: String?, _ previous: String) -> Bool {
        guard let current else { return false }
        func stackLines(_ text: String) -> [Substring] {
            text.split(separator: "\n").filter { line in
                // Older recipes predate this opt-in setting; omitted means off.
                if line == "calibration.sensor-pattern \"off\"" { return false }
                if line.hasPrefix("output.save-denoise-stages ") { return false }
                if ["background.", "crop.", "denoise.", "develop."].contains(where: { line.hasPrefix($0) }) { return false }
                if line.hasPrefix("selection.overrides ") {
                    let value = line.dropFirst("selection.overrides ".count).trimmingCharacters(in: CharacterSet(charactersIn: " \""))
                    if value.isEmpty || value.split(separator: ",").allSatisfy({ $0 == "auto" }) { return false }
                }
                return true
            }
        }
        return stackLines(current) == stackLines(previous)
    }

    static func minimumKept(settings: DeepSkySettings, count: Int) -> Int {
        let required = settings.values["calibration.sensor-pattern"] == "on" ? 36 :
            (settings.values["denoise.noise-model"] == "independent-luminance" ? 6 : 3)
        guard settings.values["selection.apply"] != "off" else { return required }
        let absolute = Int(settings.values["selection.minimum-kept"] ?? "3") ?? 3
        let fraction = Double(settings.values["selection.minimum-kept-fraction"] ?? "0.5") ?? 0.5
        guard fraction.isFinite, (0...1).contains(fraction) else { return max(required, absolute) }
        return max(required, absolute, Int(ceil(Double(count) * fraction)))
    }
}
