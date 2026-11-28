import AppKit
import Combine
import SwiftUI
import PhotonStackAppCore

struct CalibrationBatchLaunch: Identifiable {
    let id = UUID()
    var directory: URL?
    var calibration: URL?
    var expected: CalibrationJobReport?
}

@MainActor
final class CalibrationBatchSession: ObservableObject {
    let library: CalibrationLibrary
    let launch: CalibrationBatchLaunch
    @Published private(set) var directory: URL?
    @Published private(set) var batch: CalibrationBatch?
    @Published private(set) var busy = false
    @Published private(set) var stopping = false
    @Published private(set) var error: String?
    @Published var backend = DeepSkyComputeBackend.automatic
    private var task: Task<Void, Never>?
    init(library: CalibrationLibrary, launch: CalibrationBatchLaunch) {
        self.library = library; self.launch = launch; directory = launch.directory
    }
    func cancel() { guard busy else { return }; stopping = true; task?.cancel() }
    func load() {
        guard let directory, !busy else { return }
        perform { self.batch = try await self.library.inspectBatch(directory) }
    }
    func create(at destination: URL) {
        guard directory == nil, !busy, let source = launch.calibration, let expected = launch.expected else { return }
        perform {
            self.batch = try await self.library.createBatch(calibration: source, expected: expected, destination: destination, backend: self.backend)
            self.directory = destination
        }
    }
    func run() {
        guard let directory, batch != nil, !busy else { return }
        perform {
            self.batch = try await self.library.runBatch(directory) { value in await self.accept(value) }
        }
    }
    private func accept(_ value: CalibrationBatch) { batch = value }
    private func perform(_ operation: @escaping @MainActor () async throws -> Void) {
        busy = true; stopping = false; error = nil
        task = Task {
            do { try await operation() }
            catch { if !Task.isCancelled { self.error = error.localizedDescription } }
            busy = false; stopping = false; task = nil
        }
    }
}

struct CalibrationBatchView: View {
    @StateObject private var model: CalibrationBatchSession
    let chinese: Bool
    let openProject: (URL) -> Void
    @Environment(\.dismiss) private var dismiss
    init(library: CalibrationLibrary, launch: CalibrationBatchLaunch, chinese: Bool, openProject: @escaping (URL) -> Void) {
        _model = StateObject(wrappedValue: CalibrationBatchSession(library: library, launch: launch))
        self.chinese = chinese; self.openProject = openProject
    }
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text(t("校准与分组堆栈", "Calibration and grouped stacking")).font(.headline)
                Spacer()
                if model.busy {
                    ProgressView().controlSize(.small)
                    Button(model.stopping ? t("正在停止…", "Stopping…") : t("停止并保留进度", "Stop and keep progress")) { model.cancel() }.disabled(model.stopping)
                        .accessibilityIdentifier("calibration.batch-stop")
                }
                Button(t("关闭", "Close")) { dismiss() }.disabled(model.busy).keyboardShortcut(.cancelAction)
            }
            Text(t("先校准，再按目标分组堆栈。每组保存为独立项目。", "Resume calibration → group matching targets and settings → register, select, stack and develop → save editable projects."))
                .foregroundStyle(.secondary)
            if let batch = model.batch {
                Text(batch.complete ? t("全部分组已完成", "All groups complete") : batch.calibrated ? t("校准完成；可继续未完成的分组", "Calibration complete; unfinished groups can resume") : t("先完成校准，再准备分组", "Finish calibration before preparing groups"))
                    .accessibilityIdentifier("calibration.batch-summary")
                List(batch.items) { item in
                    HStack {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(item.title + " · \(item.count) " + t("帧", "frames"))
                            Text(status(item.status)).font(.caption).foregroundStyle(item.status == "failed" ? .red : .secondary)
                            if !item.message.isEmpty { Text(item.message).font(.caption).textSelection(.enabled) }
                        }
                        Spacer()
                        if let attempt = item.attempt, let root = model.directory, FileManager.default.fileExists(atPath: root.appendingPathComponent(attempt + "/project/project.json").path) {
                            Button(item.status == "complete" ? t("打开项目", "Open project") : t("检查素材项目", "Review input project")) { openProject(root.appendingPathComponent(attempt + "/project")) }.disabled(model.busy)
                        }
                    }
                }.listStyle(.bordered)
            } else {
                Picker(t("计算后端", "Compute backend"), selection: $model.backend) {
                    Text(t("自动", "Automatic")).tag(DeepSkyComputeBackend.automatic)
                    Text("CPU").tag(DeepSkyComputeBackend.cpu)
                    Text("Metal").tag(DeepSkyComputeBackend.metal)
                }.frame(width: 260).disabled(model.busy)
                Text(t("使用默认堆栈设置。同目标、同拍摄条件可跨日期合并，每组需 3–2000 张。", "Uses default deep-sky settings without applying calibration twice. Matching targets/settings across dates combine; groups outside 3–2000 frames remain with an explanation."))
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
            }
            if let error = model.error { ProcessingErrorNotice(message:error,chinese:chinese) }
            HStack {
                if let root = model.directory {
                    Button(t("查看批处理文件夹", "Reveal batch folder")) { NSWorkspace.shared.activateFileViewerSelecting([root]) }
                    Spacer()
                    Button(t("核对记录", "Verify journal")) { model.load() }.disabled(model.busy)
                    Button(t("运行 / 继续", "Run / resume")) { model.run() }.disabled(model.busy || model.batch == nil || model.batch?.complete == true)
                        .buttonStyle(.borderedProminent).accessibilityIdentifier("calibration.batch-run")
                } else {
                    Spacer()
                    Button(t("保存批处理…", "Save batch…")) {
                        let panel = NSSavePanel(); panel.nameFieldStringValue = "Calibration batch"
                        if panel.runModal() == .OK, let url = panel.url { model.create(at: url) }
                    }.disabled(model.busy).buttonStyle(.borderedProminent).accessibilityIdentifier("calibration.batch-save")
                }
            }
            Text(t("可从校准库继续任务。已完成的组会跳过，失败的组可重试。", "Reopen from the calibration library. Completed products are verified and skipped; failed/interrupted groups retry in new directories."))
                .font(.caption).foregroundStyle(.secondary)
        }.padding(18).frame(width: 850, height: 570).task { model.load() }
            .onDisappear { model.cancel() }.interactiveDismissDisabled(model.busy)
    }
    private func status(_ value: String) -> String {
        switch value {
        case "complete": return t("已完成", "Complete")
        case "running": return t("处理中 / 中断后可重试", "Running / retry after interruption")
        case "failed": return t("未完成，可重试", "Failed; retry available")
        default: return t("待处理", "Queued")
        }
    }
}
