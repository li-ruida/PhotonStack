import Foundation

/// App windows share this actor; each mutation reloads before its atomic write.
public actor WorkflowRepository {
    public static let shared = WorkflowRepository()
    public static let maximumBytes: UInt64 = 8 * 1_024 * 1_024
    public let url: URL
    private struct Library: Codable { var version = 1; var workflows: [ReusableWorkflow] }

    public init(url: URL? = nil) {
        self.url = url ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("PhotonStack/workflows.json")
    }

    public func load() throws -> [ReusableWorkflow] {
        guard FileManager.default.fileExists(atPath: url.path) else { return [] }
        let data = try BoundedDocumentIO.read(from: url, maximumBytes: Self.maximumBytes, fileManager: .default)
        let library = try JSONDecoder().decode(Library.self, from: data)
        guard library.version == 1, library.workflows.count <= 500,
              Set(library.workflows.map(\.id)).count == library.workflows.count else {
            throw WorkflowError("工作流库格式无效 / Invalid workflow library")
        }
        for workflow in library.workflows { try workflow.validate() }
        return library.workflows
    }

    @discardableResult public func save(_ workflow: ReusableWorkflow) throws -> [ReusableWorkflow] {
        try workflow.validate()
        var workflows = try load()
        workflows.removeAll { $0.id == workflow.id }
        workflows.append(workflow)
        try write(workflows)
        return workflows
    }

    @discardableResult public func remove(id: UUID) throws -> [ReusableWorkflow] {
        let workflows = try load().filter { $0.id != id }
        try write(workflows)
        return workflows
    }

    public func importWorkflow(from source: URL) throws -> ReusableWorkflow {
        let data = try BoundedDocumentIO.read(from: source, maximumBytes: Self.maximumBytes, fileManager: .default)
        var workflow = try JSONDecoder().decode(ReusableWorkflow.self, from: data)
        try workflow.validate()
        // Import is always a copy, never an implicit replacement.
        workflow.id = UUID()
        _ = try save(workflow)
        return workflow
    }

    public func exportWorkflow(_ workflow: ReusableWorkflow, to destination: URL) throws {
        try workflow.validate()
        guard destination.resolvingSymlinksInPath().standardizedFileURL != url.resolvingSymlinksInPath().standardizedFileURL else {
            throw WorkflowError("不能用单个配方覆盖工作流库 / Cannot overwrite the workflow library with a single recipe")
        }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(workflow)
        try BoundedDocumentIO.requireFits(data, maximumBytes: Self.maximumBytes)
        try data.write(to: destination, options: .atomic)
    }

    private func write(_ workflows: [ReusableWorkflow]) throws {
        guard workflows.count <= 500 else { throw WorkflowError("工作流库最多保存 500 项 / Workflow library is full") }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(Library(workflows: workflows))
        try BoundedDocumentIO.requireFits(data, maximumBytes: Self.maximumBytes)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: url, options: .atomic)
    }
}
