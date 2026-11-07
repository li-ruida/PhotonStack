import Foundation
import PhotonStackAppCore
import PhotonStackProcessing
import Testing
@testable import PhotonStackUI

@Test @MainActor func storageCleanupProtectsImportedOriginalsAndReportProvenance() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackStorage-\(UUID().uuidString)")
    let previews = root.appendingPathComponent("previews")
    let workflow = previews.appendingPathComponent("workflow-current")
    try FileManager.default.createDirectory(at: workflow, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let selectedOriginal = root.appendingPathComponent("selected.fits")
    let importedOriginal = previews.appendingPathComponent("cache-imported-original.fits")
    let report = workflow.appendingPathComponent("report.json")
    let master = workflow.appendingPathComponent("linear.fits")
    let orphan = previews.appendingPathComponent("cache-orphan.tiff")
    let manual = previews.appendingPathComponent("manual-reference.txt")
    let retained = [(importedOriginal, Data(repeating: 1, count: 8)), (report, Data(repeating: 2, count: 4)),
                    (master, Data(repeating: 3, count: 5)), (manual, Data(repeating: 4, count: 4))]
    try Data("selected".utf8).write(to: selectedOriginal)
    for (url, bytes) in retained { try bytes.write(to: url) }
    try Data(repeating: 0, count: 7).write(to: orphan)
    var project = PhotonStackProject(name: "Storage reference protection")
    project.addAssets(from: [selectedOriginal, importedOriginal])
    project.editGraph = EditGraph(operations: [EditOperation(kind: .stack,
        parameters: ["report": report.path, "sourceMaster": master.path])])
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false,
        processingService: UnavailableProcessingService(), previewDirectory: previews)
    let snapshot = try await model.storageSnapshot()
    #expect(snapshot.managedFileCount == 5 && snapshot.managedBytes == 28)
    #expect(snapshot.protectedFileCount == 4 && snapshot.protectedBytes == 21)
    #expect(snapshot.reclaimableFileCount == 1 && snapshot.reclaimableBytes == 7)
    let plan = try #require(model.makePreviewCacheCleanupPlan())
    #expect(plan.fileCount == snapshot.reclaimableFileCount && plan.totalBytes == snapshot.reclaimableBytes)
    model.clearPreviewCache(plan)
    #expect(!FileManager.default.fileExists(atPath: orphan.path))
    for (url, bytes) in retained { #expect(try Data(contentsOf: url) == bytes) }
    #expect(try Data(contentsOf: selectedOriginal) == Data("selected".utf8))
    let after = try await model.storageSnapshot()
    #expect(after.managedBytes == 21 && after.reclaimableBytes == 0)
}
