import Foundation
import PhotonStackAppCore

enum DeepSkyReviewFilter: String, CaseIterable {
    case all, excluded, kept, attention, manual
    func title(chinese: Bool) -> String {
        switch self {
        case .all: chinese ? "全部" : "All"
        case .attention: chinese ? "需关注" : "Attention"
        case .excluded: chinese ? "已排除" : "Excluded"
        case .kept: chinese ? "已保留" : "Kept"
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

/// Snapshot the selection when opening the workbench or starting analysis.
/// Later manual edits can empty the visible scope without changing it.
struct DeepSkyReviewOpeningScope {
    private let settings: DeepSkySettings

    init(settings: DeepSkySettings) {
        var snapshot = DeepSkySettings()
        snapshot.values["selection.apply"] = settings.values["selection.apply"]
        snapshot.overrides = settings.overrides
        self.settings = snapshot
    }

    func filter(report: DeepSkyReport?, isCurrent: Bool, requested: DeepSkyReviewFilter?) -> DeepSkyReviewFilter {
        requested ?? DeepSkyReview.initialFilter(report: report, settings: settings, isCurrent: isCurrent)
    }
}

/// A current plan follows live overrides. A historical report retains its
/// recorded decisions even if the project now has different lights or settings.
struct DeepSkyReviewSelection {
    private let current: DeepSkySettings?
    private let recordedOverrides: [Substring]?
    let appliesRecommendations: Bool?
    var isCurrent: Bool { current != nil }

    init(settings: DeepSkySettings) {
        current = settings
        recordedOverrides = nil
        appliesRecommendations = settings.values["selection.apply"] != "off"
    }

    init(report: DeepSkyReport, settings: DeepSkySettings, isCurrent: Bool) {
        if isCurrent { self.init(settings: settings); return }
        self.init(recorded: report)
    }

    private init(recorded report: DeepSkyReport) {
        current = nil
        let lines = report.recipe.split(separator: "\n")
        func field(_ key: String) -> Substring? {
            let prefix = key + " \""
            guard let line = lines.first(where: { $0.hasPrefix(prefix) }), line.last == "\"" else { return nil }
            return line.dropFirst(prefix.count).dropLast()
        }
        switch field("selection.apply") {
        case "on": appliesRecommendations = true
        case "off": appliesRecommendations = false
        default: appliesRecommendations = nil
        }
        let overrides = field("selection.overrides")?.split(separator: ",", omittingEmptySubsequences: false)
        if overrides == [""] { recordedOverrides = [] }
        else if let overrides, overrides.count == report.inputCount,
                overrides.allSatisfy({ ["auto", "keep", "reject"].contains($0) }) {
            recordedOverrides = overrides
        } else { recordedOverrides = nil }
    }

    func decision(_ frame: DeepSkyReport.Frame) -> String {
        if let current { return DeepSkyReview.decision(frame, settings: current) }
        if frame.reasons.contains("user-excluded") { return "reject" }
        if let recordedOverrides {
            if recordedOverrides.isEmpty { return "auto" }
            if recordedOverrides.indices.contains(frame.index) { return String(recordedOverrides[frame.index]) }
        }
        return "unknown"
    }

    func kept(_ frame: DeepSkyReport.Frame) -> Bool { kept(frame, decision: decision(frame)) }

    fileprivate func kept(_ frame: DeepSkyReport.Frame, decision: String) -> Bool {
        guard isCurrent else { return frame.selected }
        guard frame.usable else { return false }
        switch decision {
        case "keep": return true
        case "reject": return false
        default: return appliesRecommendations == false || frame.recommendedKeep
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
        DeepSkyReviewSelection(settings: settings).kept(frame)
    }
    static func keptCount(_ frames: [DeepSkyReport.Frame], settings: DeepSkySettings) -> Int {
        let selection = DeepSkyReviewSelection(settings: settings)
        return frames.reduce(0) { count, frame in count + (selection.kept(frame) ? 1 : 0) }
    }

    static func attention(_ frame: DeepSkyReport.Frame) -> Bool {
        !frame.usable || !frame.recommendedKeep || !evidenceCodes(frame).isEmpty
    }
    /// Shared by frame review and result summaries so a cue can always be
    /// reached through the attention filter. Preserve report order and count
    /// each diagnostic once, including older reports with missing warnings.
    static func evidenceCodes(_ frame: DeepSkyReport.Frame) -> [String] {
        var codes = [frame.errorCode] + frame.reasons + frame.warnings
        if !frame.starsMeasured { codes.append("stellar-measurements-unavailable") }
        if !frame.temporalAvailable && !codes.contains("insufficient-temporal-peers") {
            codes.append("temporal-measurements-unavailable")
        }
        var seen: Set<String> = ["", "user-excluded"]
        return codes.filter { seen.insert($0).inserted }
    }
    static func manual(_ frame: DeepSkyReport.Frame, settings: DeepSkySettings) -> Bool {
        ["keep", "reject"].contains(decision(frame, settings: settings))
    }
    static func decision(_ frame: DeepSkyReport.Frame, settings: DeepSkySettings) -> String {
        guard !settings.overrides.isEmpty else { return "auto" }
        return settings.overrides[frame.input]
            ?? settings.overrides[PhotonStackProject.assetIdentityPath(for: URL(fileURLWithPath: frame.input))]
            ?? "auto"
    }
    static func reason(_ code: String, chinese: Bool) -> String {
        guard chinese else { return code.replacingOccurrences(of: "-", with: " ") }
        return ["low-coverage": "覆盖率不足", "high-noise": "噪声偏高", "broad-stars": "星点偏大",
                "elongated-stars": "星点拉长", "elongated-stars-absolute": "星点偏心率超过复查阈值",
                "few-stars": "可测星点偏少", "broad-temporal-residuals": "大范围帧间异常",
                "user-excluded": "手动排除", "RegistrationRejected": "对齐不可靠",
                "insufficient-temporal-peers": "有效比较帧不足", "stellar-measurements-unavailable": "无法测量星点",
                "temporal-measurements-unavailable": "帧间异常测量不可用",
                "stellar-shapes-unavailable": "原尺寸星形样本不足",
                "localized-transients-use-pixel-rejection": "局部异常，建议像素剔除",
                "signed-linear-transient-candidate": "瞬态线条候选"][code] ?? code
    }
    static func reasons(_ frame: DeepSkyReport.Frame, chinese: Bool) -> [String] {
        evidenceCodes(frame).map { reason($0, chinese: chinese) }
    }
    struct Listing {
        let frames: [DeepSkyReport.Frame]
        let counts: [DeepSkyReviewFilter: Int]
    }

    static func initialFilter(report: DeepSkyReport?, settings: DeepSkySettings, isCurrent: Bool) -> DeepSkyReviewFilter {
        guard let report, hasRecordedSelection(report) else { return .all }
        let selection = DeepSkyReviewSelection(report: report, settings: settings, isCurrent: isCurrent)
        return report.frames.contains { !selection.kept($0) } ? .excluded : .all
    }

    /// Counts describe the entire report; the query and the chosen filter only
    /// affect the list. Compute decisions and attention rank once per exposure.
    static func listing(_ frames: [DeepSkyReport.Frame], settings: DeepSkySettings,
                        filter: DeepSkyReviewFilter, query: String, sort: DeepSkyReviewSort) -> Listing {
        listing(frames, selection: DeepSkyReviewSelection(settings: settings), filter: filter, query: query, sort: sort)
    }

    static func listing(_ frames: [DeepSkyReport.Frame], selection: DeepSkyReviewSelection,
                        filter: DeepSkyReviewFilter, query: String, sort: DeepSkyReviewSort) -> Listing {
        var counts: [DeepSkyReviewFilter: Int] = [.all: frames.count]
        var visible: [(frame: DeepSkyReport.Frame, rank: Int)] = []
        let search = query.trimmingCharacters(in: .whitespacesAndNewlines)
        let prefixed = search.hasPrefix("#")
        let digits = prefixed ? search.dropFirst() : search[...]
        let number = digits.allSatisfy(\.isNumber) ? Int(digits) : nil
        let frameNumber = number.flatMap { number in
            prefixed || frames.contains(where: { $0.index + 1 == number }) ? number : nil
        }
        for frame in frames {
            let override = selection.decision(frame)
            let manual = override == "keep" || override == "reject"
            let excluded = !selection.kept(frame, decision: override)
            let rank = !frame.usable ? 3 : !frame.recommendedKeep ? 2 : attention(frame) ? 1 : 0
            if manual { counts[.manual, default: 0] += 1 }
            counts[excluded ? .excluded : .kept, default: 0] += 1
            if rank > 0 { counts[.attention, default: 0] += 1 }
            let matches = switch filter {
            case .all: true
            case .attention: rank > 0
            case .excluded: excluded
            case .kept: !excluded
            case .manual: manual
            }
            guard matches else { continue }
            if let frameNumber {
                guard frame.index + 1 == frameNumber else { continue }
            } else if !search.isEmpty {
                let codes = evidenceCodes(frame)
                var terms = [frame.input] + codes
                for chinese in [true, false] {
                    terms.append(DeepSkyReviewEvidence.decisionTitle(for: frame, selection: selection, chinese: chinese))
                    for code in codes {
                        terms.append(DeepSkyReviewEvidence.title(for: code, chinese: chinese))
                        terms.append(reason(code, chinese: chinese))
                    }
                }
                guard terms.contains(where: { $0.localizedCaseInsensitiveContains(search) }) else { continue }
            }
            visible.append((frame, rank))
        }
        visible.sort { lhs, rhs in
            let a = lhs.frame, b = rhs.frame
            switch sort {
            case .attention: if lhs.rank != rhs.rank { return lhs.rank > rhs.rank }
            case .fwhm: if a.fwhm != b.fwhm { return a.fwhm > b.fwhm }
            case .noise: if a.noiseRatio != b.noiseRatio { return a.noiseRatio > b.noiseRatio }
            case .coverage: if a.coverage != b.coverage { return a.coverage < b.coverage }
            case .capture: break
            }
            return a.index < b.index
        }
        return Listing(frames: visible.map(\.frame), counts: counts)
    }
    /// Recipe equality alone does not prove a failed run completed analysis.
    static func analysisIsCurrent(_ report: DeepSkyReport?, recipe: @autoclosure () -> String?) -> Bool {
        guard let report, report.success else { return false }
        return sameAnalysisRecipe(recipe(), report.recipe)
    }
    /// A failure before assessment leaves selected=false on every placeholder.
    /// Only display a decision summary when the engine reached frame selection.
    static func hasRecordedSelection(_ report: DeepSkyReport) -> Bool {
        guard !report.frames.isEmpty else { return false }
        return report.success || report.errorCode == "SelectionNeedsReview" ||
            report.frames.contains { $0.selected || $0.recommendedKeep || $0.temporalAvailable } ||
            report.frames.allSatisfy { !$0.reasons.isEmpty }
    }

    /// A selection safeguard still produced frame assessments. Users can
    /// revise those decisions before retrying the matching set of inputs.
    static func canRecoverSelection(_ report: DeepSkyReport?, recipe: @autoclosure () -> String?) -> Bool {
        guard let report, !report.success, report.errorCode == "SelectionNeedsReview",
              report.frames.contains(where: \.usable) else { return false }
        return sameAnalysisRecipe(recipe(), report.recipe)
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
                    || line == "develop.sky-percentile \"0.5\""
                    || line == "develop.lock-denoise-stretch \"off\""
                    || line == "develop.color-transfer \"legacy-code\""
                    || line == "background.polynomial-degree \"2\""
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
