import SwiftUI

struct CalibrationMasterReportView: View {
    let report: CalibrationMasterReport
    let chinese: Bool
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(CalibrationPresentation.role(report.role, chinese: chinese) + " · \(report.sources.count) " + t("帧", "frames")).font(.headline)
            Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 7) {
                row(t("尺寸 / 采样", "Size / sampling"), "\(report.width) × \(report.height) · \(report.sampling)")
                row(t("合成方法", "Combination"), report.method == "median" ? t("中位数", "Median") : t("均值", "Mean"))
                row(t("偏置状态", "Bias state"), report.biasState == "removed" ? t("已扣除", "Removed") : t("仍含偏置", "Included"))
                row(t("实测温度范围", "Measured temperature range"), "\(report.temperatureMin.formatted())…\(report.temperatureMax.formatted()) °C")
                row(t("每像素最低有效帧数", "Minimum samples per pixel"), "\(report.minimumSamples)")
                row(t("无效输出像素", "Invalid output pixels"), "\(report.invalidPixels.formatted()) / \((report.width * report.height).formatted())")
                row(t("缺失输入样本", "Missing input samples"), report.missingSamples.formatted())
            }.font(.callout)
            if report.invalidPixels > 0 {
                Label(t("部分像素有效样本不足，母片中保留为缺失值。", "Some pixels lack enough samples and remain missing in the master."), systemImage: "exclamationmark.triangle")
                    .font(.caption).foregroundStyle(.orange)
            }
            Text(report.inspection.sourcesVerified ? t("原始输入和制作文件已在本次检查中核对。", "Original inputs and construction files were verified in this inspection.") : t("库内母片和制作文件已核对；本次未重新读取原始输入。", "Stored master and construction files verified; original inputs were not reread in this inspection."))
                .font(.caption).foregroundStyle(.secondary).accessibilityIdentifier("calibration.construction-verification")
            Text(t("原始路径和摘要会随制作记录保留。文件一致性不替代真实相机的校准质量检查。", "Original paths and digests are preserved. File integrity does not replace calibration quality checks on real camera data."))
                .font(.caption).foregroundStyle(.secondary)
            if let bias = report.corrections.bias { correction(t("偏置母片", "Bias master"), bias) }
            if let dark = report.corrections.darkFlat { correction(t("暗平场母片", "Dark-flat master"), dark) }
            DisclosureGroup(t("原始帧与归一化系数", "Original frames and normalization factors") + " (\(report.sources.count))") {
                LazyVStack(alignment: .leading, spacing: 9) {
                    ForEach(report.sources.indices, id: \.self) { index in
                        let source = report.sources[index]
                        VStack(alignment: .leading, spacing: 3) {
                            Text(URL(fileURLWithPath: source.path).lastPathComponent).font(.callout)
                            Text(source.path).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                            Text("SHA-256: " + source.sha).font(.caption.monospaced()).textSelection(.enabled)
                            if report.role == "flat" {
                                Text(report.factors[index].prefix(report.sampling == "mono" ? 1 : 4).map { $0.formatted(.number.precision(.fractionLength(0...6))) }.joined(separator: " · ")).font(.caption)
                            }
                        }
                    }
                }.padding(.top, 6)
            }
        }.frame(maxWidth: .infinity, alignment: .leading)
    }
    private func row(_ label: String, _ value: String) -> some View {
        GridRow { Text(label).foregroundStyle(.secondary); Text(value).textSelection(.enabled) }
    }
    private func correction(_ label: String, _ source: CalibrationMasterReport.Source) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(label).font(.callout)
            Text(source.path).font(.caption).textSelection(.enabled)
            Text("SHA-256: " + source.sha).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
        }
    }
}

struct CalibrationConstructionView: View {
    let library: CalibrationLibrary
    let entry: CalibrationLibrary.Entry
    let chinese: Bool
    @Environment(\.dismiss) private var dismiss
    @State private var report: CalibrationMasterReport?
    @State private var error: String?
    @State private var verifySources = false
    @State private var inspectionID = UUID()
    @State private var busy = false
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(t("母片制作记录", "Master construction record")).font(.headline)
                Spacer()
                if busy { ProgressView().controlSize(.small) }
                Button(t("关闭", "Close")) { dismiss() }.keyboardShortcut(.cancelAction)
            }
            Text(entry.label).font(.title3)
            ScrollView {
                if let report { CalibrationMasterReportView(report: report, chinese: chinese).padding(.trailing, 8) }
                if let error { ProcessingErrorNotice(message:error,chinese:chinese) }
            }.frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            Button(t("重新核对原始输入", "Verify original inputs")) { verifySources = true; inspectionID = UUID() }
                .disabled(busy).accessibilityIdentifier("calibration.verify-originals")
        }.padding(18).frame(width: 800, height: 650)
            .task(id: inspectionID) {
                busy = true; error = nil; report = nil
                do {
                    let value = try await library.inspectConstruction(entry, verifySources: verifySources)
                    try Task.checkCancellation(); report = value
                } catch { if !Task.isCancelled { self.error = error.localizedDescription } }
                if !Task.isCancelled { busy = false }
            }
    }
}
