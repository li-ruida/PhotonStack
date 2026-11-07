import PhotonStackAppCore
import SwiftUI

/// Track the exposure path, since frame ordinals can be reused by a later
/// report. A shrinking filtered list must not mark another exposure for edit.
struct DeepSkyReviewFocus {
    var selected = Set<String>()
    var focused: String?

    mutating func open(_ path: String?) {
        selected = Set([path].compactMap { $0 })
        focused = path
    }

    mutating func select(_ paths: Set<String>, in order: [String]) {
        let added = paths.subtracting(selected)
        selected = paths
        if added.count == 1 {
            focused = added.first
        } else if focused.map(paths.contains) != true {
            focused = order.first(where: paths.contains)
        }
    }

    mutating func reconcile(with visible: [String]) {
        select(selected.intersection(visible), in: visible)
    }
}

struct DeepSkyReviewView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let report: DeepSkyReport
    let settings: DeepSkySettings
    let reportIsCurrent: Bool
    @Binding var filter: DeepSkyReviewFilter
    @Binding var query: String
    @State private var sort: DeepSkyReviewSort = .attention
    @State private var focus = DeepSkyReviewFocus()
    @State private var undo: [[String: String]] = []
    @State private var notice: String?
    @State private var exclusionCode: String?
    @State private var prepared: (report: DeepSkyReport, evidence: DeepSkyReviewEvidence)?
    private var chinese: Bool { model.language == .simplifiedChinese }

    var body: some View {
        Group {
            if let prepared, prepared.report == report {
                workbench(context: prepared.evidence)
            } else {
                ProgressView().accessibilityLabel(chinese ? "读取筛片结果" : "Loading frame review")
            }
        }
        .onChange(of: report, initial: true) { _, value in
            prepared = (value, DeepSkyReviewEvidence(report: value))
            undo = []
            notice = nil
            exclusionCode = nil
            focus = DeepSkyReviewFocus()
        }
    }

    private struct Scope: Equatable {
        var filter: DeepSkyReviewFilter
        var query: String
        var exclusionCode: String?
    }

    private func workbench(context: DeepSkyReviewEvidence) -> some View {
        let selection = DeepSkyReviewSelection(report: report, settings: settings, isCurrent: reportIsCurrent)
        let listing = DeepSkyReview.listing(report.frames, selection: selection, filter: filter, query: query, sort: sort)
        let visible: [DeepSkyReport.Frame]
        if filter == .excluded, let exclusionCode {
            visible = listing.frames.filter { DeepSkyReviewEvidence.exclusionCodes($0, selection: selection).contains(exclusionCode) }
        } else {
            visible = listing.frames
        }
        let paths = visible.map(\.input)
        let chosen = visible.filter { focus.selected.contains($0.input) }
        let focused = visible.first { $0.input == focus.focused }
        return VStack(alignment: .leading, spacing: 8) {
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 12) {
                    scopeControls(listing.counts, selection: selection)
                    Spacer(minLength: 0)
                    searchAndSort
                }
                VStack(alignment: .leading, spacing: 6) {
                    scopeControls(listing.counts, selection: selection)
                    searchAndSort
                }
            }
            if filter == .excluded {
                exclusionReasons(DeepSkyExclusionSummary(report: report, selection: selection, chinese: chinese))
            }
            GeometryReader { geometry in
                let previewHeight = max(240, min(440, geometry.size.height * 0.52))
                if geometry.size.width >= 730 {
                    HSplitView {
                        frameList(visible, paths: paths, context: context, selection: selection)
                            .frame(minWidth: 245, idealWidth: 315, maxWidth: 420)
                        detail(focused, in: visible, context: context, selection: selection, previewHeight: previewHeight)
                            .padding(.leading, 10).frame(minWidth: 410)
                    }
                } else {
                    VStack(spacing: 5) {
                        frameList(visible, paths: paths, context: context, selection: selection)
                            .frame(height: min(185, max(100, geometry.size.height * 0.32)))
                        Divider()
                        detail(focused, in: visible, context: context, selection: selection, previewHeight: 240)
                    }
                }
            }
            actions(chosen, selection: selection)
        }
        .onChange(of: Scope(filter: filter, query: query, exclusionCode: exclusionCode), initial: true) { _, _ in
            focus.open(representative(in: visible, selection: selection))
        }
        .onChange(of: paths) { _, updated in focus.reconcile(with: updated) }
    }

    private func representative(in frames: [DeepSkyReport.Frame], selection: DeepSkyReviewSelection) -> String? {
        let rejected = frames.first { $0.usable && !$0.recommendedKeep && !selection.kept($0) }
            ?? frames.first { !selection.kept($0) }
        return (rejected ?? frames.first)?.input
    }

    private func show(_ scope: DeepSkyReviewFilter, selection: DeepSkyReviewSelection) {
        // Clicking a count always opens the entire set, even after a filename search.
        if filter == scope && query.isEmpty && exclusionCode == nil {
            let frames = DeepSkyReview.listing(report.frames, selection: selection, filter: scope, query: "", sort: sort).frames
            focus.open(representative(in: frames, selection: selection))
        }
        exclusionCode = nil
        filter = scope
        query = ""
    }

    private func scopeControls(_ counts: [DeepSkyReviewFilter: Int], selection: DeepSkyReviewSelection) -> some View {
        HStack(spacing: 6) {
            Text(reportIsCurrent ? (chinese ? "本次堆栈" : "Next stack") : (chinese ? "上次报告" : "Last report"))
                .font(.callout.weight(.semibold)).accessibilityIdentifier("review.planned-count")
            ForEach([DeepSkyReviewFilter.kept, .excluded], id: \.self) { scope in
                Button { show(scope, selection: selection) } label: {
                    Label("\(scope.title(chinese: chinese)) \(counts[scope, default: 0])",
                          systemImage: scope == .kept ? "checkmark.circle" : "minus.circle")
                        .monospacedDigit().fixedSize()
                }
                .buttonStyle(.bordered)
                .tint(filter == scope ? Color.accentColor : .secondary)
                .accessibilityIdentifier("review.filter.\(scope.rawValue)")
                .accessibilityAddTraits(filter == scope ? .isSelected : [])
            }
            Menu {
                ForEach([DeepSkyReviewFilter.all, .attention, .manual], id: \.self) { scope in
                    Button { show(scope, selection: selection) } label: {
                        if filter == scope { Label("\(scope.title(chinese: chinese)) \(counts[scope, default: 0])", systemImage: "checkmark") }
                        else { Text("\(scope.title(chinese: chinese)) \(counts[scope, default: 0])") }
                    }
                }
            } label: {
                if [.all, .attention, .manual].contains(filter) {
                    Text("\(filter.title(chinese: chinese)) \(counts[filter, default: 0])")
                } else {
                    Text(chinese ? "其他名单" : "Other lists")
                }
            }.fixedSize().accessibilityIdentifier("review.filter.more")
        }
    }

    private var searchAndSort: some View {
        HStack(spacing: 8) {
            TextField(chinese ? "搜索文件、序号或原因" : "Search file, number or reason", text: $query)
                .textFieldStyle(.roundedBorder).frame(minWidth: 135, maxWidth: 270)
                .accessibilityIdentifier("review.search")
            Picker(chinese ? "排序" : "Sort", selection: $sort) {
                ForEach(DeepSkyReviewSort.allCases, id: \.self) { Text($0.title(chinese: chinese)).tag($0) }
            }.fixedSize().labelsHidden().accessibilityLabel(chinese ? "排序" : "Sort")
        }
    }

    @ViewBuilder private func exclusionReasons(_ summary: DeepSkyExclusionSummary) -> some View {
        if let first = summary.items.first {
            HStack(spacing: 8) {
                Text(chinese ? "排除原因" : "Exclusion reasons").fontWeight(.medium)
                if summary.items.count == 1 && exclusionCode == nil {
                    Text("\(first.title) · \(first.count) " + (chinese ? "张" : "frames"))
                } else {
                    let active = summary.items.first { $0.code == exclusionCode }
                    Menu {
                        Button(chinese ? "全部原因" : "All reasons") { exclusionCode = nil }
                        Divider()
                        ForEach(summary.items) { item in
                            Button("\(item.title) · \(item.count)") { exclusionCode = item.code; query = "" }
                        }
                    } label: {
                        Text(active.map { "\($0.title) · \($0.count)" } ??
                             (exclusionCode == nil ? (chinese ? "全部 \(summary.items.count) 种" : "All \(summary.items.count) reasons")
                                : (chinese ? "该原因已无排除帧" : "No exclusions for this reason")))
                    }.fixedSize().accessibilityIdentifier("review.exclusion-reasons")
                    if exclusionCode != nil {
                        Button(chinese ? "全部原因" : "All reasons") { exclusionCode = nil }.controlSize(.small)
                    } else {
                        Text("\(first.title) · \(first.count)").lineLimit(1)
                    }
                    if summary.hasMultipleReasonsPerFrame {
                        Text(chinese ? "一帧可有多个原因" : "A frame can have multiple reasons").foregroundStyle(.secondary)
                    }
                }
                Spacer(minLength: 0)
            }.font(.caption).accessibilityIdentifier("review.exclusion-summary")
        } else if exclusionCode != nil {
            Button(chinese ? "清除原因筛选" : "Clear reason filter") { exclusionCode = nil }.controlSize(.small)
        }
    }

    private func frameList(_ visible: [DeepSkyReport.Frame], paths: [String], context: DeepSkyReviewEvidence,
                           selection: DeepSkyReviewSelection) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(chinese ? "显示 \(paths.count) 张" : "\(paths.count) listed").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button(chinese ? "全选列表" : "Select listed") { focus.select(Set(paths), in: paths) }
                    .controlSize(.small).disabled(paths.isEmpty)
            }
            if visible.isEmpty {
                VStack(spacing: 7) {
                    Text(chinese ? "没有符合条件的帧" : "No matching frames").foregroundStyle(.secondary)
                    Button(chinese ? "显示全部" : "Show all") { show(.all, selection: selection) }
                }.frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                DeepSkyReviewTable(paths: paths,
                    content: .init(report: report, overrides: settings.overrides, apply: settings.values["selection.apply"],
                                   current: reportIsCurrent, chinese: chinese),
                    selected: focus.selected, focused: focus.focused,
                    row: { index in
                        let frame = visible[index]
                        let evidence = context.frame(frame, selection: selection, chinese: chinese)
                        let description = evidence.summary == evidence.decisionTitle ? evidence.decisionTitle :
                            evidence.decisionTitle + " · " + evidence.summary
                        return .init(index: frame.index, filename: URL(fileURLWithPath: frame.input).lastPathComponent,
                                     decision: description, symbol: !frame.usable ? "exclamationmark.octagon" :
                                        evidence.kept ? "checkmark.circle" : "minus.circle", kept: evidence.kept)
                    }, select: { focus.select($0, in: paths) })
            }
        }
    }

    @ViewBuilder private func detail(_ frame: DeepSkyReport.Frame?, in visible: [DeepSkyReport.Frame],
                                     context: DeepSkyReviewEvidence, selection: DeepSkyReviewSelection,
                                     previewHeight: CGFloat) -> some View {
        if let frame {
            DeepSkyReviewFrameDetail(model: model, frame: frame, reference: report.frames.first(where: \.reference),
                evidence: context.frame(frame, selection: selection, chinese: chinese), previewHeight: previewHeight,
                hasPrevious: frame.input != visible.first?.input, hasNext: frame.input != visible.last?.input,
                navigate: { direction in
                    guard let i = visible.firstIndex(where: { $0.input == frame.input }), visible.indices.contains(i + direction) else { return }
                    focus.open(visible[i + direction].input)
                })
        } else {
            Text(chinese ? "选择一帧查看依据与原片" : "Select a frame to inspect its decision and original")
                .foregroundStyle(.secondary).frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    private func actions(_ chosen: [DeepSkyReport.Frame], selection: DeepSkyReviewSelection) -> some View {
        HStack(spacing: 8) {
            Text(chinese ? "已选 \(chosen.count) 张" : "\(chosen.count) selected").font(.callout).monospacedDigit()
            let hasUsable = chosen.contains(where: \.usable)
            Button { mark("keep", chosen: chosen, selection: selection) } label: {
                Label(chinese ? "保留" : "Keep", systemImage: "checkmark")
            }.keyboardShortcut("k", modifiers: .command).disabled(!hasUsable).accessibilityIdentifier("review.keep")
            Button { mark("reject", chosen: chosen, selection: selection) } label: {
                Label(chinese ? "排除" : "Exclude", systemImage: "minus.circle")
            }.keyboardShortcut("r", modifiers: .command).disabled(!hasUsable).accessibilityIdentifier("review.exclude")
            Button(chinese ? "恢复自动" : "Reset to auto") { mark("auto", chosen: chosen, selection: selection) }
                .disabled(chosen.isEmpty).accessibilityIdentifier("review.auto")
            Button { undoLast() } label: {
                Label(chinese ? "撤销标记" : "Undo decision", systemImage: "arrow.uturn.backward")
            }.disabled(undo.isEmpty).accessibilityIdentifier("review.undo")
            Spacer(minLength: 0)
            if let notice { Text(notice).font(.caption).foregroundStyle(.secondary).lineLimit(2) }
        }.controlSize(.regular).disabled(!reportIsCurrent || model.isProcessing)
    }

    private func mark(_ value: String, chosen: [DeepSkyReport.Frame], selection: DeepSkyReviewSelection) {
        guard reportIsCurrent, !model.isProcessing else { return }
        let frames = chosen.filter { (value == "auto" || $0.usable) && selection.decision($0) != value }
        guard !frames.isEmpty else { return }
        undo.append(Dictionary(uniqueKeysWithValues: frames.map { ($0.input, selection.decision($0)) }))
        if undo.count > 20 { undo.removeFirst() }
        model.updateDeepSkyOverrides(Dictionary(uniqueKeysWithValues: frames.map { ($0.input, value) }))
        notice = chinese ? "已更新 \(frames.count) 张" : "Updated \(frames.count) frames"
    }

    private func undoLast() {
        guard reportIsCurrent, !model.isProcessing, let previous = undo.popLast() else { return }
        model.updateDeepSkyOverrides(previous)
        notice = chinese ? "已撤销上次标记" : "Last decision undone"
    }
}
