import PhotonStackAppCore

/// Summarizes the immutable run report, never the user's next-run selections.
/// These are acquisition/assessment cues, not detected defects in the final image.
struct DeepSkyResultAssessment {
    struct Issue: Identifiable, Equatable {
        let code: String
        let frameIndices: [Int]
        var id: String { code }
    }

    let stacked: Int
    let input: Int
    let excluded: Int
    let registrationFailed: Int
    let otherFailed: Int
    let attention: Int
    let issues: [Issue]

    init(_ report: DeepSkyReport) {
        let selected = report.frames.filter(\.selected)
        stacked = selected.count
        input = report.frames.count
        excluded = report.frames.filter { !$0.selected }.count
        registrationFailed = report.frames.filter { !$0.usable && $0.errorCode == "RegistrationRejected" }.count
        otherFailed = report.frames.filter { !$0.usable && $0.errorCode != "RegistrationRejected" }.count
        var grouped: [String: [Int]] = [:]
        attention = selected.filter(DeepSkyReview.attention).count
        for frame in selected {
            for code in DeepSkyReview.evidenceCodes(frame) { grouped[code, default: []].append(frame.index) }
        }
        issues = grouped.map { Issue(code: $0.key, frameIndices: $0.value.sorted()) }
            .sorted { $0.frameIndices.count == $1.frameIndices.count
                ? $0.code < $1.code : $0.frameIndices.count > $1.frameIndices.count }
    }

    func summary(chinese: Bool) -> String {
        chinese ? "源帧记录：入栈 \(stacked)/\(input) 张 · 已排除 \(excluded) 张 · 入栈中需复查 \(attention) 张"
            : "Source record: \(stacked)/\(input) stacked · \(excluded) excluded · \(attention) stacked frames need review"
    }

    static func advice(_ code: String, chinese: Bool) -> String {
        switch code {
        case "localized-transients-use-pixel-rejection", "signed-linear-transient-candidate":
            chinese ? "检查对应位置的成图与像素剔除结果；候选不等于残留飞机线，不必直接删除整帧。"
                : "Inspect those locations and pixel rejection results. Candidates do not prove residual trails or require discarding whole frames."
        case "stellar-measurements-unavailable", "stellar-shapes-unavailable", "insufficient-temporal-peers", "temporal-measurements-unavailable":
            chinese ? "这项质量检查缺少足够测量，不能视为通过；请查看原片并核对分析设置。"
                : "This check lacks sufficient measurements, not a pass. Inspect the source frames and analysis settings."
        case "elongated-stars-absolute":
            chinese ? "本帧虽未显著差于同组素材，星点仍可能拉长；请在原图 1:1 检查跟踪、对焦和光学像差。"
                : "This frame is not a relative outlier, but its stars may still be elongated. Inspect tracking, focus and optical aberrations at 1:1."
        case "broad-stars", "elongated-stars", "few-stars":
            chinese ? "先按星宽或异常排序复查源帧；只重新调色不能修复失焦或配准问题。"
                : "Review source frames by star width or attention first. Recoloring alone cannot fix focus or alignment problems."
        case "high-noise", "low-coverage", "broad-temporal-residuals":
            chinese ? "复查这些帧的噪声、覆盖与帧间差异，再决定是否修改保留选择并重新堆栈。"
                : "Inspect noise, coverage and temporal differences before changing selection and restacking."
        default:
            chinese ? "查看筛片工作台中的逐帧报告；此提示不自动改变保留选择。"
                : "Inspect the per-frame report in frame review; this cue does not change selections."
        }
    }
}
