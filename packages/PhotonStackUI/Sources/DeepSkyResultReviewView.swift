import PhotonStackAppCore
import SwiftUI

/// Review cues, deliberately not an automatic pass/fail or aesthetic score.
enum DeepSkyResultReview {
    static func cues(_ report: DeepSkyReport) -> [String] {
        guard report.success, !report.analysisOnly else { return [] }
        var result: [String] = []
        if report.masterReused == true || report.inputCount == 0 { result.append("reused-master") }
        if report.selectedCount > 0 && report.selectedCount < 8 { result.append("few-frames") }
        if report.frames.contains(where: { $0.selected && !$0.recommendedKeep }) { result.append("override") }
        if let display = report.displayReview {
            if display.nearBlackFraction >= 0.01 { result.append("near-black") }
            if display.nearWhiteFraction >= 0.001 { result.append("near-white") }
        } else { result.append("unmeasured") }
        return result
    }

    static func text(_ cue: String, chinese: Bool) -> String {
        switch cue {
        case "reused-master": chinese
            ? "本次仅重新处理已有母版，没有重新筛片、配准或堆栈；母版里的缺陷可能仍在。"
            : "This run only reprocessed an existing master; source review, alignment and stacking were not repeated. Existing defects may remain."
        case "few-frames": chinese
            ? "入栈少于 8 张：请重点检查噪声和残留轨迹。帧数不是总曝光，也不能单独决定画质。"
            : "Fewer than 8 frames stacked: inspect noise and residual trails. Frame count alone does not establish exposure or quality."
        case "override": chinese
            ? "成图包含未被自动推荐保留的帧。请回到筛片页复核这些选择。"
            : "The result includes frames not recommended by automatic assessment. Revisit those selections in frame review."
        case "near-black": chinese
            ? "近黑像素较多：复查暗部是否被压死，可调整背景亮度、天空参考或拉伸。真实暗背景也可能触发此提示。"
            : "Many near-black pixels: check crushed shadows and review background level, sky reference or stretching. Genuine dark sky can also trigger this cue."
        case "near-white": chinese
            ? "近白像素较多：复查星核和亮部是否丢失层次，可降低拉伸或调整白点。这不等于原片已经过曝。"
            : "Many near-white pixels: inspect star cores and highlights; review stretching or the white point. This does not prove source overexposure."
        default: chinese
            ? "旧报告没有显示端指标；没有数据不表示没有问题。更新后期可生成这些指标。"
            : "This older report has no display metrics. Missing data does not mean no issues; reprocess the master to measure them."
        }
    }
}

struct DeepSkyResultReviewView: View {
    let report: DeepSkyReport
    let chinese: Bool
    var viewImage: () -> Void
    var editFinishing: () -> Void
    var reviewFrames: (() -> Void)? = nil
    @State var expanded = false

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(chinese ? "成图已生成 · 下一步检查画质" : "Image ready · inspect the result")
                .font(.callout.weight(.semibold))
            Text(chinese ? "先看全图，再用 100% 检查星点、彩边、斑驳和暗部；以下指标不检测全部缺陷。"
                 : "Inspect the full image, then stars, color fringes, mottling and shadows at 100%. Metrics cannot detect every defect.")
                .font(.caption).foregroundStyle(.secondary)
            if !report.frames.isEmpty {
                Text(DeepSkyResultAssessment(report).summary(chinese: chinese))
                    .font(.caption).fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("deep-sky.quality.source-summary")
            }
            DisclosureGroup(chinese ? "查看复查提示与显示指标" : "Review cues and display metrics", isExpanded: $expanded) {
                ScrollView {
                  VStack(alignment: .leading, spacing: 6) {
                    if let locked = report.denoiseStretchLocked {
                        Text(chinese
                             ? "色调参考：\(locked ? "背景校正后、降噪前" : "降噪后的处理图像")"
                             : "Tone reference: \(locked ? "background-corrected, before denoising" : "processed image after denoising")")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    ForEach(DeepSkyResultReview.cues(report), id: \.self) { cue in
                        Text(DeepSkyResultReview.text(cue, chinese: chinese))
                            .font(.caption).fixedSize(horizontal: false, vertical: true)
                    }
                    sourceIssues
                    if let metrics = report.displayReview {
                        Text(chinese
                             ? "近黑 \(percent(metrics.nearBlackFraction)) · 近白 \(percent(metrics.nearWhiteFraction))"
                             : "Near black \(percent(metrics.nearBlackFraction)) · Near white \(percent(metrics.nearWhiteFraction))")
                            .font(.caption.monospacedDigit())
                        Text(chinese ? "按导出前 RGB 统计：三个通道均 ≤ 1/255 为近黑，均 ≥ 0.995 为近白；无覆盖像素不计入。不是线性信号质量或原片过曝率。"
                             : "Pre-export RGB: all channels ≤ 1/255 are near black; all ≥ 0.995 are near white. Uncovered pixels excluded. Not linear signal quality or source clipping.")
                            .font(.caption2).foregroundStyle(.secondary)
                    }
                  }.frame(maxWidth: .infinity, alignment: .leading).padding(.top, 4)
                }.frame(maxHeight: 180)
            }.font(.caption)
            HStack {
                Button(chinese ? "打开成图复查" : "Inspect rendered image", action: viewImage)
                    .buttonStyle(.borderedProminent)
                    .accessibilityIdentifier("deep-sky.quality.inspect")
                Button(chinese ? "调整后期参数" : "Adjust finishing", action: editFinishing)
                    .accessibilityIdentifier("deep-sky.quality.finish")
                if !report.frames.isEmpty, let reviewFrames {
                    Button(chinese ? "复查源帧" : "Review source frames", action: reviewFrames)
                        .accessibilityIdentifier("deep-sky.quality.frames")
                }
            }
        }
        .accessibilityIdentifier("deep-sky.quality-review")
    }

    @ViewBuilder private var sourceIssues: some View {
        let assessment = DeepSkyResultAssessment(report)
        if !report.frames.isEmpty {
            if assessment.registrationFailed + assessment.otherFailed > 0 {
                Text(chinese
                     ? "已排除的处理失败帧：配准失败 \(assessment.registrationFailed) 张，其他失败 \(assessment.otherFailed) 张；它们未参与成图。"
                     : "Excluded failures: \(assessment.registrationFailed) alignment, \(assessment.otherFailed) other. These frames did not contribute to the image.")
                    .font(.caption).fixedSize(horizontal: false, vertical: true)
            }
            ForEach(assessment.issues) { issue in
                Text("\(DeepSkyReview.reason(issue.code, chinese: chinese)) · \(issue.frameIndices.count)\(chinese ? " 张入栈帧" : " stacked frames"). \(DeepSkyResultAssessment.advice(issue.code, chinese: chinese))")
                    .font(.caption).fixedSize(horizontal: false, vertical: true)
            }
            Text(chinese ? "以上来自源帧记录，不是成图缺陷检测；多个提示可能涉及同一帧。张数不等于总曝光，所有帧都通过相对筛选也不保证清晰。"
                 : "These are source-frame records, not final-image defect detection. Cues may overlap. Counts are not exposure; passing relative screening does not guarantee sharpness.")
                .font(.caption2).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func percent(_ value: Double) -> String {
        value.formatted(.percent.precision(.fractionLength(2)))
    }
}
