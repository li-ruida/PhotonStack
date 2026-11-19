import AppKit
import SwiftUI
import PhotonStackProcessing

struct CalibrationJobView: View {
    @StateObject private var model: CalibrationJobSession
    let chinese: Bool
    let openProject: (URL) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var group = 0
    @State private var frame: Int? = 0
    @State private var stacking = false
    init(library: CalibrationLibrary, launch: CalibrationJobLaunch, chinese: Bool, openProject: @escaping (URL) -> Void) {
        _model = StateObject(wrappedValue: CalibrationJobSession(library: library, launch: launch)); self.chinese = chinese
        self.openProject = openProject
    }
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(t("分组亮场校准", "Grouped light calibration")).font(.headline)
                Spacer()
                if model.busy {
                    ProgressView().controlSize(.small)
                    Button(model.cancelling ? t("正在停止…", "Stopping…") : t("停止并保留进度", "Stop and keep progress")) { model.cancel() }
                        .disabled(model.cancelling).accessibilityIdentifier("calibration.job-stop")
                }
                Button(t("关闭", "Close")) { model.cancel(); dismiss() }.keyboardShortcut(.cancelAction)
                    .accessibilityIdentifier("calibration.job-close")
            }
            HStack(spacing: 18) {
                Label(t("分配母片", "Assign masters"), systemImage: model.directory == nil ? "1.circle" : "checkmark.circle.fill")
                Image(systemName: "chevron.right").foregroundStyle(.tertiary)
                Label(t("执行校准", "Calibrate"), systemImage: model.report?.status == "complete" ? "checkmark.circle.fill" : "2.circle")
                Image(systemName: "chevron.right").foregroundStyle(.tertiary)
                Label(t("查看与恢复", "Inspect and resume"), systemImage: "3.circle")
            }.font(.callout).foregroundStyle(.secondary)
            if model.directory == nil, let draft = model.draft {
                assignment(draft)
            } else {
                taskResult
            }
            if model.busy {
                ProgressView(value: min(1, max(0, model.progress?.progress ?? 0)))
                Text(progressText).font(.caption).foregroundStyle(.secondary)
            }
            if let error = model.error {
                VStack(alignment: .leading, spacing: 4) {
                    Text(t("本次操作未成功。请检查母片分配，以及原片、母片和任务文件是否可读且未被修改。已保存的帧会保留。", "This operation did not succeed. Check master assignments and that source frames, masters and task files are readable and unchanged. Saved frames are retained."))
                        .foregroundStyle(.red).accessibilityIdentifier("calibration.job-error")
                    DisclosureGroup(t("错误详情", "Error details")) {
                        ScrollView { Text(error).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }.frame(maxHeight: 90)
                    }.accessibilityIdentifier("calibration.job-error-details")
                }.font(.caption)
            }
            Text(t("保留线性 CFA / 单色与原始坐标。只写新任务目录，原片不修改；校准完成后仍需配准、堆栈与显影。", "Preserves linear CFA / mono and source coordinates. Writes only to a new task folder; sources stay unchanged. Registration, stacking and development follow calibration."))
                .font(.caption).foregroundStyle(.secondary)
        }.padding(18).frame(width: 1010, height: 710)
            .task { if model.directory != nil { model.refresh() } }
            .onDisappear { model.cancel() }
            .interactiveDismissDisabled(model.busy)
            .sheet(isPresented: $stacking) {
                if let directory = model.directory, let report = model.report {
                    CalibrationStackProjectView(library: model.library, directory: directory, report: report, chinese: chinese, openProject: openProject)
                }
            }
            .onReceive(NotificationCenter.default.publisher(for: .photonStackProcessingProgress).receive(on: RunLoop.main)) { notification in
                if let event = notification.object as? ProcessingProgressEvent { model.accept(event) }
            }
    }
    private var progressText: String {
        if model.cancelling { return t("等待当前进程结束，随后核对已保存帧…", "Waiting for the process to exit, then checking saved frames…") }
        switch model.progress?.stage {
        case "preflight": return t("核对全部来源和拍摄条件…", "Checking sources and acquisition settings…")
        case "calibrate": return t("已校准", "Calibrated") + " \(model.progress?.step ?? 0) / \(model.progress?.total ?? 0)"
        case "verify-products": return t("核对科学图、掩膜与任务记录…", "Verifying images, masks and task records…")
        default: return t("正在核对和处理…", "Verifying and processing…")
        }
    }
    private func assignment(_ draft: CalibrationJobDraft) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(t("已明确分配", "Assignments ready") + " \(draft.readyCount) / \(draft.review.lights.count)").font(.callout)
                    .accessibilityIdentifier("calibration.job-ready")
                Spacer()
                Button(t("填入唯一候选", "Fill unique candidates")) { model.draft?.fillUniqueCandidates() }.accessibilityIdentifier("calibration.job-suggest")
            }
            HSplitView {
                VStack(alignment: .leading, spacing: 8) {
                    Picker(t("素材组", "Group"), selection: $group) {
                        ForEach(draft.review.groups.indices, id: \.self) { i in
                            let g = draft.review.groups[i]
                            Text("\(i + 1) · " + (g.text["object"] ?? "?") + " · " + (g.text["filter"] ?? "?") + " · \(g.members.count)").tag(i)
                        }
                    }.accessibilityIdentifier("calibration.job-group")
                    if draft.review.groups.indices.contains(group) {
                        List(draft.review.groups[group].members, id: \.self, selection: $frame) { i in
                            HStack {
                                Image(systemName: draft.issue(frame: i) == nil ? "checkmark.circle" : "circle.dashed").foregroundStyle(.secondary)
                                Text(URL(fileURLWithPath: draft.review.lights[i].path).lastPathComponent).lineLimit(1)
                            }.tag(i)
                        }.listStyle(.bordered).accessibilityIdentifier("calibration.job-frames")
                    }
                }.frame(minWidth: 280, idealWidth: 300, maxWidth: 340).padding(.trailing, 10)
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) {
                        if draft.review.groups.indices.contains(group) {
                            let g = draft.review.groups[group]
                            Text(t("整组分配", "Assign the entire group")).font(.headline)
                            Text((g.text["utcDate"] ?? "?") + " UTC · " + (g.numeric["exposure"].map { $0.formatted() + " s" } ?? "?"))
                                .font(.caption).foregroundStyle(.secondary)
                            ForEach(0..<3, id: \.self) { role in
                                masterPicker(draft, role: role, frames: g.members, options: draft.groupCandidates(group, role: role), id: "group")
                            }
                            Text(t("整组菜单只列出与组内每张原片都匹配的母片。可在下方逐张修改。", "Group menus include only masters compatible with every member. Individual frames can be changed below."))
                                .font(.caption).foregroundStyle(.secondary)
                            if let frame, g.members.contains(frame) {
                                Divider()
                                Text(t("当前原片", "Selected frame")).font(.headline)
                                Text(URL(fileURLWithPath: draft.review.lights[frame].path).lastPathComponent).textSelection(.enabled)
                                ForEach(0..<3, id: \.self) { role in
                                    masterPicker(draft, role: role, frames: [frame], options: draft.candidates(frame: frame, role: role), id: "frame")
                                }
                                if let issue = draft.issue(frame: frame) { Text(issueText(issue)).font(.caption).foregroundStyle(.orange) }
                            }
                        }
                        Divider()
                        Text(t("仍含偏置的暗场直接从亮场扣除；已去偏置的暗场或仍含偏置的平场需要偏置母片。", "A bias-included dark is subtracted directly. A bias-removed dark or bias-included flat requires a bias master."))
                            .font(.caption).foregroundStyle(.secondary)
                    }.padding(.leading, 12).frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            HStack {
                Text(t("CPU 线程", "CPU threads"))
                Stepper("\(draft.workers)", value: Binding(get: { model.draft?.workers ?? 4 }, set: { model.draft?.workers = $0 }), in: 1...32).frame(width: 85)
                Text(t("内存预算", "Memory budget"))
                Picker("MiB", selection: Binding(get: { model.draft?.memoryMiB ?? 512 }, set: { model.draft?.memoryMiB = $0 })) {
                    ForEach([128, 256, 512, 1024, 2048], id: \.self) { Text("\($0) MiB").tag($0) }
                }.frame(width: 135).labelsHidden()
                Spacer()
                Button(t("选择保存位置并开始…", "Choose destination and start…")) { start() }.buttonStyle(.borderedProminent)
                    .disabled(!model.canStart).accessibilityIdentifier("calibration.job-start")
            }.font(.callout)
        }.disabled(model.busy)
            .onChange(of: group) { _, value in if draft.review.groups.indices.contains(value) { frame = draft.review.groups[value].members.first } }
    }
    private func masterPicker(_ draft: CalibrationJobDraft, role: Int, frames: [Int], options: [Int], id: String) -> some View {
        let values = Set(frames.map { draft.choices[$0][role] })
        let selected = values.count == 1 ? values.first! : -3
        return Picker(CalibrationPresentation.role(CalibrationPlanReport.roles[role], chinese: chinese), selection: Binding(get: { selected }, set: { model.draft?.assign($0, role: role, frames: frames) })) {
            if selected == -3 { Text(t("逐帧分配不同", "Varies by frame")).tag(-3) }
            Text(t("待选择", "Choose…")).tag(CalibrationJobDraft.unresolved)
            Text(t("不使用", "Do not use")).tag(CalibrationJobDraft.omitted)
            ForEach(options, id: \.self) { i in
                Text(draft.entries[i].label + " · " + biasLabel(draft.entries[i].receipt?.biasState ?? "unknown")).tag(i)
            }
        }.accessibilityIdentifier("calibration.job-\(id)-\(CalibrationPlanReport.roles[role])")
    }
    private func biasLabel(_ state: String) -> String {
        state == "included" ? t("含偏置", "Bias included") : state == "removed" ? t("已去偏置", "Bias removed") : t("偏置未确认", "Bias unconfirmed")
    }
    private func issueText(_ code: String) -> String {
        switch code {
        case "unresolved": return t("请逐项选择母片，或明确选择“不使用”。", "Choose each master or explicitly select Do not use.")
        case "bias-or-dark": return t("原始亮场至少需要偏置或暗场。", "Raw lights require at least a bias or dark master.")
        case "dark-needs-bias": return t("已去偏置的暗场还需要偏置母片。", "A bias-removed dark also requires a bias master.")
        case "flat-needs-bias": return t("仍含偏置的平场需要偏置母片。", "A bias-included flat requires a bias master.")
        default: return t("母片偏置状态未确认或选择无效，请返回母片管理核对。", "The master preparation state is unconfirmed or the choice is invalid. Review the master record.")
        }
    }
    private var taskResult: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let directory = model.directory {
                HStack {
                    Text(directory.path).font(.caption).lineLimit(2).textSelection(.enabled)
                    Spacer()
                    Button(t("在访达中显示", "Show in Finder")) { NSWorkspace.shared.activateFileViewerSelecting([directory]) }
                        .disabled(!FileManager.default.fileExists(atPath: directory.path))
                }
            }
            if let report = model.report {
                Text((report.status == "complete" ? t("校准已完成", "Calibration complete") : t("部分完成，可继续", "Partially complete; ready to resume")) + " · \(report.completed) / \(report.total)")
                    .font(.headline).accessibilityIdentifier("calibration.job-summary")
                Text(report.sourcesVerified ? t("本次执行已核对原片、母片与保存产物。", "This run verified source frames, masters and saved products.") : t("已核对保存产物；尚未重新核对原片和母片。恢复时会检查全部来源。", "Saved products verified; original frames and masters have not been rechecked. Resuming verifies all sources."))
                    .font(.caption).foregroundStyle(.secondary).accessibilityIdentifier("calibration.job-verification")
                HSplitView {
                    List(report.frames.indices, id: \.self, selection: $frame) { i in
                        let item = report.frames[i]
                        VStack(alignment: .leading, spacing: 3) {
                            Text(URL(fileURLWithPath: item.source).lastPathComponent).lineLimit(1)
                            Text(t("组", "Group") + " \(item.group + 1) · " + (item.complete ? t("已保存", "Saved") : t("待处理", "Pending"))).font(.caption).foregroundStyle(.secondary)
                        }.tag(i)
                    }.listStyle(.bordered).frame(minWidth: 260, maxWidth: 330).accessibilityIdentifier("calibration.job-products")
                    ScrollView {
                        VStack(alignment: .leading, spacing: 10) {
                            if let frame, report.frames.indices.contains(frame) {
                                let item = report.frames[frame]
                                Text(URL(fileURLWithPath: item.source).lastPathComponent).font(.headline)
                                Text(item.source).font(.caption).textSelection(.enabled)
                                ForEach(CalibrationPlanReport.roles, id: \.self) { role in
                                    VStack(alignment: .leading, spacing: 3) {
                                        Text(CalibrationPresentation.role(role, chinese: chinese)).font(.subheadline.bold())
                                        if let id = item.masters[role] ?? nil, let master = report.masters[id] {
                                            Text(master.path).font(.caption).textSelection(.enabled)
                                            Text(biasLabel(master.biasState)).font(.caption).foregroundStyle(.secondary)
                                        } else { Text(t("不使用", "Not used")).font(.caption).foregroundStyle(.secondary) }
                                    }
                                }
                                if item.complete, let directory = model.directory {
                                    Button(t("查看此帧科学文件", "Reveal this frame's science FITS")) {
                                        NSWorkspace.shared.activateFileViewerSelecting([directory.appendingPathComponent(item.directory).appendingPathComponent("calibrated.fits")])
                                    }.accessibilityIdentifier("calibration.job-reveal-frame")
                                }
                            }
                        }.padding(.leading, 12).frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            } else {
                ContentUnavailableView(model.busy ? t("正在处理校准任务", "Processing calibration task") : t("任务尚未通过核验", "Task not verified"), systemImage: "checklist",
                    description: Text(t("已有完成帧会保留。查看错误信息，或重新打开任务文件夹核对。", "Completed frames are retained. Review the error or reopen the task folder to verify it.")))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            HStack {
                if model.stopped { Text(t("已停止，完成帧已保留。", "Stopped; completed frames retained.")).font(.caption).foregroundStyle(.secondary) }
                Spacer()
                if let report = model.report, report.completed >= 3 {
                    Button(t("进入堆栈…", "Continue to stacking…")) { stacking = true }.disabled(model.busy)
                        .accessibilityIdentifier("calibration.job-stack")
                }
                if model.report == nil, model.draft != nil, let directory = model.directory, !FileManager.default.fileExists(atPath: directory.path) {
                    Button(t("返回分配", "Back to assignments")) { model.retryPreparation() }.disabled(model.busy)
                }
                Button(t("重新核对产物", "Recheck products")) { model.refresh() }.disabled(model.busy).accessibilityIdentifier("calibration.job-refresh")
                Button(model.report?.status == "complete" ? t("核对全部来源", "Verify all sources") : t("继续校准", "Resume calibration")) { model.resume() }
                    .disabled(!model.canResume).buttonStyle(.borderedProminent).accessibilityIdentifier("calibration.job-resume")
            }
        }
    }
    private func start() {
        let panel = NSSavePanel(); panel.nameFieldStringValue = "calibration-job"
        panel.message = t("保存到一个新任务文件夹，之后可重新打开并恢复。", "Save to a new task folder that can be reopened and resumed later.")
        if panel.runModal() == .OK, let url = panel.url { model.start(at: url) }
    }
}
