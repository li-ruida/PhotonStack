import Darwin
import Foundation
import PhotonStackAppCore
import PhotonStackProcessing

/// Immutable calibration identity and settings, with a durable per-stack journal.
/// Interrupted attempts are retained; retry always writes to a new directory.
struct CalibrationBatch: Codable, Sendable {
    struct Item: Codable, Identifiable, Sendable {
        var id: Int
        var groups: [Int]
        var title: String
        var count: Int
        var status = "queued"
        var message = ""
        var attempt: String?
        var products: [String: String] = [:]
    }
    var schema = 1
    var directory, calibrationDirectory, jobSHA: String
    var settings: DeepSkySettings
    var backend: DeepSkyComputeBackend
    var calibrated = false
    var items: [Item] = []
    static func failureMessage(_ error: Error) -> String {
        struct Failure: Decodable { var message: String; var errorCode: String? }
        if case let ProcessingServiceError.commandFailed(_, text) = error, text.utf8.count <= 4 * 1024 * 1024,
           let data = text.split(separator: "\n").last.map({ Data($0.utf8) }),
           let report = try? JSONDecoder().decode(Failure.self, from: data), !report.message.isEmpty {
            return String(([report.errorCode, report.message].compactMap { $0 }.joined(separator: ": ")).prefix(2000))
        }
        return String(error.localizedDescription.prefix(2000))
    }
    var complete: Bool { calibrated && !items.isEmpty && items.allSatisfy { $0.status == "complete" } }
    var isValid: Bool {
        schema == 1 && directory.hasPrefix("/") && calibrationDirectory.hasPrefix("/") &&
        SpectralCalibrationReport.digest(jobSHA) && settings.lastReport == nil && items.count <= 4096 &&
        Set(items.map(\.id)).count == items.count && items.enumerated().allSatisfy { i, item in
            item.id == i && (1...4096).contains(item.count) && !item.groups.isEmpty && item.groups.count <= 4096 &&
            item.groups.allSatisfy { (0..<4096).contains($0) } && Set(item.groups).count == item.groups.count &&
            ["queued", "running", "failed", "complete"].contains(item.status) &&
            item.message.utf8.count <= 8192 && item.title.utf8.count <= 2048 &&
            (item.attempt.map { UUID(uuidString: $0) != nil } ?? true) && item.products.count <= 32 &&
            item.products.allSatisfy { path, sha in
                !path.hasPrefix("/") && path.split(separator: "/", omittingEmptySubsequences: false).allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." } && SpectralCalibrationReport.digest(sha)
            } && (item.status != "complete" || (item.attempt != nil && item.products["project/project.json"] != nil && item.products["result/app-report.json"] != nil))
        }
    }
}

private final class CalibrationBatchLock {
    let fd: Int32
    init(_ directory: URL) throws {
        fd = open(directory.appendingPathComponent(".batch.lock").path, O_CREAT | O_RDWR | O_NOFOLLOW, 0o600)
        guard fd >= 0 else { throw CalibrationAcquisition.failure("Cannot open batch lock.") }
        var metadata = stat()
        guard fstat(fd, &metadata) == 0, metadata.st_mode & S_IFMT == S_IFREG,
              flock(fd, LOCK_EX | LOCK_NB) == 0 else { close(fd); throw CalibrationAcquisition.failure("Batch is already running or its lock is invalid.") }
    }
    deinit { flock(fd, LOCK_UN); close(fd) }
}

extension CalibrationLibrary {
    func createBatch(calibration: URL, expected: CalibrationJobReport, destination: URL,
                     settings: DeepSkySettings = .init(), backend: DeepSkyComputeBackend = .automatic) async throws -> CalibrationBatch {
        let report = try await inspectCalibration(calibration)
        guard report.jobSHA == expected.jobSHA else { throw fail("Calibration task changed.") }
        let target = URL(fileURLWithPath: try CalibrationAcquisition.canonicalPath(destination.deletingLastPathComponent()), isDirectory: true).appendingPathComponent(destination.lastPathComponent)
        guard !fm.fileExists(atPath: target.path) else { throw fail("Choose a new batch folder.") }
        var settings = settings; settings.lastReport = nil
        for key in ["calibration.darks", "calibration.biases", "calibration.flats"] { settings.values[key] = "" }
        settings.values["calibration.sensor-pattern"] = "off"; settings.values["decode.debayer"] = "on"
        _ = try settings.recipe(schema: await service.deepSkySchema(), inputs: [])
        let batch = CalibrationBatch(directory: target.path, calibrationDirectory: try CalibrationAcquisition.canonicalPath(calibration),
                                     jobSHA: report.jobSHA, settings: settings, backend: backend)
        let stage = target.deletingLastPathComponent().appendingPathComponent(".calibration-batch-" + UUID().uuidString)
        try fm.createDirectory(at: stage, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: stage) }
        try persistBatch(batch, at: stage)
        try publish(stage, to: target)
        return batch
    }
    func inspectBatch(_ root: URL) async throws -> CalibrationBatch {
        try directory(root)
        let file = root.appendingPathComponent("batch.json"); try regular(file)
        guard (try file.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? Int.max) <= 16 * 1024 * 1024 else { throw fail("Batch journal is too large.") }
        let data = try Data(contentsOf: file)
        guard data.count <= 16 * 1024 * 1024 else { throw fail("Batch journal grew during read.") }
        let batch = try JSONDecoder().decode(CalibrationBatch.self, from: data)
        guard batch.isValid, batch.directory == (try CalibrationAcquisition.canonicalPath(root)) else { throw fail("Invalid or relocated batch journal.") }
        for item in batch.items where item.status == "complete" {
            try verifyBatchProducts(item, root: root)
            let project = try await ProjectRepository().load(from: root.appendingPathComponent(item.attempt! + "/project"))
            try verifyStackProject(project)
        }
        return batch
    }
    private func persistBatch(_ batch: CalibrationBatch, at root: URL) throws {
        guard batch.isValid else { throw fail("Invalid batch state.") }
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(batch)
        guard data.count <= 16 * 1024 * 1024 else { throw fail("Batch journal is too large.") }
        try data.write(to: root.appendingPathComponent("batch.json"), options: .atomic)
    }
    private func verifyBatchProducts(_ item: CalibrationBatch.Item, root: URL) throws {
        guard let attempt = item.attempt else { throw fail("Missing batch attempt.") }
        let folder = root.appendingPathComponent(attempt); try directory(folder)
        for (name, sha) in item.products {
            let file = folder.appendingPathComponent(name); try regular(file)
            guard file.resolvingSymlinksInPath().path.hasPrefix(folder.resolvingSymlinksInPath().path + "/"),
                  try Self.fileDigest(file) == sha else { throw fail("Completed batch product changed: \(name)") }
        }
    }
    static func batchItems(_ plan: CalibrationStackPlan) -> [CalibrationBatch.Item] {
        var buckets: [[CalibrationStackPlan.Group]] = []
        for group in plan.groups {
            if let i = buckets.firstIndex(where: { CalibrationStackPlan.compatible($0[0].acquisition, group.acquisition) }) { buckets[i].append(group) }
            else { buckets.append([group]) }
        }
        return buckets.enumerated().map { i, groups in
            let a = groups[0].acquisition
            return .init(id: i, groups: groups.map(\.id), title: (a.text["object"] ?? "?") + " · " + (a.text["filter"] ?? "?"), count: groups.reduce(0) { $0 + $1.frames.count })
        }
    }
    func runBatch(_ root: URL, update: @Sendable (CalibrationBatch) async -> Void = { _ in }) async throws -> CalibrationBatch {
        try directory(root)
        let lock = try CalibrationBatchLock(root); defer { withExtendedLifetime(lock) {} }
        var batch = try await inspectBatch(root)
        if batch.complete { await update(batch); return batch }
        let source = URL(fileURLWithPath: batch.calibrationDirectory)
        var report = try await inspectCalibration(source)
        guard report.jobSHA == batch.jobSHA else { throw fail("Calibration task identity changed.") }
        if report.status != "complete" { report = try await resumeCalibration(source, expected: report) }
        guard report.status == "complete" else { throw fail("Calibration must finish before stacking.") }
        let plan = try await stackPlan(directory: source, expected: report)
        let expected = Self.batchItems(plan)
        if batch.calibrated {
            guard batch.items.count == expected.count, zip(batch.items, expected).allSatisfy({ $0.groups == $1.groups && $0.count == $1.count }) else { throw fail("Batch grouping changed.") }
        } else { batch.items = expected; batch.calibrated = true }
        try persistBatch(batch, at: root); await update(batch)
        let schema = try await service.deepSkySchema()
        for i in batch.items.indices where batch.items[i].status != "complete" {
            try Task.checkCancellation()
            let attempt = UUID().uuidString
            batch.items[i].attempt = attempt; batch.items[i].products = [:]
            batch.items[i].status = "running"; batch.items[i].message = ""
            try persistBatch(batch, at: root); await update(batch)
            do {
                let folder = root.appendingPathComponent(attempt)
                try fm.createDirectory(at: folder, withIntermediateDirectories: false)
                let projectURL = try await createStackProject(plan, groups: Set(batch.items[i].groups), name: batch.items[i].title, destination: folder.appendingPathComponent("project"))
                var project = try await ProjectRepository().load(from: projectURL)
                try verifyStackProject(project)
                let initialSHA = try Self.fileDigest(projectURL.appendingPathComponent("project.json"))
                let inputs = project.assets.filter { $0.role == .light }.map(\.originalURL)
                let recipe = try batch.settings.recipe(schema: schema, inputs: inputs)
                let recipeURL = folder.appendingPathComponent("recipe.txt")
                try recipe.write(to: recipeURL, atomically: true, encoding: .utf8)
                let result = folder.appendingPathComponent("result")
                let report = try await service.deepSky(recipe: recipeURL, outputDirectory: result, analyzeOnly: false, computeBackend: batch.backend)
                try Task.checkCancellation(); try verifyStackProject(project)
                guard try Self.fileDigest(projectURL.appendingPathComponent("project.json")) == initialSHA else { throw fail("Project was edited during the batch; it has been preserved.") }
                var settings = batch.settings; settings.lastReport = report; project.deepSkySettings = settings
                let masters = ([report.master, report.backgroundMaster, report.rejectionLow, report.rejectionHigh] + report.noiseReferencePaths + report.drizzlePaths).filter { !$0.isEmpty }.map { URL(fileURLWithPath: $0) }
                project.appendArtifact(.init(kind: .stackMaster, name: "Deep Sky Linear Masters", operationKind: .stack,
                    outputDirectory: result, outputURLs: masters, parameters: ["recipe": report.recipe], metrics: ["frames": String(report.selectedCount)]))
                var parameters = ["mode": "deepSkyRecipe", "recipe": report.recipe, "report": result.appendingPathComponent("app-report.json").path,
                    "output": report.developed, "calibrationLightIDs": project.assets.map(\.id.uuidString).joined(separator: ","),
                    "calibrationDarkIDs": "", "calibrationBiasIDs": "", "calibrationFlatIDs": ""]
                for (index, frame) in report.frames.enumerated() { parameters["inputPath.\(index)"] = frame.input }
                let operation = EditOperation(kind: .stack, parameters: parameters); project.appendOperation(operation)
                project.workspaceState = .init(previewURL: URL(fileURLWithPath: report.developed), previewOperationID: operation.id)
                try await ProjectRepository().save(project, to: projectURL)
                var products: [String: String] = [:]
                for file in masters + [URL(fileURLWithPath: report.developed), result.appendingPathComponent("app-report.json"), projectURL.appendingPathComponent("project.json"), recipeURL] {
                    try regular(file)
                    let base = folder.resolvingSymlinksInPath().path + "/", path = file.resolvingSymlinksInPath().path
                    guard path.hasPrefix(base) else { throw fail("Batch result is outside its attempt folder.") }
                    products[String(path.dropFirst(base.count))] = try Self.fileDigest(file)
                }
                batch.items[i].products = products; batch.items[i].status = "complete"
            } catch {
                batch.items[i].status = "failed"
                batch.items[i].message = CalibrationBatch.failureMessage(error)
                try persistBatch(batch, at: root); await update(batch)
                if Task.isCancelled || error is CancellationError { throw CancellationError() }
                continue
            }
            try persistBatch(batch, at: root); await update(batch)
        }
        return batch
    }
}
