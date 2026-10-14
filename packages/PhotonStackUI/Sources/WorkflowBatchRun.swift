import Foundation
import Combine
import PhotonStackAppCore

public struct WorkflowRunItem: Identifiable, Codable, Sendable {
    public var id = UUID()
    public var folder: URL
    public var inputs: [URL]
    public var status: ProcessingJobStatus = .queued
    public var completedSteps = 0
    public var message = ""
    public var output: URL?
    public var directory: URL?
    public var title: String { inputs.count == 1 ? inputs[0].lastPathComponent : folder.lastPathComponent }

    func directoryName(index: Int) -> String {
        let stem = inputs.count == 1 ? inputs[0].deletingPathExtension().lastPathComponent : folder.lastPathComponent
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_"))
        let safe = String(String.UnicodeScalarView(stem.unicodeScalars.filter { allowed.contains($0) }.prefix(40)))
        return String(format: "%04d", index + 1) + "-" + (safe.isEmpty ? "target" : safe) + "-" + id.uuidString.prefix(8)
    }
}

/// Lifecycle is owned by the workspace's cancellable foreground task, including quit guards.
@MainActor
public final class WorkflowBatchRun: ObservableObject {
    @Published public private(set) var items: [WorkflowRunItem] = []
    @Published public private(set) var isRunning = false
    @Published public private(set) var outputDirectory: URL?
    @Published public private(set) var error: String?
    @Published public private(set) var stepCount = 0
    public init() {}

    public typealias Executor = @MainActor (WorkflowInputPlan, [URL], URL, @MainActor (Int, String) -> Void) async throws -> URL

    public func run(workflow: ReusableWorkflow, folders: [URL], output: URL, execute: Executor) async throws {
        guard !isRunning else { throw WorkflowError("工作流正在运行 / Workflow is already running") }
        try workflow.validate()
        guard !folders.isEmpty, folders.count <= 100, output.isFileURL else { throw WorkflowError("请选择 1–100 个目标文件夹和输出位置 / Choose target folders and output location") }
        let folders = Array(Set(folders.map { $0.resolvingSymlinksInPath().standardizedFileURL })).sorted { $0.path < $1.path }
        let root = output.resolvingSymlinksInPath().appendingPathComponent("PhotonStack-Workflow-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
        outputDirectory = root
        items = []
        error = nil
        stepCount = workflow.steps.count + 1
        isRunning = true
        defer { isRunning = false }
        var plans: [URL: WorkflowInputPlan] = [:]
        do {
            // Persist the exact immutable recipe beside the results for reproducibility.
            try JSONEncoder().encode(workflow).write(to: root.appendingPathComponent("workflow.json"), options: .atomic)
            for folder in folders {
                try Task.checkCancellation()
                do {
                    let plan = try await WorkflowInputPlan.inspectAsync(folder: folder, workflow: workflow)
                    plans[folder] = plan
                    let groups = workflow.combinesFrames ? [plan.lights] : plan.lights.map { [$0] }
                    items += groups.map { WorkflowRunItem(folder: folder, inputs: $0) }
                } catch is CancellationError { throw CancellationError() }
                catch { items.append(WorkflowRunItem(folder: folder, inputs: [], status: .failed, message: error.localizedDescription)) }
            }
            try persist(root: root)
            for index in items.indices where items[index].status == .queued {
                try Task.checkCancellation()
                guard let plan = plans[items[index].folder] else { throw WorkflowError("目标绑定丢失 / Missing target binding") }
                let directory = root.appendingPathComponent(items[index].directoryName(index: index), isDirectory: true)
                items[index].directory = directory
                items[index].status = .running
                try persist(root: root)
                do {
                    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
                    let result = try await execute(plan, items[index].inputs, directory) { step, message in
                        self.items[index].completedSteps = step
                        self.items[index].message = message
                    }
                    try Task.checkCancellation()
                    guard result.resolvingSymlinksInPath().deletingLastPathComponent() == directory.resolvingSymlinksInPath(),
                          FileManager.default.fileExists(atPath: result.path) else { throw WorkflowError("输出文件无效 / Invalid output file") }
                    items[index].output = result
                    items[index].status = .succeeded
                    items[index].completedSteps = workflow.steps.count + 1
                    items[index].message = "完成 / Completed"
                } catch is CancellationError { throw CancellationError() }
                catch {
                    if Task.isCancelled { throw CancellationError() }
                    items[index].status = .failed
                    items[index].message = error.localizedDescription
                }
                try persist(root: root)
            }
        } catch {
            let cancelled = error is CancellationError || Task.isCancelled
            for index in items.indices where items[index].status == .queued || items[index].status == .running {
                items[index].status = cancelled ? .cancelled : .failed
                items[index].message = cancelled ? "已取消 / Cancelled" : error.localizedDescription
            }
            self.error = cancelled ? nil : error.localizedDescription
            try? persist(root: root)
            throw error
        }
    }

    private func persist(root: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(items).write(to: root.appendingPathComponent("run.json"), options: .atomic)
    }
}
