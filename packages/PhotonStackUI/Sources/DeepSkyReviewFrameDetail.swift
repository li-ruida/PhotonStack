import PhotonStackAppCore
import SwiftUI

struct DeepSkyReviewFrameDetail: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let frame: DeepSkyReport.Frame
    let reference: DeepSkyReport.Frame?
    let evidence: DeepSkyReviewEvidence.FrameEvidence
    let previewHeight: CGFloat
    let hasPrevious: Bool
    let hasNext: Bool
    let navigate: (Int) -> Void
    private var chinese: Bool { model.language == .simplifiedChinese }

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 10) {
                    HStack(alignment: .top) {
                        Text("#\(frame.index + 1)  " + URL(fileURLWithPath: frame.input).lastPathComponent)
                            .font(.headline).fixedSize(horizontal: false, vertical: true)
                            .textSelection(.enabled).help(frame.input)
                            .accessibilityIdentifier("review.detail.filename")
                        Spacer(minLength: 4)
                        Button { navigate(-1) } label: { Image(systemName: "chevron.up") }
                            .keyboardShortcut(.upArrow, modifiers: .option).disabled(!hasPrevious)
                            .accessibilityLabel(chinese ? "上一帧" : "Previous frame").help("⌥↑")
                        Button { navigate(1) } label: { Image(systemName: "chevron.down") }
                            .keyboardShortcut(.downArrow, modifiers: .option).disabled(!hasNext)
                            .accessibilityLabel(chinese ? "下一帧" : "Next frame").help("⌥↓")
                    }.id("review.detail.top")
                    decision
                    if !evidence.reasons.isEmpty {
                        evidenceGroup(exclusionHeading, items: evidence.reasons)
                            .accessibilityIdentifier("review.detail.reasons")
                    }
                    if !evidence.warnings.isEmpty {
                        DisclosureGroup {
                            evidenceItems(evidence.warnings)
                        } label: {
                            Text((chinese ? "观察提醒 · " : "Observations · ") + evidence.warnings.map(\.title).joined(separator: chinese ? "、" : ", "))
                                .font(.callout).fixedSize(horizontal: false, vertical: true)
                        }.accessibilityIdentifier("review.detail.observations")
                    }
                    Divider()
                    DeepSkyFramePreview(model: model, frame: frame, reference: reference)
                        .frame(height: previewHeight)
                    measurements
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.trailing, 5)
            }
            .onChange(of: frame.input) { _, _ in proxy.scrollTo("review.detail.top", anchor: .top) }
        }
    }

    private var exclusionHeading: String {
        if !frame.usable { return chinese ? "不可用依据" : "Why this frame is unusable" }
        if evidence.kept || evidence.decision == .manuallyExcluded || evidence.decision == .recordedExcluded {
            return chinese ? "引擎建议排除的依据" : "Why the engine recommended exclusion"
        }
        return chinese ? "整帧排除依据" : "Whole-frame exclusion evidence"
    }

    private var decision: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Label(evidence.decisionTitle, systemImage: !frame.usable ? "exclamationmark.octagon" :
                        evidence.kept ? "checkmark.circle.fill" : "minus.circle.fill")
                    .font(.callout.weight(.semibold)).foregroundStyle(evidence.kept ? Color.green : .orange)
                if frame.reference {
                    Label(chinese ? "参考帧" : "Reference frame", systemImage: "star").font(.caption).foregroundStyle(.secondary)
                }
            }
            Text(evidence.decisionDetail).font(.callout).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
        }.accessibilityIdentifier("review.detail.decision")
    }

    private func evidenceGroup(_ title: String, items: [DeepSkyReviewEvidence.Item]) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.subheadline.weight(.semibold))
            evidenceItems(items)
        }
    }

    private func evidenceItems(_ items: [DeepSkyReviewEvidence.Item]) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            ForEach(items) { item in
                VStack(alignment: .leading, spacing: 1) {
                    Text(item.title).font(.callout.weight(.medium))
                    if !item.detail.isEmpty {
                        Text(item.detail).font(.caption).foregroundStyle(.secondary)
                    }
                }.fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
            }
        }.frame(maxWidth: .infinity, alignment: .leading)
    }

    private var measurements: some View {
        DisclosureGroup(chinese ? "逐帧测量与像素剔除" : "Frame measurements and pixel rejection") {
            VStack(alignment: .leading, spacing: 8) {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 100), alignment: .leading)], alignment: .leading, spacing: 7) {
                    metric(chinese ? "星点宽度" : "Star width", frame.starsMeasured && frame.fwhm > 0 ? String(format: "%.2f px", frame.fwhm) : "—")
                    metric(chinese ? "偏心率" : "Eccentricity", frame.starsMeasured && frame.fwhm > 0 ? String(format: "%.3f", frame.eccentricity) : "—")
                    metric(chinese ? "噪声倍率" : "Noise ratio", frame.usable ? String(format: "%.2f×", frame.noiseRatio) : "—")
                    metric(chinese ? "有效覆盖" : "Coverage", frame.usable ? String(format: "%.1f%%", frame.coverage * 100) : "—")
                    metric(chinese ? "星点检测" : "Star detections", DeepSkyReview.starCountText(frame)).help(DeepSkyReview.starCountHelp(frame, chinese: chinese))
                    metric(chinese ? "匹配星对" : "Matched pairs", frame.reference ? "—" : "\(frame.matches)")
                }
                if frame.fwhmEstimator == "native-gaussian-v1" {
                    Text(chinese ? "原尺寸高斯拟合 · \(frame.starShapeCount ?? 0) 颗有效星形样本" :
                            "Native Gaussian fits · \(frame.starShapeCount ?? 0) star-shape samples")
                } else if frame.starsMeasured {
                    Text(chinese ? "历史星宽估计；重新分析可获取原尺寸测量。" : "Historical width estimate. Analyze again for a native measurement.")
                }
                if frame.temporalAvailable {
                    Text(String(format: chinese ? "正残差 %.3f%% · 负残差 %.3f%%" : "Positive residual %.3f%% · negative %.3f%%",
                                frame.positiveFraction * 100, frame.negativeFraction * 100))
                }
                if frame.stackComparedSamples > 0 {
                    Text(chinese ? "高侧 / 低侧像素样本剔除 · \(frame.rejectedHighSamples) / \(frame.rejectedLowSamples)" :
                            "Rejected high / low pixel samples · \(frame.rejectedHighSamples) / \(frame.rejectedLowSamples)")
                }
            }.font(.caption).textSelection(.enabled).padding(.vertical, 4)
        }.font(.callout)
    }

    private func metric(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.callout.monospacedDigit())
        }
    }
}
