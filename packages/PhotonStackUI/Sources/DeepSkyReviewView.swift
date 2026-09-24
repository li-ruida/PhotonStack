import AppKit
import PhotonStackAppCore
import SwiftUI

struct DeepSkyReviewView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let report: DeepSkyReport
    @State private var filter: DeepSkyReviewFilter = .all
    @State private var sort: DeepSkyReviewSort = .attention
    @State private var query = ""
    @State private var selected = Set<Int>()
    @State private var focused: Int?
    @State private var undo: [[String: String]] = []
    @State private var notice: String?
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var settings: DeepSkySettings { model.deepSkySettings }
    private var visible: [DeepSkyReport.Frame] {
        DeepSkyReview.frames(report.frames, settings: settings, filter: filter, query: query, sort: sort)
    }
    private var focusedFrame: DeepSkyReport.Frame? { visible.first { $0.index == focused } }
    private var chosen: [DeepSkyReport.Frame] { visible.filter { selected.contains($0.index) } }
    private var keptCount: Int { report.frames.filter { DeepSkyReview.kept($0, settings: settings) }.count }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            summary
            HStack(spacing: 12) {
                Picker(chinese ? "显示范围" : "Show", selection: $filter) {
                    ForEach(DeepSkyReviewFilter.allCases, id: \.self) { item in
                        Text("\(item.title(chinese: chinese)) \(count(item))").tag(item)
                    }
                }.pickerStyle(.segmented).labelsHidden().frame(maxWidth: 540)
                TextField(chinese ? "搜索文件名、序号或原因" : "Search name, number or reason", text: $query)
                    .textFieldStyle(.roundedBorder).accessibilityIdentifier("review.search")
                Picker(chinese ? "排序" : "Sort", selection: $sort) {
                    ForEach(DeepSkyReviewSort.allCases, id: \.self) { Text($0.title(chinese: chinese)).tag($0) }
                }.frame(width: 190).labelsHidden()
            }
            HSplitView {
                VStack(alignment: .leading, spacing: 6) {
                    HStack {
                        Text(chinese ? "\(visible.count) 张 · 选中 \(chosen.count) 张" : "\(visible.count) frames · \(chosen.count) selected").font(.caption).foregroundStyle(.secondary)
                        Spacer()
                        Button(chinese ? "全选列表" : "Select listed") { selected = Set(visible.map(\.index)) }
                            .controlSize(.small).disabled(visible.isEmpty)
                    }
                    if visible.isEmpty {
                        VStack(spacing: 12) {
                            Image(systemName: "line.3.horizontal.decrease.circle").font(.largeTitle).foregroundStyle(.secondary)
                            Text(chinese ? "没有符合条件的照片" : "No matching frames")
                            Button(chinese ? "显示全部" : "Show all") { query = ""; filter = .all }
                        }.frame(maxWidth: .infinity, maxHeight: .infinity)
                    } else {
                        ScrollViewReader { proxy in
                            List(selection: $selected) {
                                ForEach(visible) { frame in row(frame).tag(frame.index).id(frame.index) }
                            }.listStyle(.inset).accessibilityIdentifier("review.frames")
                                .onChange(of: focused) { _, id in
                                    if let id { proxy.scrollTo(id, anchor: .center) }
                                }
                                .onChange(of: visible.map(\.index)) { _, _ in
                                    if let focused { proxy.scrollTo(focused, anchor: .center) }
                                }
                        }
                    }
                    Text(chinese ? "⌘ / ⇧ 多选 · ↑ ↓ 浏览" : "⌘ / ⇧ multi-select · ↑ ↓ browse")
                        .font(.caption).foregroundStyle(.secondary)
                }.frame(minWidth: 260, idealWidth: 290, maxWidth: 380)
                VStack(alignment: .leading, spacing: 10) {
                    if let frame = focusedFrame {
                        HStack(alignment: .top) {
                            VStack(alignment: .leading, spacing: 3) {
                                Text(URL(fileURLWithPath: frame.input).lastPathComponent).font(.callout.weight(.semibold)).lineLimit(1).truncationMode(.middle).help(frame.input)
                                Text(decision(frame)).font(.caption).foregroundStyle(DeepSkyReview.kept(frame, settings: settings) ? .green : .orange)
                            }
                            Spacer(minLength: 6)
                            Button { navigate(-1) } label: { Image(systemName: "chevron.up") }
                                .help(chinese ? "上一张（⌥↑）" : "Previous (⌥↑)").keyboardShortcut(.upArrow, modifiers: .option)
                                .disabled(focused == visible.first?.index)
                            Button { navigate(1) } label: { Image(systemName: "chevron.down") }
                                .help(chinese ? "下一张（⌥↓）" : "Next (⌥↓)").keyboardShortcut(.downArrow, modifiers: .option)
                                .disabled(focused == visible.last?.index)
                        }
                        DeepSkyFramePreview(model: model, frame: frame, reference: report.frames.first(where: \.reference))
                        metrics(frame)
                    } else {
                        Text(chinese ? "从左侧选择照片，检查原片后决定是否使用。" : "Select a frame to inspect the original and decide whether to use it.")
                            .foregroundStyle(.secondary).frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                }.padding(.leading, 12).frame(minWidth: 570, maxWidth: .infinity)
            }
            actions
        }
        .onAppear { reconcileSelection() }
        .onChange(of: visible.map(\.index)) { _, _ in reconcileSelection() }
        .onChange(of: selected) { _, ids in
            if !ids.contains(focused ?? -1) { focused = visible.first { ids.contains($0.index) }?.index }
        }
    }

    private var summary: some View {
        HStack(alignment: .firstTextBaseline, spacing: 18) {
            Label(chinese ? "下次堆栈：\(keptCount) / \(report.inputCount) 张" : "Next stack: \(keptCount) / \(report.inputCount)", systemImage: "checkmark.circle")
                .font(.headline).accessibilityIdentifier("review.planned-count")
            Text(chinese ? "已排除 \(report.inputCount - keptCount) 张" : "\(report.inputCount - keptCount) excluded").foregroundStyle(.secondary)
            Spacer()
            Text(chinese ? "标记即时保存，不删除原片" : "Decisions autosave; originals stay intact").font(.caption).foregroundStyle(.secondary)
        }
    }

    private func row(_ frame: DeepSkyReport.Frame) -> some View {
        HStack(spacing: 8) {
            Image(systemName: !frame.usable ? "exclamationmark.octagon" : !DeepSkyReview.kept(frame, settings: settings) ? "minus.circle" : DeepSkyReview.attention(frame) ? "exclamationmark.triangle" : "checkmark.circle")
                .foregroundStyle(!frame.usable || !DeepSkyReview.kept(frame, settings: settings) ? .orange : DeepSkyReview.attention(frame) ? .yellow : .green)
            VStack(alignment: .leading, spacing: 3) {
                Text("#\(frame.index + 1)  " + URL(fileURLWithPath: frame.input).lastPathComponent)
                    .lineLimit(1).truncationMode(.middle)
                Text(rowDetail(frame)).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
        }.padding(.vertical, 3).help(frame.input)
    }
    private func rowDetail(_ frame: DeepSkyReport.Frame) -> String {
        let reason = DeepSkyReview.reasons(frame, chinese: chinese).first
        return decision(frame) + (reason.map { " · " + $0 } ?? (frame.reference ? (chinese ? " · 参考帧" : " · Reference") : ""))
    }
    private func decision(_ frame: DeepSkyReport.Frame) -> String {
        if !frame.usable { return chinese ? "不可用，无法强制保留" : "Unusable; cannot force inclusion" }
        let manual = DeepSkyReview.manual(frame, settings: settings)
        if chinese { return (manual ? "手动" : "自动") + (DeepSkyReview.kept(frame, settings: settings) ? "保留" : "排除") }
        return (manual ? "Manual: " : "Auto: ") + (DeepSkyReview.kept(frame, settings: settings) ? "keep" : "exclude")
    }
    private func count(_ item: DeepSkyReviewFilter) -> Int {
        DeepSkyReview.frames(report.frames, settings: settings, filter: item, query: "", sort: .capture).count
    }
    private func metrics(_ frame: DeepSkyReport.Frame) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 20) {
                metric(chinese ? "星点宽度" : "Star width", frame.starsMeasured && frame.fwhm > 0 ? String(format: "%.2f px", frame.fwhm) : "—",
                       frame.fwhmEstimator == "native-gaussian-v1"
                        ? (chinese ? "原尺寸高斯拟合，\(frame.starShapeCount ?? 0) 颗有效星点；用于比较模糊程度，不代表光学分辨率。" : "Native Gaussian fits to \(frame.starShapeCount ?? 0) stars; a comparative blur measure, not optical resolution.")
                        : (chinese ? "历史分析的星宽估计；重新分析可更新为原尺寸测量。" : "Width estimate from an earlier analysis; analyze again for native measurements."))
                metric(chinese ? "拉长程度" : "Elongation", frame.starsMeasured && frame.fwhm > 0 ? String(format: "%.2f", frame.eccentricity) : "—", chinese ? "偏心率：0 更圆，接近 1 更细长" : "Eccentricity: 0 is round; near 1 is elongated")
                metric(chinese ? "噪声倍率" : "Noise ratio", String(format: "%.2f×", frame.noiseRatio), chinese ? "相对于这组照片的中位数，越高噪声越大" : "Relative to the sequence median")
                metric(chinese ? "有效覆盖" : "Coverage", String(format: "%.1f%%", frame.coverage * 100), chinese ? "对齐后的有效像素覆盖比例" : "Valid coverage after registration")
                Spacer(minLength: 0)
            }
            let reasons = DeepSkyReview.reasons(frame, chinese: chinese)
            Text(reasons.isEmpty ? (chinese ? "未发现整帧异常。可结合原片检查后保留。" : "No whole-frame issue found. Inspect the original before deciding.") : reasons.joined(separator: "；"))
                .font(.callout).foregroundStyle(reasons.isEmpty ? Color.secondary : Color.orange).lineLimit(2).help(reasons.joined(separator: "\n"))
            DisclosureGroup(chinese ? "详细测量与剔除统计" : "Detailed measurements and rejection") {
                ScrollView {
                    Text(String(format: chinese ? "星点 %@ · 匹配 %d · 正残差 %.3f%% · 负残差 %.3f%%\n通道样本剔除 +%llu / −%llu；局部异常通常无需排除整帧。" : "Stars %@ · Matches %d · Positive %.3f%% · Negative %.3f%%\nChannel samples rejected +%llu / −%llu; local anomalies rarely require whole-frame exclusion.", DeepSkyReview.starCountText(frame), frame.matches, frame.positiveFraction * 100, frame.negativeFraction * 100, frame.rejectedHighSamples, frame.rejectedLowSamples))
                        .font(.caption).frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled)
                        .help(DeepSkyReview.starCountHelp(frame, chinese: chinese))
                }.frame(maxHeight: 60)
            }.font(.caption)
        }
    }
    private func metric(_ label: String, _ value: String, _ help: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.callout.monospacedDigit().weight(.medium))
        }.help(help)
    }
    private var actions: some View {
        HStack(spacing: 10) {
            Text(chinese ? "标记选中 \(chosen.count) 张" : "Mark \(chosen.count) selected").font(.callout)
            Button { mark("keep") } label: { Label(chinese ? "保留" : "Keep", systemImage: "checkmark") }
                .keyboardShortcut("k", modifiers: .command).help("⌘K")
                .disabled(chosen.filter(\.usable).isEmpty || model.isProcessing).accessibilityIdentifier("review.keep")
            Button { mark("reject") } label: { Label(chinese ? "排除" : "Exclude", systemImage: "minus.circle") }
                .keyboardShortcut("r", modifiers: .command).help("⌘R")
                .disabled(chosen.filter(\.usable).isEmpty || model.isProcessing).accessibilityIdentifier("review.exclude")
            Button(chinese ? "恢复自动" : "Reset to auto") { mark("auto") }
                .disabled(chosen.isEmpty || model.isProcessing).accessibilityIdentifier("review.auto")
            Button { undoLast() } label: { Label(chinese ? "撤销标记" : "Undo decision", systemImage: "arrow.uturn.backward") }
                .disabled(undo.isEmpty || model.isProcessing).accessibilityIdentifier("review.undo")
            Spacer()
            if let notice { Text(notice).font(.caption).foregroundStyle(.secondary).lineLimit(1) }
        }.controlSize(.regular)
    }
    private func reconcileSelection() {
        let available = Set(visible.map(\.index))
        selected.formIntersection(available)
        if selected.isEmpty, let first = visible.first { selected = [first.index] }
        if !selected.contains(focused ?? -1) { focused = visible.first { selected.contains($0.index) }?.index }
    }
    private func navigate(_ delta: Int) {
        guard let index = visible.firstIndex(where: { $0.index == focused }), visible.indices.contains(index + delta) else { return }
        focused = visible[index + delta].index; selected = [visible[index + delta].index]
    }
    private func mark(_ value: String) {
        let frames = chosen.filter { value == "auto" || $0.usable }
        guard !frames.isEmpty else { return }
        undo.append(Dictionary(uniqueKeysWithValues: frames.map { ($0.input, settings.overrides[$0.input] ?? "auto") }))
        if undo.count > 20 { undo.removeFirst() }
        model.updateDeepSkyOverrides(Dictionary(uniqueKeysWithValues: frames.map { ($0.input, value) }))
        notice = chinese ? "已更新 \(frames.count) 张，可撤销" : "Updated \(frames.count) frames; undo available"
    }
    private func undoLast() {
        guard let previous = undo.popLast() else { return }
        model.updateDeepSkyOverrides(previous)
        notice = chinese ? "已撤销上次标记" : "Last decision undone"
    }
}
