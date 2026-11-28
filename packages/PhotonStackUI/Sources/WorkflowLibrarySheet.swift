import SwiftUI
import PhotonStackAppCore
#if os(macOS)
import AppKit
#endif

@MainActor
struct WorkflowLibrarySheet: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @ObservedObject var batch: WorkflowBatchRun
    @Binding var presented: Bool
    let repository: WorkflowRepository
    @State private var workflows: [ReusableWorkflow] = []
    @State private var selection: UUID?
    @State private var name = ""
    @State private var redetect = false
    @State private var draft: ReusableWorkflow?
    @State private var folders: [URL] = []
    @State private var output: URL?
    @State private var checks: [String] = []
    @State private var ready = false
    @State private var busy = false
    @State private var error: String?
    @State private var confirmDelete = false
    @State private var creating = false
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var selected: ReusableWorkflow? { workflows.first { $0.id == selection } }
    private var locked: Bool { busy || model.hasActiveProcessing || batch.isRunning }
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Label(t("工作流库", "Workflow library"), systemImage: "point.3.connected.trianglepath.dotted").font(.title2.bold())
                Spacer()
                Button(t("关闭", "Close")) { presented = false }.keyboardShortcut(.cancelAction).disabled(busy)
            }
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    librarySection
                    if creating {
                        captureSection
                    }
                    if !creating, let workflow = selected {
                        Divider()
                        stepSummary(workflow)
                        targetSection(workflow)
                    }
                    if !batch.items.isEmpty { runSection }
                }.frame(maxWidth: .infinity, alignment: .leading)
            }
            if busy { ProgressView().controlSize(.small) }
            if let error = error ?? batch.error ?? model.errorMessage {
                Text(localizedWorkflowMessage(error)).foregroundStyle(.red).textSelection(.enabled).font(.callout)
            }
        }
        .padding(20)
        .frame(width: 760, height: selected != nil || draft != nil || !batch.items.isEmpty ? 640 : 360)
        .interactiveDismissDisabled(busy)
        .task { await load() }
        .task(id: selected?.combinesFrames) {
            if selected?.combinesFrames == true { await model.loadDeepSkySchema() }
        }
        .onChange(of: selection) { _, _ in ready = false; checks = []; creating = false }
        .confirmationDialog(t("删除这个已保存的工作流？", "Delete this saved workflow?"), isPresented: $confirmDelete) {
            Button(t("删除工作流", "Delete workflow"), role: .destructive) {
                guard let selected else { return }
                perform {
                    workflows = try await repository.remove(id: selected.id)
                    selection = workflows.first?.id
                }
            }
        } message: { Text(t("只删除工作流，保留照片和处理结果。", "Only the workflow is deleted. Photos and results are kept.")) }
    }

    private var captureSection: some View {
        GroupBox(t("保存当前步骤", "Save current steps")) {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    TextField(t("工作流名称", "Workflow name"), text: $name)
                        .accessibilityIdentifier("workflow.name")
                    Button(t("读取步骤", "Read steps")) {
                        perform { draft = try await model.captureReusableWorkflow(name: name, redetectSelections: redetect) }
                    }.disabled(locked || name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                        .help(t("仅包含已应用的调整。", "Includes applied edits only."))
                }
                Toggle(t("重新检测航迹和云雾", "Redetect trails and clouds"), isOn: $redetect)
                    .font(.callout)
                if let draft {
                    stepSummary(draft)
                    HStack {
                        Button(t("保存工作流", "Save workflow")) {
                            perform {
                                workflows = try await repository.save(draft)
                                selection = draft.id
                                self.draft = nil
                                creating = false
                            }
                        }.buttonStyle(.borderedProminent).disabled(locked)
                        Text(t("仅保存已应用的调整", "Applied edits only")).font(.caption).foregroundStyle(.secondary)
                    }
                }
            }.padding(6)
        }
        .disabled(locked)
        .onChange(of: name) { _, _ in draft = nil }
        .onChange(of: redetect) { _, _ in draft = nil }
    }

    private var librarySection: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Button(creating ? t("取消新建", "Cancel new workflow") : t("新建工作流", "New workflow")) { creating.toggle() }
                    .disabled(locked).accessibilityIdentifier("workflow.new")
                Spacer()
                Button(t("导入工作流…", "Import workflow…")) { importWorkflow() }.disabled(locked)
                Button(t("导出工作流…", "Export workflow…")) { exportWorkflow() }.disabled(locked || selected == nil || creating)
                Button(t("删除", "Delete"), role: .destructive) { confirmDelete = true }.disabled(locked || selected == nil || creating)
            }
            if workflows.isEmpty {
                if !creating {
                    ContentUnavailableView(t("暂无工作流", "No saved workflows"), systemImage: "point.3.connected.trianglepath.dotted")
                        .frame(maxWidth: .infinity)
                }
            } else if !creating {
                Picker(t("已保存", "Saved"), selection: $selection) {
                    ForEach(workflows) { workflow in Text(workflow.name).tag(Optional(workflow.id)) }
                }.disabled(locked).accessibilityIdentifier("workflow.selection")
            }
        }
    }

    private func stepSummary(_ workflow: ReusableWorkflow) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            Label(workflow.combinesFrames ? t("整组堆栈 + 后期 · 每个文件夹生成一张结果", "Stack + finish · One result per folder") : t("逐张后期 · 每张照片生成独立结果", "Per-image finishing · One result per photo"),
                  systemImage: workflow.combinesFrames ? "photo.stack" : "photo")
            ForEach(Array(workflow.steps.enumerated()), id: \.offset) { index, step in
                DisclosureGroup("\(index + 1). \(step.title(chinese: chinese))") {
                    ForEach((step.deepSkyValues ?? step.parameters).keys.sorted(), id: \.self) { key in
                        HStack(alignment: .top) {
                            let field = model.deepSkySchema?.fields.first { $0.key == key }
                            Text(field.map { chinese ? $0.chineseLabel : $0.label }
                                 ?? model.language.parameterTitle(key, kind: step.kind)).foregroundStyle(.secondary)
                            Spacer()
                            let value = (step.deepSkyValues ?? step.parameters)[key] ?? ""
                            if field?.type == "bool" || ["true", "false"].contains(value) {
                                Text(["on", "true"].contains(value) ? t("开启", "On") : t("关闭", "Off"))
                            } else if let field, field.type == "choice" {
                                Text(model.language.deepSkyChoiceTitle(value, field: field))
                            } else {
                                Text(model.language.parameterOption(value, key: key, kind: step.kind)).textSelection(.enabled)
                            }
                        }.font(.caption)
                    }
                }
            }
            Label(t("全分辨率 TIFF", "Full-resolution TIFF"), systemImage: "square.and.arrow.up")
                .font(.caption).foregroundStyle(.secondary)
            ForEach(workflow.notices, id: \.self) {
                Text(localizedWorkflowMessage($0)).font(.caption).foregroundStyle(.secondary)
            }
        }.padding(10).background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: 8))
    }

    private func targetSection(_ workflow: ReusableWorkflow) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(t("处理目标", "Targets")).font(.headline)
            HStack {
                Button(t("选择目标文件夹…", "Choose target folders…")) { chooseFolders() }.disabled(locked)
                    .help(t("每个文件夹一个目标。读取 lights 子目录（若存在），否则读取文件夹顶层。校准帧放在 darks / biases / flats 子目录。", "One folder per target. Reads lights if present, otherwise the folder's top level. Place calibration frames in darks / biases / flats."))
                if !folders.isEmpty {
                    Text(t("\(folders.count) 个", "\(folders.count) selected")).foregroundStyle(.secondary)
                }
                Spacer()
                Button(t("选择输出位置…", "Choose output location…")) { chooseOutput() }.disabled(locked)
            }
            ForEach(folders, id: \.self) { Text($0.path).font(.caption).textSelection(.enabled) }
            if let output { Text(t("输出：", "Output: ") + output.path).font(.caption).textSelection(.enabled) }
            HStack {
                Button(t("检查目标", "Check targets")) {
                    perform {
                        let targets = folders
                        let language = model.language
                        let photoUnit = t("张照片", "photos")
                        let result = await Task.detached {
                            targets.map { folder -> (String, Bool) in
                                do {
                                    let plan = try WorkflowInputPlan.inspect(folder: folder, workflow: workflow)
                                    return ("✓ \(plan.title): \(plan.lights.count) \(photoUnit)", true)
                                } catch { return ("✕ \(folder.lastPathComponent): \(language.localizedMessage(error.localizedDescription))", false) }
                            }
                        }.value
                        checks = result.map(\.0)
                        ready = !result.isEmpty && result.contains { $0.1 }
                    }
                }.disabled(locked || folders.isEmpty)
                Button(t("批量运行", "Run workflow")) {
                    guard let output else { return }
                    error = nil
                    model.startReusableWorkflowBatch(batch, workflow: workflow, folders: folders, output: output)
                }.buttonStyle(.borderedProminent).disabled(locked || !ready || output == nil)
                if batch.isRunning { Button(t("取消运行", "Cancel run")) { model.cancelCurrentTask() } }
            }
            ForEach(Array(checks.enumerated()), id: \.offset) { _, text in Text(text).font(.caption) }
            if ready { Text(t("无法处理的目标会记为失败，其余继续。每次运行新建目录，不覆盖已有结果。", "Invalid targets are reported as failures; other targets continue. Each run creates a new directory.")).font(.caption).foregroundStyle(.secondary) }
        }
    }

    private var runSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(t("运行结果", "Run results")).font(.headline)
                Spacer()
                if let directory = batch.outputDirectory {
                    Button(t("打开结果目录", "Open results")) {
                        #if os(macOS)
                        NSWorkspace.shared.open(directory)
                        #endif
                    }
                }
            }
            let succeeded = batch.items.filter { $0.status == .succeeded }.count
            let failed = batch.items.filter { $0.status == .failed }.count
            Text(t("共 \(batch.items.count) 项 · 已完成 \(succeeded) · 失败 \(failed)", "\(batch.items.count) jobs · \(succeeded) completed · \(failed) failed"))
                .font(.caption).foregroundStyle(.secondary)
            ForEach(batch.items) { item in
                HStack(alignment: .top) {
                    Image(systemName: item.status == .succeeded ? "checkmark.circle.fill" : item.status == .failed ? "exclamationmark.triangle" : "circle")
                        .foregroundStyle(item.status == .failed ? Color.red : Color.secondary)
                    VStack(alignment: .leading) {
                        Text(item.title)
                        Text("\(model.language.processingStatusName(item.status)) · \(localizedWorkflowMessage(item.message))")
                            .font(.caption).foregroundStyle(.secondary)
                        if item.status == .running {
                            Text(t("步骤 \(min(item.completedSteps + 1, batch.stepCount)) / \(batch.stepCount)", "Step \(min(item.completedSteps + 1, batch.stepCount)) / \(batch.stepCount)"))
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    }
                    Spacer()
                    if item.status == .running { ProgressView().controlSize(.small) }
                    if let result = item.output {
                        Button(t("显示成片", "Reveal result")) {
                            #if os(macOS)
                            NSWorkspace.shared.activateFileViewerSelecting([result])
                            #endif
                        }
                    }
                }
            }
        }
    }

    private func load() async {
        do {
            workflows = try await repository.load()
            selection = selection ?? workflows.first?.id
        } catch { self.error = error.localizedDescription }
    }

    private func localizedWorkflowMessage(_ message: String) -> String {
        model.language.localizedMessage(message)
    }
    private func perform(_ action: @escaping @MainActor () async throws -> Void) {
        guard !busy else { return }
        busy = true
        error = nil
        Task { @MainActor in
            defer { busy = false }
            do { try await action() } catch { self.error = error.localizedDescription }
        }
    }
    private func chooseFolders() {
        #if os(macOS)
        if let urls = NativeFilePanels.workflowTargetFolders(message: t("选择一组或多组新目标照片所在的文件夹", "Select folders containing new target photos")) {
            folders = urls; ready = false; checks = []
        }
        #endif
    }
    private func chooseOutput() {
        #if os(macOS)
        if let url = NativeFilePanels.openBatchOutputDirectory(message: t("选择输出位置，每次运行将新建子目录", "Choose a location for a new run directory")) { output = url }
        #endif
    }
    private func importWorkflow() {
        #if os(macOS)
        guard let url = NativeFilePanels.openCurvePresetLibraryURL(message: t("导入工作流 JSON", "Import workflow JSON")) else { return }
        perform {
            let workflow = try await repository.importWorkflow(from: url)
            workflows = try await repository.load()
            selection = workflow.id
        }
        #endif
    }
    private func exportWorkflow() {
        #if os(macOS)
        guard let selected, let url = NativeFilePanels.exportWorkflowURL(message: t("导出可复用工作流（不含原片路径）", "Export a portable workflow without source paths")) else { return }
        perform {
            try await model.exportReusableWorkflow(selected, to: url, repository: repository)
        }
        #endif
    }
}

extension PhotonStackWorkspaceModel {
    func exportReusableWorkflow(_ workflow: ReusableWorkflow, to url: URL, repository: WorkflowRepository) async throws {
        try validateExportDestination(url, includesSidecar: false)
        try await repository.exportWorkflow(workflow, to: url)
    }
}

extension WorkflowStep {
    func title(chinese: Bool) -> String {
        if deepSkyValues != nil { return chinese ? "筛片 → 校准 → 配准 → 堆栈 → 深空后期" : "Analyze → calibrate → register → stack → develop" }
        return (chinese ? AppLanguage.simplifiedChinese : .english).processingOperationName(kind)
    }
}
