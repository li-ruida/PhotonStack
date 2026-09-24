import PhotonStackAppCore
import SwiftUI

struct DeepSkyProcessingPresentation {
    let job: ProcessingJob?
    let message: String?
    let fraction: Double?
    let chinese: Bool

    static func combinationDetail(step: Int?, total: Int?, overall: Double, chinese: Bool) -> String? {
        guard let step, let total, step >= 0, total > 0, step <= total, overall.isFinite else { return nil }
        let stagePercent = Int((Double(step) / Double(total) * 100).rounded())
        let overallPercent = Int((min(max(overall, 0), 1) * 100).rounded())
        return chinese
            ? "像素合成与剔除 · 本阶段 \(stagePercent)%（总流程 \(overallPercent)%）"
            : "Combining and rejecting pixels · stage \(stagePercent)% (overall \(overallPercent)%)"
    }

    var title: String {
        switch job?.title {
        case "Deep Sky Frame Assessment": chinese ? "正在分析亮场" : "Analyzing light frames"
        case "Deep Sky Recipe": chinese ? "正在分析与堆栈" : "Analyzing and stacking"
        case "Deep Sky Background & Denoise": chinese ? "正在修整成片" : "Reprocessing the master"
        default: chinese ? "正在处理图像" : "Processing images"
        }
    }

    var detail: String {
        if fraction.map({ $0.isFinite && $0 >= 1 }) == true {
            return chinese ? "正在保存并加载结果…" : "Saving and loading the result…"
        }
        guard let message, !message.isEmpty, message != job?.title else {
            return chinese ? "正在准备…" : "Preparing…"
        }
        return message
    }

    func elapsed(at now: Date) -> String? {
        guard let start = job?.createdAt else { return nil }
        let duration = now.timeIntervalSince(start)
        guard duration.isFinite else { return nil }
        let seconds = Int(min(max(0, duration), Double(Int.max / 2)))
        let hours = seconds / 3600, minutes = (seconds % 3600) / 60, rest = seconds % 60
        let value = hours > 0 ? String(format: "%d:%02d:%02d", hours, minutes, rest)
            : String(format: "%d:%02d", minutes, rest)
        return chinese ? "已用时 \(value)" : "Elapsed \(value)"
    }
}

struct DeepSkyProcessingView: View {
    let presentation: DeepSkyProcessingPresentation
    var compact = false

    var body: some View {
        VStack(alignment: compact ? .leading : .center, spacing: compact ? 6 : 16) {
            if !compact {
                Image(systemName: "photo.stack").font(.system(size: 42)).foregroundStyle(.secondary)
                Text(presentation.title).font(.title3.weight(.semibold))
                    .accessibilityIdentifier("deep-sky.processing.title")
            }
            Text(presentation.detail)
                .font(compact ? .caption : .body).monospacedDigit()
                .accessibilityIdentifier("deep-sky.processing.detail")
            if let fraction = presentation.fraction, fraction.isFinite {
                ProgressView(value: min(max(fraction, 0), 1))
            } else {
                ProgressView().controlSize(.small)
            }
            TimelineView(.periodic(from: .now, by: 1)) { context in
                if let elapsed = presentation.elapsed(at: context.date) {
                    Text(elapsed).font(.caption).monospacedDigit().foregroundStyle(.secondary)
                        .accessibilityIdentifier("deep-sky.processing.elapsed")
                }
            }
            if !compact {
                Text(presentation.chinese ? "可以关闭此面板，处理会继续。" : "You can close this panel while processing continues.")
                    .font(.callout).foregroundStyle(.secondary)
            }
        }
        .frame(maxWidth: compact ? .infinity : 520)
    }
}
