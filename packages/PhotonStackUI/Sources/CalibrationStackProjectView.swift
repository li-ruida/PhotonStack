import AppKit
import Combine
import SwiftUI

@MainActor
final class CalibrationStackProjectSession: ObservableObject {
    let library: CalibrationLibrary
    let directory: URL
    let expected: CalibrationJobReport
    @Published private(set) var plan: CalibrationStackPlan?
    @Published var selected: Set<Int> = []
    @Published private(set) var busy = false
    @Published private(set) var error: String?
    @Published private(set) var created: URL?
    private var task: Task<Void, Never>?
    init(library: CalibrationLibrary, directory: URL, expected: CalibrationJobReport) {
        self.library = library; self.directory = directory; self.expected = expected
    }
    var frameCount: Int? { guard let plan else { return nil }; return (try? plan.frames(in: selected))?.count }
    func load() {
        guard !busy else { return }; busy = true; plan = nil; error = nil
        task = Task {
            do {
                let value = try await library.stackPlan(directory: directory, expected: expected)
                try Task.checkCancellation(); plan = value
                if let first = value.groups.first(where: { $0.complete }) { selected = [first.id] }
            } catch { if !Task.isCancelled { self.error = error.localizedDescription } }
            busy = false; task = nil
        }
    }
    func create(at destination: URL) {
        guard !busy, let plan, frameCount != nil, created == nil else { return }
        let groups = selected; busy = true; error = nil
        task = Task {
            do { created = try await library.createStackProject(plan, groups: groups, name: destination.lastPathComponent, destination: destination) }
            catch { if !Task.isCancelled { self.error = error.localizedDescription } }
            busy = false; task = nil
        }
    }
    func cancel() { task?.cancel() }
}

struct CalibrationStackProjectView: View {
    @StateObject private var model: CalibrationStackProjectSession
    let chinese: Bool
    let openProject: (URL) -> Void
    @Environment(\.dismiss) private var dismiss
    init(library: CalibrationLibrary, directory: URL, report: CalibrationJobReport, chinese: Bool, openProject: @escaping (URL) -> Void) {
        _model = StateObject(wrappedValue: CalibrationStackProjectSession(library: library, directory: directory, expected: report))
        self.chinese = chinese; self.openProject = openProject
    }
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text(t("进入堆栈", "Continue to stacking")).font(.headline)
                Spacer()
                if model.busy { ProgressView().controlSize(.small) }
                Button(t("关闭", "Close")) { model.cancel(); dismiss() }.keyboardShortcut(.cancelAction)
                    .accessibilityIdentifier("calibration.stack-close")
            }
            if let created = model.created {
                Label(t("堆栈项目已保存", "Stack project saved"), systemImage: "checkmark.circle").font(.headline)
                Text(created.path).font(.callout).textSelection(.enabled)
                Text(t("科学图和校准记录已复制到新项目。打开后按一键成片流程分析、筛片、配准与堆栈。", "Science frames and calibration records were copied into the new project. Open it to analyze, select, register and stack using the one-click workflow."))
                    .foregroundStyle(.secondary)
                Spacer()
                HStack {
                    Button(t("在访达中显示", "Show in Finder")) { NSWorkspace.shared.activateFileViewerSelecting([created]) }
                    Spacer()
                    Button(t("打开堆栈项目", "Open stack project")) { openProject(created) }.buttonStyle(.borderedProminent)
                        .accessibilityIdentifier("calibration.stack-open")
                }
            } else {
                Text(t("选择同一目标、滤镜、曝光和设备设置的已完成分组。可以合并不同 UTC 日期；不同目标或不同滤镜请分别建项目。", "Select completed groups with the same target, filter, exposure and equipment settings. Different UTC dates can be combined; create separate projects for different targets or filters."))
                    .font(.callout).foregroundStyle(.secondary)
                if let plan = model.plan {
                    List(plan.groups) { group in
                        Toggle(isOn: Binding(get: { model.selected.contains(group.id) }, set: { value in
                            if value { model.selected.insert(group.id) } else { model.selected.remove(group.id) }
                        })) {
                            VStack(alignment: .leading, spacing: 3) {
                                Text("\(t("组", "Group")) \(group.id + 1) · " + (group.acquisition.text["object"] ?? "?") + " · " + (group.acquisition.text["filter"] ?? "?"))
                                Text("\(group.frames.count) " + t("帧", "frames") + " · " + String((group.acquisition.text["observedAt"] ?? "?").prefix(10)) + " UTC · " + (group.acquisition.numeric["exposure"].map { $0.formatted() + " s" } ?? "?") + (group.complete ? "" : t(" · 尚未完成", " · Incomplete")))
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                        }.disabled(!group.complete || model.busy).accessibilityIdentifier("calibration.stack-group.\(group.id)")
                    }.listStyle(.bordered)
                } else { Spacer() }
                Text(model.frameCount.map { t("将复制 \($0) 张科学图及校准记录，保留负值与无效像素。", "Copies \($0) science frames and calibration records, preserving signed and missing samples.") } ?? t("请选择共 3–2000 张已完成且拍摄条件相容的亮场。", "Select 3–2000 completed lights with compatible acquisition settings."))
                    .font(.caption).foregroundStyle(.secondary).accessibilityIdentifier("calibration.stack-count")
                if let error = model.error { Text(error).font(.caption).foregroundStyle(.red).textSelection(.enabled).lineLimit(5) }
                HStack {
                    Text(t("有效性掩膜仅保留为记录，不会作为亮场导入。", "Validity masks are retained as records, never imported as light frames."))
                        .font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button(t("保存新堆栈项目…", "Save new stack project…")) { save() }
                        .disabled(model.busy || model.frameCount == nil).buttonStyle(.borderedProminent)
                        .accessibilityIdentifier("calibration.stack-save")
                }
            }
        }.padding(18).frame(width: 790, height: 540)
            .task { model.load() }.onDisappear { model.cancel() }.interactiveDismissDisabled(model.busy)
    }
    private func save() {
        let panel = NSSavePanel(); panel.nameFieldStringValue = t("已校准堆栈项目", "Calibrated stack project")
        panel.message = t("保存到一个新文件夹；原校准任务和当前项目保持不变。", "Save to a new folder, preserving the calibration task and current project.")
        if panel.runModal() == .OK, let url = panel.url { model.create(at: url) }
    }
}
