import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct CalibrationLibraryView: View {
    @StateObject private var session: CalibrationLibrarySession
    let chinese: Bool
    let openProject: (URL) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var reviewing = false
    @State private var importing = false
    @State private var building = false
    @State private var constructionEntry: CalibrationLibrary.Entry?
    @State private var calibrationJob: CalibrationJobLaunch?
    @State private var reviewEntry: CalibrationLibrary.Entry?
    @State private var declaring = false
    @State private var batchLaunch: CalibrationBatchLaunch?
    @State private var qualityEntry: CalibrationLibrary.Entry?
    init(library: CalibrationLibrary, lights: [URL], chinese: Bool, openProject: @escaping (URL) -> Void) {
        _session = StateObject(wrappedValue: CalibrationLibrarySession(library: library, lights: lights))
        self.chinese = chinese
        self.openProject = openProject
    }
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label(t("校准库", "Calibration library"), systemImage: "externaldrive").font(.headline)
                Picker("", selection: $reviewing) {
                    Text(t("母片管理", "Masters")).tag(false)
                    Text(t("检查匹配", "Match review")).tag(true)
                }.pickerStyle(.segmented).frame(width: 260).accessibilityIdentifier("calibration.page")
                Button(t("打开校准任务…", "Open calibration task…")) { openJob() }.disabled(session.busy)
                    .accessibilityIdentifier("calibration.open-job")
                Button(t("打开草稿…", "Open draft…")) { openDraft() }.disabled(session.busy)
                    .accessibilityIdentifier("calibration.open-draft")
                Button(t("打开批处理…", "Open batch…")) {
                    let panel = NSOpenPanel(); panel.canChooseDirectories = true; panel.canChooseFiles = false
                    if panel.runModal() == .OK, let url = panel.url { batchLaunch = .init(directory: url) }
                }.disabled(session.busy).accessibilityIdentifier("calibration.open-batch")
                Spacer()
                if session.busy {
                    ProgressView().controlSize(.small)
                    Button(t("停止", "Stop")) { session.cancel() }.accessibilityIdentifier("calibration.stop")
                }
                Button(t("关闭", "Close")) { session.cancel(); dismiss() }.keyboardShortcut(.cancelAction)
            }
            // Keep the native list/split views alive across tab switches. Removing
            // the focused outline during NSSegmentedCell tracking can create an
            // accessibility focus graph cycle on macOS.
            ZStack {
                masters.opacity(reviewing ? 0 : 1).allowsHitTesting(!reviewing).disabled(reviewing).accessibilityHidden(reviewing)
                review.opacity(reviewing ? 1 : 0).allowsHitTesting(reviewing).disabled(!reviewing).accessibilityHidden(!reviewing)
            }
            if let error = session.error {
                ProcessingErrorNotice(message:error,chinese:chinese)
            }
        }.padding(18).frame(width: 1040, height: 760)
            .task { session.perform { try await session.reload() } }
            .onDisappear { session.cancel() }
            .onChange(of: session.archived) { _, _ in session.perform { try await session.reload() } }
            .onChange(of: session.selectedMasters) { _, _ in session.invalidate() }
            .onChange(of: session.tolerance) { _, _ in session.invalidate() }
            .onChange(of: session.selection) { _, _ in session.verifiedSelectionChanged() }
            .sheet(isPresented: $importing) {
                CalibrationMasterImportView(session: session, chinese: chinese, close: { importing = false })
            }
            .sheet(isPresented: $building, onDismiss: { session.perform { try await session.reload() } }) {
                CalibrationMasterBuildView(library: session.library, entries: session.entries, chinese: chinese) { id in
                    session.perform { try await session.reload(); session.selection = id }
                }
            }
            .sheet(item: $constructionEntry) { entry in
                CalibrationConstructionView(library: session.library, entry: entry, chinese: chinese)
            }
            .sheet(item: $calibrationJob) { launch in
                CalibrationJobView(library: session.library, launch: launch, chinese: chinese, openProject: openProject)
            }
            .sheet(item: $reviewEntry, onDismiss: { session.perform { try await session.reload() } }) { entry in
                CalibrationMasterReviewView(library: session.library, entry: entry, chinese: chinese)
            }
            .sheet(isPresented: $declaring) {
                CalibrationDeclarationView(library: session.library, chinese: chinese)
            }
            .sheet(item: $batchLaunch) { launch in
                CalibrationBatchView(library: session.library, launch: launch, chinese: chinese, openProject: openProject)
            }
            .sheet(item: $qualityEntry) { entry in
                CalibrationQualityView(library: session.library, entry: entry, chinese: chinese)
            }
    }
    private var masters: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Button(t("添加母片…", "Add master…")) { session.clearError(); importing = true }.disabled(session.busy || session.archived)
                    .accessibilityIdentifier("calibration.add-master")
                Button(t("制作母片…", "Build master…")) { session.clearError(); building = true }.disabled(session.busy || session.archived)
                    .accessibilityIdentifier("calibration.build-master")
                Button(t("重新载入", "Reload")) { session.perform { try await session.reload() } }.disabled(session.busy)
                Button(t("修正拍摄参数…", "Declare acquisition…")) { declaring = true }.disabled(session.busy)
                    .accessibilityIdentifier("calibration.declare")
                Spacer()
                Toggle(t("已归档", "Archived"), isOn: $session.archived).disabled(session.busy).accessibilityIdentifier("calibration.archived")
            }
            HSplitView {
                List(selection: $session.selection) {
                    ForEach(session.entries) { entry in
                        HStack {
                            if !entry.archived, entry.receipt?.rejected == false, CalibrationPlanReport.roles.contains(entry.receipt?.role ?? "") {
                                Toggle("", isOn: Binding(get: { session.selectedMasters.contains(entry.id) }, set: { value in
                                    if value { session.selectedMasters.insert(entry.id) } else { session.selectedMasters.remove(entry.id) }
                                })).labelsHidden().help(t("参与本次匹配", "Include in this review"))
                                    .accessibilityLabel(t("参与匹配：", "Include in review: ") + entry.label)
                                    .accessibilityIdentifier("calibration.include." + entry.id)
                            }
                            VStack(alignment: .leading, spacing: 3) {
                                Text(entry.label).lineLimit(1)
                                Text(entry.receipt.map { CalibrationPresentation.role($0.role, chinese: chinese) } ?? t("记录损坏", "Damaged record"))
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                        }.tag(entry.id)
                    }
                }.listStyle(.bordered).frame(minWidth: 230, idealWidth: 270, maxWidth: 320).disabled(session.busy)
                    .accessibilityIdentifier("calibration.masters")
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) {
                        if let entry = session.selectedEntry {
                            Text(entry.label).font(.headline)
                            if let r = entry.receipt {
                                if r.rejected { Label(t("已拒绝参与校准", "Rejected for calibration"), systemImage: "xmark.circle").foregroundStyle(.red) }
                                Button(t("检查并编辑…", "Review and edit declarations…")) { reviewEntry = entry }
                                    .disabled(session.busy).accessibilityIdentifier("calibration.edit-review")
                                Button(t("检查母片质量…", "Inspect pixels and defects…")) { qualityEntry = entry }
                                    .disabled(session.busy).accessibilityIdentifier("calibration.quality")
                                Text(t("偏置状态：", "Bias state: ") + (r.biasState == "included" ? t("仍含偏置", "Included") : r.biasState == "removed" ? t("已扣除", "Removed") : t("尚未确认", "Not confirmed")))
                                if !r.notes.isEmpty { Text(r.notes).font(.callout).textSelection(.enabled) }
                                HStack {
                                    Button(t("重新核对文件", "Verify file")) { session.inspectSelected() }.accessibilityIdentifier("calibration.verify")
                                    Button(t("导出母片…", "Export master…")) { exportMaster(entry) }.accessibilityIdentifier("calibration.export-master")
                                    Button(entry.archived ? t("恢复", "Restore") : t("归档", "Archive")) {
                                        session.perform { try await session.library.archive(entry); try await session.reload() }
                                    }.accessibilityIdentifier("calibration.archive")
                                }.disabled(session.busy)
                                if r.construction != nil {
                                    Button(t("制作记录…", "Construction record…")) { constructionEntry = entry }
                                        .disabled(session.busy).accessibilityIdentifier("calibration.construction")
                                }
                                Text(session.verified != nil ? t("文件和导入记录一致。", "File matches the import record.") : t("下方为导入时参数；匹配前会重新核对文件。", "Parameters recorded at import; files are rechecked before matching."))
                                    .font(.caption).foregroundStyle(.secondary)
                                CalibrationAcquisitionDetails(source: session.verified ?? r.acquisition, chinese: chinese)
                            }
                            if let issue = entry.issue { Text(issue).foregroundStyle(.red).textSelection(.enabled) }
                        } else {
                            ContentUnavailableView(t("添加已有母片", "Add existing masters"), systemImage: "externaldrive.badge.plus",
                                description: Text(t("保存偏置、暗场和平场，供不同日期和滤镜的素材检查匹配。", "Store bias, dark and flat masters for review across dates and filters.")))
                        }
                    }.frame(maxWidth: .infinity, alignment: .leading).padding(.leading, 12)
                }
            }
        }
    }
    private var review: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Button(t("选择亮场或文件夹…", "Choose lights or folders…")) { chooseLights() }.disabled(session.busy)
                    .accessibilityIdentifier("calibration.choose-lights")
                Text("\(session.lights.count) FITS · \(session.selectedMasters.count) " + t("母片候选", "master candidates")).font(.callout)
                Spacer()
                Text(t("温度容差", "Temperature tolerance"))
                TextField("°C", text: $session.tolerance).frame(width: 48).disabled(session.busy).accessibilityIdentifier("calibration.tolerance")
                Text("°C")
                Button(t("检查匹配", "Check matches")) { session.check() }.buttonStyle(.borderedProminent)
                    .disabled(session.busy || session.lights.isEmpty || session.archived || session.parsedTolerance == nil)
                    .accessibilityIdentifier("calibration.check")
            }
            if session.parsedTolerance == nil {
                Text(t("温度容差请输入 0–10°C 内的数值。", "Enter a temperature tolerance between 0 and 10°C."))
                    .font(.caption).foregroundStyle(.orange)
            }
            if session.archived { Text(t("返回母片管理并关闭“已归档”，再检查当前母片。", "Turn off Archived in Masters to review active candidates.")).foregroundStyle(.orange) }
            ZStack {
                if let report = session.report {
                    VStack {
                        CalibrationReviewView(report: report, chinese: chinese,
                            masterLabels: masterLabels,
                            groupSelection: $session.groupSelection, frameSelection: $session.frameSelection)
                    }.accessibilityElement(children: .contain)
                        .opacity(session.reportIsCurrent ? 1 : 0).allowsHitTesting(session.reportIsCurrent)
                        .disabled(!session.reportIsCurrent).accessibilityHidden(!session.reportIsCurrent)
                }
                ContentUnavailableView(t("先检查拍摄条件", "Check acquisition conditions"), systemImage: "checklist",
                    description: Text(t("选择 FITS 亮场及母片候选，查看分组、缺失信息和不匹配原因。没有母片也能先检查素材分组。", "Choose FITS lights and master candidates to review groups, missing information and conflicts. You can inspect groups before adding masters.")))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .opacity(session.reportIsCurrent ? 0 : 1).allowsHitTesting(!session.reportIsCurrent)
                    .accessibilityHidden(session.reportIsCurrent)
            }
            if let report = session.report, session.reportIsCurrent {
                HStack {
                    Text(t("按 UTC 日期分组；同组内仍逐张核对温度和候选。", "Groups use UTC dates; temperatures and candidates are checked for every frame."))
                        .font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button(t("导出检查结果…", "Export review…")) { exportReview(report) }.disabled(session.busy).accessibilityIdentifier("calibration.export-review")
                    Button(t("分配并校准…", "Assign and calibrate…")) {
                        session.perform {
                            let draft = try CalibrationJobDraft(review: report, entries: session.entries.filter { session.selectedMasters.contains($0.id) })
                            calibrationJob = CalibrationJobLaunch(draft: draft)
                        }
                    }.disabled(session.busy).accessibilityIdentifier("calibration.assign-job")
                }
            }
        }
    }
    private var masterLabels: [String: String] {
        var labels: [String: String] = [:]
        for entry in session.entries {
            if let sha = entry.receipt?.acquisition.sha, labels[sha] == nil || session.selectedMasters.contains(entry.id) {
                labels[sha] = entry.label
            }
        }
        return labels
    }
    private func chooseLights() {
        let panel = NSOpenPanel(); panel.canChooseFiles = true; panel.canChooseDirectories = true; panel.allowsMultipleSelection = true
        panel.message = t("选择 FITS 亮场或包含亮场的文件夹。文件夹内其他格式会跳过。", "Choose FITS lights or folders containing them. Other file formats are skipped.")
        if panel.runModal() == .OK {
            let urls = panel.urls
            session.perform {
                let inputs = try await session.library.scanLights(urls)
                try Task.checkCancellation(); session.setScannedLights(inputs)
            }
        }
    }
    private func openJob() {
        let panel = NSOpenPanel(); panel.canChooseFiles = false; panel.canChooseDirectories = true; panel.allowsMultipleSelection = false
        panel.message = t("选择包含 job.txt 的校准任务文件夹。", "Choose a calibration task folder containing job.txt.")
        if panel.runModal() == .OK, let url = panel.url { calibrationJob = CalibrationJobLaunch(directory: url) }
    }
    private func openDraft() {
        let panel = NSOpenPanel(); panel.canChooseFiles = true; panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false; panel.allowedContentTypes = [.json]
        if panel.runModal() == .OK, let url = panel.url {
            session.perform {
                let draft = try await session.library.openDraft(url)
                try Task.checkCancellation(); calibrationJob = CalibrationJobLaunch(draft: draft)
            }
        }
    }
    private func exportMaster(_ entry: CalibrationLibrary.Entry) {
        let panel = NSSavePanel(); panel.nameFieldStringValue = entry.label
        panel.message = t("导出为新文件夹，包含母片与制作记录。", "Export a new folder containing the master and preparation record.")
        if panel.runModal() == .OK, let url = panel.url { session.perform { try await session.library.export(entry, to: url) } }
    }
    private func exportReview(_ report: CalibrationPlanReport) {
        let panel = NSSavePanel(); panel.nameFieldStringValue = "calibration-review.json"
        if panel.runModal() == .OK, let url = panel.url {
            session.perform {
                let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
                let sources = (report.lights + report.masters.map(\.acquisition)).map { URL(fileURLWithPath: $0.path) }
                try WorkspaceExportProtection.validate(url, sources: sources)
                try encoder.encode(report).write(to: url, options: .atomic)
            }
        }
    }
}
