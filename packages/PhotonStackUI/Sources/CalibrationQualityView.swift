import AppKit
import SwiftUI

struct CalibrationQualityView: View {
    let library: CalibrationLibrary
    let entry: CalibrationLibrary.Entry
    let chinese: Bool
    @Environment(\.dismiss) private var dismiss
    @State private var sigma = "8"
    @State private var tolerance = "10"
    @State private var minimum = "0.5"
    @State private var maximum = "1.5"
    @State private var report: CalibrationQualityReport?
    @State private var directory: URL?
    @State private var busy = false
    @State private var error: String?
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    private var valid: Bool {
        guard let s = Double(sigma), let a = Double(tolerance), let l = Double(minimum), let h = Double(maximum) else { return false }
        return s.isFinite && (1...100).contains(s) && a.isFinite && a >= 0 && l > 0 && l < 1 && h > 1 && h <= 100
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack { Text(t("母片像素检查与坏像素图", "Master quality and defect map")).font(.headline); Spacer(); Text(entry.label).foregroundStyle(.secondary) }
            HStack(alignment: .top, spacing: 18) {
                VStack(alignment: .leading, spacing: 10) {
                    Form {
                        TextField(t("暗场 / 偏置阈值（σ）", "Dark / bias threshold (σ)"), text: $sigma)
                        TextField(t("最小绝对差（原始计数）", "Minimum absolute difference"), text: $tolerance)
                        TextField(t("平场最低相对响应", "Minimum flat response"), text: $minimum)
                        TextField(t("平场最高相对响应", "Maximum flat response"), text: $maximum)
                    }.disabled(busy)
                    Text(t("按颜色分别查找异常像素。调整阈值后查看白点分布。", "Measures four CFA phases or one mono plane. Dark/bias uses median and MAD; flats use response relative to the median. Nonfinite samples are always flagged."))
                        .font(.caption).foregroundStyle(.secondary)
                    if let report {
                        Text(t("候选坏点", "Candidate defects") + ": \(report.rejected) / \(report.width * report.height)").font(.headline)
                        ForEach(report.phases.indices, id: \.self) { q in
                            let p = report.phases[q]
                            Text("\(q + 1) · " + t("中位数", "Median") + " \(p.median.formatted()) · σ \(p.madSigma.formatted())\n" + t("低 / 高 / 无效", "Low / high / invalid") + " \(p.low) / \(p.high) / \(p.invalid)").font(.caption)
                        }
                    }
                    Spacer()
                }.frame(width: 360)
                if let directory, let image = NSImage(contentsOf: directory.appendingPathComponent("preview.png")) {
                    Image(nsImage: image).resizable().interpolation(.none).scaledToFit().frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    ContentUnavailableView(t("等待检查", "Ready to inspect"), systemImage: "viewfinder")
                }
            }
            Text(t("白点表示疑似坏点，暗角和灰尘也可能被标记。确认后导出，在校准时使用。", "White marks are threshold candidates, not proof of permanent sensor damage. Flat vignetting or dust may be flagged. Review the map before selecting bad-pixels.fits for calibration. Reduced previews retain visible defects."))
                .font(.caption).foregroundStyle(.secondary)
            if let error { ProcessingErrorNotice(message:error,chinese:chinese) }
            HStack {
                if busy { ProgressView().controlSize(.small) }
                if let directory { Button(t("查看文件", "Show files")) { NSWorkspace.shared.activateFileViewerSelecting([directory.appendingPathComponent("bad-pixels.fits")]) } }
                Spacer(); Button(t("关闭", "Close")) { dismiss() }.disabled(busy).keyboardShortcut(.cancelAction)
                Button(t("检查并保存…", "Inspect and save…")) { inspect() }.disabled(busy || !valid)
                    .buttonStyle(.borderedProminent).accessibilityIdentifier("calibration.quality-run")
            }
        }.padding(20).frame(width: 900, height: 600).interactiveDismissDisabled(busy)
            .onChange(of: [sigma, tolerance, minimum, maximum]) { _, _ in report = nil; directory = nil }
    }
    private func inspect() {
        guard valid else { return }
        let panel = NSSavePanel(); panel.nameFieldStringValue = "Sensor quality"
        if panel.runModal() == .OK, let url = panel.url {
            busy = true; error = nil; report = nil; directory = nil
            Task {
                do { report = try await library.inspectQuality(entry, sigma: Double(sigma)!, tolerance: Double(tolerance)!, flatMin: Double(minimum)!, flatMax: Double(maximum)!, to: url); directory = url }
                catch { self.error = error.localizedDescription }
                busy = false
            }
        }
    }
}
