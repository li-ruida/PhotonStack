import Foundation
import PhotonStackAppCore

/// Statistics describe the immutable analysis. The selection resolves whether
/// this screen shows current project decisions or the recorded result.
struct DeepSkyReviewEvidence {
    enum Decision: Equatable {
        case automaticallyKept, automaticallyExcluded, manuallyKept, manuallyExcluded
        case selectionDisabled, unavailable, recordedKept, recordedExcluded
    }

    struct Item: Identifiable, Equatable {
        let code: String
        let title: String
        let detail: String
        var id: String { code }
    }

    struct FrameEvidence {
        let decision: Decision
        let kept: Bool
        let decisionTitle: String
        let decisionDetail: String
        let summary: String
        let reasons: [Item]
        let warnings: [Item]
    }

    private let limits: [String: Double]
    private let eccentricityMedian: Double?
    private let widthMedian: Double?
    private let countMedian: Double?
    private let residualLimit: Double?
    private let starMeasurementDisabled: Bool

    init(report: DeepSkyReport) {
        // Only the recorded recipe can supply a numeric threshold. Historical
        // recipes missing a value must not inherit today's project defaults.
        let keys: Set<String> = ["selection.maximum-eccentricity", "selection.maximum-noise-ratio",
            "selection.maximum-fwhm-ratio", "selection.minimum-star-ratio", "selection.minimum-coverage",
            "selection.maximum-residual-fraction", "registration.minimum-matches"]
        var recorded: [String: Double] = [:]
        var starsOff = false
        if report.recipe.hasPrefix("PHOTONSTACK_DEEP_SKY_RECIPE 1\n") {
            for line in report.recipe.split(separator: "\n") {
                if line == "selection.measure-stars \"off\"" { starsOff = true }
                guard let space = line.firstIndex(of: " ") else { continue }
                let key = String(line[..<space])
                guard keys.contains(key) else { continue }
                let raw = line[line.index(after: space)...]
                guard raw.count >= 2, raw.first == "\"", raw.last == "\"",
                      let number = Double(raw.dropFirst().dropLast()), number.isFinite else { continue }
                recorded[key] = number
            }
        }
        limits = recorded
        starMeasurementDisabled = starsOff
        // Use the same peers as SequenceQualityAnalyzer::assess. Rejected but
        // usable frames participate. A zero width is an unavailable shape fit.
        let usable = report.frames.filter(\.usable)
        let counted = usable.filter { $0.starsMeasured && $0.starCount > 0 }
        let shapes = counted.filter { $0.fwhm > 0 }
        eccentricityMedian = Self.median(shapes.map(\.eccentricity))
        widthMedian = Self.median(shapes.map(\.fwhm))
        countMedian = Self.median(counted.map { Double($0.starCount) })
        let residuals = usable.filter(\.temporalAvailable).map { $0.positiveFraction + $0.negativeFraction }
        if let floor = recorded["selection.maximum-residual-fraction"],
           let center = Self.median(residuals), let mad = Self.median(residuals.map { abs($0 - center) }) {
            residualLimit = max(floor, center + 4 * 1.482602218505602 * mad)
        } else { residualLimit = nil }
    }

    private static func median(_ samples: [Double]) -> Double? {
        guard !samples.isEmpty, samples.allSatisfy(\.isFinite) else { return nil }
        let ordered = samples.sorted()
        let mid = ordered.count / 2
        return ordered.count.isMultiple(of: 2) ? (ordered[mid - 1] + ordered[mid]) / 2 : ordered[mid]
    }

    private static func distinct(_ codes: [String]) -> [String] {
        var seen: Set<String> = ["", "user-excluded"]
        return codes.filter { seen.insert($0).inserted }
    }

    private static func engineReasons(_ frame: DeepSkyReport.Frame) -> [String] {
        distinct([frame.errorCode] + frame.reasons)
    }

    /// Actual reasons for exclusion, suitable for whole-report aggregation.
    /// When the origin is unknown, a recommendation cannot establish who made
    /// the recorded decision. It remains available in the frame detail.
    static func exclusionCodes(_ frame: DeepSkyReport.Frame, selection: DeepSkyReviewSelection) -> [String] {
        guard frame.usable else {
            let reasons = engineReasons(frame)
            return reasons.isEmpty ? ["invalid-frame"] : reasons
        }
        guard !selection.kept(frame) else { return [] }
        switch selection.decision(frame) {
        case "reject": return ["user-excluded"]
        case "auto" where !frame.recommendedKeep:
            let reasons = engineReasons(frame)
            return reasons.isEmpty ? ["reason-unavailable"] : reasons
        default: return ["reported-exclusion"]
        }
    }

    static func title(for code: String, chinese: Bool) -> String {
        switch code {
        case "reported-exclusion": chinese ? "报告记录排除" : "Recorded exclusion"
        case "reason-unavailable": chinese ? "报告未记录具体原因" : "Reason unavailable in report"
        case "invalid-frame": chinese ? "帧不可用" : "Frame unavailable"
        case "RegistrationRejected": chinese ? "配准未通过" : "Registration rejected"
        case "user-excluded": chinese ? "手动排除" : "Manually excluded"
        case "localized-transients-use-pixel-rejection": chinese ? "局部像素异常" : "Localized pixel outliers"
        case "signed-linear-transient-candidate": chinese ? "检测到疑似线状瞬态" : "Possible transient line"
        default: DeepSkyReview.reason(code, chinese: chinese)
        }
    }

    private static func decision(_ frame: DeepSkyReport.Frame, selection: DeepSkyReviewSelection) -> Decision {
        guard frame.usable else { return .unavailable }
        let kept = selection.kept(frame)
        switch selection.decision(frame) {
        case "keep" where kept: return .manuallyKept
        case "reject" where !kept: return .manuallyExcluded
        case "auto" where kept && selection.appliesRecommendations == false: return .selectionDisabled
        case "auto" where kept == frame.recommendedKeep && selection.appliesRecommendations != false:
            return kept ? .automaticallyKept : .automaticallyExcluded
        default: return kept ? .recordedKept : .recordedExcluded
        }
    }

    static func decisionTitle(for frame: DeepSkyReport.Frame, selection: DeepSkyReviewSelection, chinese: Bool) -> String {
        decisionTitle(decision(frame, selection: selection), recommendedKeep: frame.recommendedKeep, chinese: chinese)
    }

    private static func decisionTitle(_ decision: Decision, recommendedKeep: Bool, chinese: Bool) -> String {
        switch decision {
        case .unavailable: chinese ? "不可用" : "Unavailable"
        case .manuallyExcluded: chinese ? "手动排除" : "Manually excluded"
        case .manuallyKept: chinese ? (recommendedKeep ? "手动保留" : "强制保留") : (recommendedKeep ? "Manually kept" : "Forced keep")
        case .selectionDisabled: chinese ? "保留（未启用自动排除）" : "Kept with selection off"
        case .automaticallyExcluded: chinese ? "自动排除" : "Automatically excluded"
        case .automaticallyKept: chinese ? "自动保留" : "Automatically kept"
        case .recordedKept: chinese ? "报告保留" : "Recorded keep"
        case .recordedExcluded: chinese ? "报告排除" : "Recorded exclusion"
        }
    }

    func frame(_ frame: DeepSkyReport.Frame, selection: DeepSkyReviewSelection, chinese: Bool) -> FrameEvidence {
        let kept = selection.kept(frame)
        let decision = Self.decision(frame, selection: selection)
        let title = Self.decisionTitle(decision, recommendedKeep: frame.recommendedKeep, chinese: chinese)

        let reported = Self.engineReasons(frame)
        var reasonCodes = !frame.usable || !frame.recommendedKeep ? reported : []
        if reasonCodes.isEmpty && !frame.usable { reasonCodes = ["invalid-frame"] }
        if reasonCodes.isEmpty && !frame.recommendedKeep { reasonCodes = ["reason-unavailable"] }
        let reasons = reasonCodes.map { item($0, frame: frame, chinese: chinese) }
        // Older reports can contain advisory codes in reasons even though their
        // recorded recommendation was keep. Preserve those as observations.
        var warningCodes = (frame.usable && frame.recommendedKeep ? reported : []) + frame.warnings
        if frame.usable {
            if !frame.starsMeasured && !starMeasurementDisabled { warningCodes.append("stellar-measurements-unavailable") }
            if !frame.temporalAvailable && !warningCodes.contains("insufficient-temporal-peers") {
                warningCodes.append("temporal-measurements-unavailable")
            }
        }
        let warnings = Self.distinct(warningCodes).filter { !reasonCodes.contains($0) }
            .map { item($0, frame: frame, chinese: chinese) }

        let origin = selection.isCurrent
            ? (chinese ? "当前选择" : "Current decision")
            : (chinese ? "上次报告" : "Recorded decision")
        let detail: String, summary: String
        let recommendation = frame.recommendedKeep
            ? (chinese ? "引擎建议保留。" : "The engine recommended keeping this frame.")
            : (chinese ? "引擎建议排除。" : "The engine recommended excluding this frame.")
        switch decision {
        case .unavailable:
            detail = chinese ? "本帧未能完成有效分析，无法入栈。" : "This frame has no usable analysis and cannot enter the stack."
            summary = reasons.first?.title ?? title
        case .manuallyExcluded:
            detail = chinese ? "\(origin)由手动标记排除。\(recommendation)" : "\(origin) follows a manual exclusion. \(recommendation)"
            summary = recommendation
        case .manuallyKept:
            detail = chinese ? "\(origin)由手动标记保留。\(recommendation)" : "\(origin) follows a manual keep. \(recommendation)"
            summary = recommendation
        case .selectionDisabled:
            detail = chinese ? "自动排除已关闭，可用帧按保留处理。\(recommendation)" : "Automatic exclusion is disabled. Usable frames are kept. \(recommendation)"
            summary = recommendation
        case .automaticallyExcluded:
            detail = chinese ? "\(origin)采用引擎的整帧排除建议。" : "\(origin) applies the engine’s recommendation to exclude the frame."
            summary = reasons.first.map(compactSummary) ?? detail
        case .automaticallyKept:
            detail = chinese ? "本帧未触发整帧排除判据。" : "This frame triggered no whole-frame exclusion criterion."
            summary = warnings.first.map(compactSummary) ?? detail
        case .recordedKept, .recordedExcluded:
            detail = chinese ? "报告记录了该结果，决定来源未注明。\(recommendation)" : "The report records the result without its decision source. \(recommendation)"
            summary = detail
        }
        return FrameEvidence(decision: decision, kept: kept, decisionTitle: title, decisionDetail: detail,
                             summary: summary, reasons: reasons, warnings: warnings)
    }

    private func compactSummary(_ item: Item) -> String {
        guard let metric = item.detail.components(separatedBy: " · ").first, !metric.isEmpty else { return item.title }
        return item.title + " · " + metric
    }

    private func item(_ code: String, frame f: DeepSkyReport.Frame, chinese: Bool) -> Item {
        let title = Self.title(for: code, chinese: chinese)
        func number(_ value: Double, _ places: Int = 3) -> String {
            String(format: "%.*f", locale: Locale(identifier: "en_US_POSIX"), places, value)
        }
        let percent: (Double) -> String = { number($0 * 100, 2) + "%" }
        var parts: [String] = []
        switch code {
        case "elongated-stars", "elongated-stars-absolute":
            let reviewLimit = limits["selection.maximum-eccentricity"]
            let relativeLimit = eccentricityMedian.map { $0 + 0.15 }
            // A real M31 exposure and its exclusion threshold both round to
            // 0.860. Retain sufficient precision to show the measured side.
            var places = 3
            let bounds = [reviewLimit, relativeLimit].compactMap { $0 }
            while places < 9 && bounds.contains(where: {
                $0 != f.eccentricity && number($0, places) == number(f.eccentricity, places)
            }) { places += 1 }
            if f.starsMeasured && f.fwhm > 0 {
                parts.append(chinese ? "本帧偏心率 \(number(f.eccentricity, places))" : "Frame eccentricity \(number(f.eccentricity, places))")
            }
            if let group = eccentricityMedian {
                parts.append(chinese ? "同组中位 \(number(group, places))" : "Group median \(number(group, places))")
            }
            if let limit = reviewLimit {
                parts.append(chinese ? "配方复查线 \(number(limit, places))" : "Recipe review threshold \(number(limit, places))")
                if let relativeLimit {
                    let bound = number(relativeLimit, places)
                    parts.append(chinese ? "相对排除线约 \(bound)（中位 + 0.15）" : "Relative exclusion threshold ≈ \(bound) (median + 0.15)")
                }
            }
            parts.append(chinese ? "偏心率越接近 1，星点越细长" : "Stars get more elongated as eccentricity approaches 1")
        case "broad-stars":
            if f.fwhm > 0 { parts.append(chinese ? "本帧 FWHM \(number(f.fwhm, 2)) px" : "Frame FWHM \(number(f.fwhm, 2)) px") }
            if let group = widthMedian {
                parts.append(chinese ? "同组中位 \(number(group, 2)) px" : "Group median \(number(group, 2)) px")
                if let limit = limits["selection.maximum-fwhm-ratio"] {
                    parts.append(chinese ? "排除线约 \(number(group * limit, 2)) px（\(number(limit, 2)) 倍中位）"
                                         : "Exclusion threshold ≈ \(number(group * limit, 2)) px (\(number(limit, 2))× median)")
                }
            }
        case "few-stars":
            if f.starsMeasured {
                let count = DeepSkyReview.starCountText(f)
                parts.append(chinese ? "本帧测到 \(count) 个星点" : "\(count) detections in this frame")
            }
            if let group = countMedian {
                parts.append(chinese ? "报告中的同组中位 \(number(group, 1))" : "Median reported count \(number(group, 1))")
                if let limit = limits["selection.minimum-star-ratio"] {
                    parts.append(chinese ? "最低比例 \(percent(limit))" : "Minimum fraction \(percent(limit))")
                }
            }
        case "high-noise":
            parts.append(chinese ? "报告噪声比 \(number(f.noiseRatio, 2)) 倍" : "Reported noise ratio \(number(f.noiseRatio, 2))×")
            if let limit = limits["selection.maximum-noise-ratio"] {
                parts.append(chinese ? "配方上限 \(number(limit, 2)) 倍" : "Recipe maximum \(number(limit, 2))×")
            }
        case "low-coverage":
            parts.append(chinese ? "网格有效覆盖 \(percent(f.coverage))" : "Valid grid coverage \(percent(f.coverage))")
            if let limit = limits["selection.minimum-coverage"] {
                parts.append(chinese ? "配方最低 \(percent(limit))" : "Recipe minimum \(percent(limit))")
            }
        case "broad-temporal-residuals", "localized-transients-use-pixel-rejection":
            if f.temporalAvailable {
                parts.append(chinese ? "比较网格正残差 \(percent(f.positiveFraction))，负残差 \(percent(f.negativeFraction))"
                                     : "Positive outliers \(percent(f.positiveFraction)), negative \(percent(f.negativeFraction)) of compared grid samples")
                if code == "broad-temporal-residuals", let limit = residualLimit {
                    parts.append(chinese ? "合计 \(percent(f.positiveFraction + f.negativeFraction))，本组排除线约 \(percent(limit))"
                                         : "Combined \(percent(f.positiveFraction + f.negativeFraction)), group exclusion threshold ≈ \(percent(limit))")
                }
            }
        case "RegistrationRejected":
            if f.matches > 0 { parts.append(chinese ? "报告匹配 \(f.matches) 个星点" : "\(f.matches) reported matches") }
            if let limit = limits["registration.minimum-matches"] {
                parts.append(chinese ? "配方至少要求 \(number(limit, 0)) 个匹配" : "Recipe requires at least \(number(limit, 0)) matches")
            }
        case "signed-linear-transient-candidate":
            if let trails = f.trails, !trails.isEmpty {
                parts.append(chinese ? "比较网格发现 \(trails.count) 条线状候选" : "\(trails.count) line candidates in the comparison grid")
            }
        case "stellar-shapes-unavailable":
            if let count = f.starShapeCount {
                parts.append(chinese ? "有效原尺寸星形拟合 \(count) 个，无法报告星宽与偏心率" : "\(count) accepted native fits, width and eccentricity unavailable")
            }
        default: break
        }
        if code == f.errorCode && !f.message.isEmpty { parts.append(f.message) }
        return Item(code: code, title: title, detail: parts.joined(separator: " · "))
    }
}
