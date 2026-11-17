import SwiftUI
import UniformTypeIdentifiers

struct MosaicSeamReportView: View {
    let report: MosaicSeamReport
    let chinese: Bool
    @Environment(\.dismiss) private var dismiss
    @State private var pairID = ""
    @State private var channel = 0
    @State private var selectedCell: Int?
    @State private var exporting = false
    @State private var exportError: String?
    private var pair: MosaicSeamPair? { report.pairs.first { $0.id == pairID } ?? report.pairs.first }
    private func text(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    private func number(_ value: Double) -> String { String(format: "%.5g", value) }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text(text("重叠区亮度检查", "Overlap brightness review")).font(.headline)
                Spacer()
                Button(text("导出报告…", "Export report…")) { exporting = true }
                    .accessibilityIdentifier("mosaic.seams.export")
                Button(text("关闭", "Close")) { dismiss() }.keyboardShortcut(.cancelAction)
            }
            Text(text("曝光匹配前后 · \(report.encoding) 输入单位 · 测量画布 \(report.width) × \(report.height)\(report.preview ? " · 预览采样" : " · 成片采样")",
                      "Before / after exposure matching · \(report.encoding) input units · measurement canvas \(report.width) × \(report.height)\(report.preview ? " · preview sampling" : " · final sampling")"))
                .font(.caption).foregroundStyle(.secondary)
            if let pair {
                HStack {
                    Picker(text("天区", "Panels"), selection: Binding(get: { self.pair?.id ?? "" }, set: { pairID = $0; selectedCell = nil })) {
                        ForEach(report.pairs) { pair in Text("#\(pair.first + 1) ↔ #\(pair.second + 1)").tag(pair.id) }
                    }.frame(width: 220).accessibilityIdentifier("mosaic.seams.pair")
                    Picker(text("通道", "Channel"), selection: $channel) {
                        ForEach(0..<report.channels, id: \.self) { Text(["R", "G", "B"][$0]).tag($0) }
                    }.pickerStyle(.segmented).frame(width: 160)
                    Spacer()
                    Button(text("查看整个重叠区", "Whole overlap")) { selectedCell = nil }
                }
                HStack(alignment: .top, spacing: 24) {
                    heatmap(pair, after: false)
                    heatmap(pair, after: true)
                }
                Text(text("每格为局部中值差（#\(pair.second + 1) − #\(pair.first + 1)）。红为正、蓝为负，两图共用色阶 ±\(number(scale(pair)))；灰色无有效样本。点击格子查看数值。",
                          "Cells show local median differences (#\(pair.second + 1) − #\(pair.first + 1)). Red positive, blue negative; shared scale ±\(number(scale(pair))). Gray means no valid samples. Click a cell for values."))
                    .font(.caption).foregroundStyle(.secondary)
                let region = selectedCell.map { pair.cells[$0] } ?? pair.summary
                Text(selectedCell.map { text("分区 \($0 + 1)", "Cell \($0 + 1)") } ?? text("整个重叠区", "Whole overlap"))
                    .font(.subheadline.bold())
                Text(text("有效样本 \(region.before?.samples ?? 0) / \(region.candidates) · 画布范围 ",
                          "Valid samples \(region.before?.samples ?? 0) / \(region.candidates) · canvas bounds ") +
                     region.bounds.map(number).joined(separator: ", ")).font(.caption.monospacedDigit())
                if let before = region.before, let after = region.after {
                    statistics(before: before, after: after, first: pair.first, second: pair.second)
                    let increased = (0..<report.channels).filter {
                        after.rmsDifference[$0] > before.rmsDifference[$0] * 1.01 + 1e-9
                    }.map { ["R", "G", "B"][$0] }.joined(separator: "/")
                    if !increased.isEmpty {
                        Text(text("\(increased) 通道 RMS 增加超过 1%，请同时检查分区差值和亮星。",
                                  "\(increased) RMS increased by more than 1%; inspect local differences and bright stars."))
                            .font(.caption).foregroundStyle(.orange)
                    }
                    if before.samples < 16 {
                        Text(text("样本较少，不能据此判断整段接缝。", "Few samples; this does not establish the whole seam's quality."))
                            .font(.caption).foregroundStyle(.orange)
                    }
                } else {
                    Text(text("该区域没有完整覆盖的有效采样，无法判断亮度差。", "No fully covered samples in this region; brightness difference is unknown."))
                        .foregroundStyle(.secondary)
                }
                DisclosureGroup(text("实际应用的曝光校正", "Applied exposure corrections")) {
                    ForEach([pair.first, pair.second], id: \.self) { index in
                        Text("#\(index + 1) " + report.inputs[index]).font(.caption2).textSelection(.enabled)
                        Text("#\(index + 1)  gain " + report.corrections[index].gains.map(number).joined(separator: " / ") +
                             "  offset " + report.corrections[index].offsets.map(number).joined(separator: " / "))
                            .font(.caption.monospacedDigit()).textSelection(.enabled)
                    }
                }
            } else {
                Text(text("没有相交的天区；没有重叠区亮度数据。", "No intersecting panels; no overlap brightness data."))
            }
            Spacer(minLength: 0)
            Text(text("测量相同天空位置的 3×3 中值，每对天区最多 48×48 点；覆盖率须 ≥99%。星云与亮星仍可能影响统计。这里比较融合前的两块面板，不能代替最终成片接缝或目标保真的目视检查。",
                      "Measures 3×3 medians at common sky positions, up to 48×48 points per pair, with ≥99% coverage. Nebulae and bright stars may affect statistics. These are panel differences before blending, not a verdict on final seams or target preservation."))
                .font(.caption).foregroundStyle(.secondary)
            if let exportError { Text(exportError).foregroundStyle(.red).font(.caption) }
        }
        .padding(20).frame(width: 760, height: 760)
        .fileExporter(isPresented: $exporting, document: SeamJSONDocument(report: report), contentType: .json,
                      defaultFilename: "mosaic-overlap-report") { result in
            if case let .failure(error) = result { exportError = error.localizedDescription }
        }
    }
    private func scale(_ pair: MosaicSeamPair) -> Double {
        max(1e-12, pair.cells.flatMap { [$0.before?.medianDifference[channel], $0.after?.medianDifference[channel]] }
            .compactMap { $0 }.map(abs).max() ?? 0)
    }
    private func heatmap(_ pair: MosaicSeamPair, after: Bool) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(after ? text("匹配后", "After") : text("匹配前", "Before")).font(.subheadline)
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 3), count: 4), spacing: 3) {
                ForEach(0..<16, id: \.self) { index in
                    let stats = after ? pair.cells[index].after : pair.cells[index].before
                    let value = stats?.medianDifference[channel]
                    Button { selectedCell = index } label: {
                        ZStack {
                            RoundedRectangle(cornerRadius: 3).fill(Color.secondary.opacity(0.08))
                            if let value {
                                RoundedRectangle(cornerRadius: 3).fill((value >= 0 ? Color.red : .blue)
                                    .opacity(min(0.75, abs(value) / scale(pair) * 0.75)))
                            }
                            Text(value.map(number) ?? "—").font(.caption2.monospacedDigit())
                        }.frame(height: 40)
                            .overlay(RoundedRectangle(cornerRadius: 3).stroke(selectedCell == index ? Color.accentColor : .clear, lineWidth: 2))
                    }.buttonStyle(.plain).accessibilityIdentifier("mosaic.seams.\(after ? "after" : "before").\(index)")
                }
            }
        }
    }
    private func statistics(before: MosaicSeamStatistics, after: MosaicSeamStatistics, first: Int, second: Int) -> some View {
        Grid(alignment: .leading, horizontalSpacing: 15, verticalSpacing: 6) {
            GridRow {
                Text(text("通道", "Channel")); Text(text("中值差 前 → 后", "Median Δ before → after"))
                Text(text("RMS 前 → 后", "RMS before → after")); Text(text("|差| P95 前 → 后", "|Δ| P95 before → after"))
            }.foregroundStyle(.secondary)
            ForEach(0..<report.channels, id: \.self) { c in
                GridRow {
                    Text(["R", "G", "B"][c])
                    Text("\(number(before.medianDifference[c])) → \(number(after.medianDifference[c]))")
                    Text("\(number(before.rmsDifference[c])) → \(number(after.rmsDifference[c]))")
                    Text("\(number(before.p95Absolute[c])) → \(number(after.p95Absolute[c]))")
                }
            }
            Divider().gridCellColumns(4)
            GridRow {
                Text(text("面板中值", "Panel median")); Text("#\(first + 1) " + text("前 → 后", "before → after"))
                Text("#\(second + 1) " + text("前 → 后", "before → after"))
            }.foregroundStyle(.secondary)
            ForEach(0..<report.channels, id: \.self) { c in
                GridRow {
                    Text(["R", "G", "B"][c])
                    Text("\(number(before.firstMedian[c])) → \(number(after.firstMedian[c]))")
                    Text("\(number(before.secondMedian[c])) → \(number(after.secondMedian[c]))")
                }
            }
        }.font(.caption.monospacedDigit()).textSelection(.enabled)
    }
}
private struct SeamJSONDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.json] }
    var report: MosaicSeamReport
    init(report: MosaicSeamReport) { self.report = report }
    init(configuration: ReadConfiguration) throws {
        report = try JSONDecoder().decode(MosaicSeamReport.self, from: configuration.file.regularFileContents ?? Data())
    }
    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return FileWrapper(regularFileWithContents: try encoder.encode(report))
    }
}
