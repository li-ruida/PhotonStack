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
    private static let repository = WorkflowRepository.shared
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
            Text(t("把当前结果的步骤保存一次，给新的目标文件夹重复使用。原片与当前项目不会被改动。", "Save the steps behind your result and reuse them on new folders. Originals and the open project stay unchanged."))
                .foregroundStyle(.secondary)
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    captureSection
                    Divider()
                    librarySection
                    if let workflow = selected {
                        stepSummary(workflow)
                        targetSection(workflow)
                    }
                    if !batch.items.isEmpty { runSection }
                }.frame(maxWidth: .infinity, alignment: .leading)
            }
            if busy { ProgressView().controlSize(.small) }
            if let error = error ?? batch.error ?? model.errorMessage { Text(error).foregroundStyle(.red).textSelection(.enabled).font(.callout) }
        }
        .padding(22).frame(width: 820, height: 700)
        .interactiveDismissDisabled(busy)
        .task { await load() }
        .onChange(of: selection) { _, _ in ready = false; checks = [] }
        .confirmationDialog(t("删除这个已保存的工作流？", "Delete this saved workflow?"), isPresented: $confirmDelete) {
            Button(t("删除工作流", "Delete workflow"), role: .destructive) {
                guard let selected else { return }
                perform {
                    workflows = try await Self.repository.remove(id: selected.id)
                    selection = workflows.first?.id
                }
            }
        } message: { Text(t("只删除配方，不删除照片或已生成的结果。", "Only the recipe is deleted, never photos or results.")) }
    }

    private var captureSection: some View {
        GroupBox(t("1 · 保存当前处理步骤", "1 · Save the current steps")) {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    TextField(t("工作流名称，例如：城市星轨去航迹", "Name, e.g. City star trails"), text: $name)
                        .accessibilityIdentifier("workflow.name")
                    Button(t("读取当前步骤", "Read current steps")) {
                        perform { draft = try await model.captureReusableWorkflow(name: name, redetectSelections: redetect) }
                    }.disabled(locked || name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
                Toggle(t("飞机线 / 云区改为在新照片上自动重新检测（不沿用手工圈选）", "Re-detect trails / clouds on new images instead of reusing manual selections"), isOn: $redetect)
                    .font(.callout)
                if let draft {
                    stepSummary(draft)
                    HStack {
                        Button(t("确认保存工作流", "Save workflow")) {
                            perform {
                                workflows = try await Self.repository.save(draft)
                                selection = draft.id
                                self.draft = nil
                            }
                        }.buttonStyle(.borderedProminent).disabled(locked)
                        Text(t("保存的是上方已应用步骤，不是尚未执行的滑块值。", "Saves applied steps, not unexecuted slider changes.")).font(.caption).foregroundStyle(.secondary)
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
                Text(t("2 · 选择工作流", "2 · Choose a workflow")).font(.headline)
                Spacer()
                Button(t("导入配方", "Import recipe")) { importWorkflow() }.disabled(locked)
                Button(t("导出配方", "Export recipe")) { exportWorkflow() }.disabled(locked || selected == nil)
                Button(t("删除", "Delete"), role: .destructive) { confirmDelete = true }.disabled(locked || selected == nil)
            }
            if workflows.isEmpty {
                Text(t("还没有工作流。先处理一组照片后保存，或导入已有配方。", "No workflows yet. Process a set and save its steps, or import a recipe.")).foregroundStyle(.secondary)
            } else {
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
                            Text(key).foregroundStyle(.secondary)
                            Spacer()
                            Text((step.deepSkyValues ?? step.parameters)[key] ?? "").textSelection(.enabled)
                        }.font(.caption)
                    }
                }
            }
            Text(t("最后按保存的导出设置生成全分辨率 TIFF。", "Exports full-resolution TIFF using the saved export settings.")).font(.caption).foregroundStyle(.secondary)
            ForEach(workflow.notices, id: \.self) { Text($0).font(.caption).foregroundStyle(.secondary) }
        }.padding(10).background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: 8))
    }

    private func targetSection(_ workflow: ReusableWorkflow) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(t("3 · 选择新目标并运行", "3 · Choose new targets and run")).font(.headline)
            Text(t("每个目标单独一个文件夹。读取其 lights 子目录（若存在），否则读取该文件夹顶层；不递归混合其他目标。校准帧放在 darks / biases / flats 子目录。", "One folder per target. Reads its lights subfolder if present, otherwise its top level; never mixes nested targets. Calibration frames go in darks / biases / flats."))
                .font(.caption).foregroundStyle(.secondary)
            HStack {
                Button(t("选择目标文件夹…", "Choose target folders…")) { chooseFolders() }.disabled(locked)
                Text(t("已选 \(folders.count) 个", "\(folders.count) selected")).foregroundStyle(.secondary)
                Spacer()
                Button(t("选择输出位置…", "Choose output location…")) { chooseOutput() }.disabled(locked)
            }
            ForEach(folders, id: \.self) { Text($0.path).font(.caption).textSelection(.enabled) }
            if let output { Text(t("输出：", "Output: ") + output.path).font(.caption).textSelection(.enabled) }
            HStack {
                Button(t("检查目标", "Check targets")) {
                    perform {
                        let targets = folders
                        let result = await Task.detached {
                            targets.map { folder -> (String, Bool) in
                                do {
                                    let plan = try WorkflowInputPlan.inspect(folder: folder, workflow: workflow)
                                    return ("✓ \(plan.title): \(plan.lights.count) photos", true)
                                } catch { return ("✕ \(folder.lastPathComponent): \(error.localizedDescription)", false) }
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
                        Text("\(item.status.rawValue) · \(item.message)").font(.caption).foregroundStyle(.secondary)
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
            workflows = try await Self.repository.load()
            selection = selection ?? workflows.first?.id
        } catch { self.error = error.localizedDescription }
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
            let workflow = try await Self.repository.importWorkflow(from: url)
            workflows = try await Self.repository.load()
            selection = workflow.id
        }
        #endif
    }
    private func exportWorkflow() {
        #if os(macOS)
        guard let selected, let url = NativeFilePanels.exportWorkflowURL(message: t("导出可复用工作流（不含原片路径）", "Export a portable workflow without source paths")) else { return }
        perform { try await Self.repository.exportWorkflow(selected, to: url) }
        #endif
    }
}

extension WorkflowStep {
    func title(chinese: Bool) -> String {
        if deepSkyValues != nil { return chinese ? "筛片 → 校准 → 配准 → 堆栈 → 深空后期" : "Analyze → calibrate → register → stack → develop" }
        guard chinese else { return kind.rawValue }
        let titles: [ProcessingOperationKind: String] = [.rawDecode: "RAW 解码", .stack: "堆栈", .drizzle: "Drizzle 堆栈", .crop: "裁切", .background: "背景校正", .normalize: "归一化", .stretch: "拉伸", .curves: "曲线", .localContrast: "局部对比度", .denoise: "降噪", .sharpen: "锐化", .deconvolve: "反卷积", .colorNeutralize: "色彩中和", .colorSaturate: "饱和度", .starReduce: "缩星", .comaReduce: "彗差修正", .artifactRemove: "去航迹", .cloudRemove: "去云"]
        return titles[kind] ?? kind.rawValue
    }
}
