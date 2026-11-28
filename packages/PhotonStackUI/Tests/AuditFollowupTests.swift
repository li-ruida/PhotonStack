import AppKit
import Foundation
import Testing
import PhotonStackAppCore
import PhotonStackProcessing
@testable import PhotonStackUI

@Test @MainActor func auditEditedStateBelongsOnlyToAttachedWorkspaceWindow() {
    _ = NSApplication.shared
    let workspace = NSWindow(contentRect: .init(x: 0, y: 0, width: 200, height: 100), styleMask: [.titled, .closable], backing: .buffered, defer: false)
    let auxiliary = NSWindow(contentRect: .init(x: 0, y: 0, width: 200, height: 100), styleMask: [.titled, .closable], backing: .buffered, defer: false)
    workspace.isReleasedWhenClosed = false; auxiliary.isReleasedWhenClosed = false
    let coordinator = WindowCloseGuard.Coordinator(hasUnsavedChanges: true, hasActiveProcessing: false, onRequestClose: {})
    coordinator.attach(to: workspace)
    #expect(workspace.isDocumentEdited)
    #expect(!auxiliary.isDocumentEdited)
    coordinator.hasUnsavedChanges = false; coordinator.attach(to: workspace)
    #expect(!workspace.isDocumentEdited)
    coordinator.hasUnsavedChanges = true; coordinator.attach(to: workspace)
    workspace.close()
    #expect(!workspace.isDocumentEdited)
    coordinator.detach(); auxiliary.close()
}

@Test @MainActor func auditCalibrationCancellationKeepsSessionLockedUntilWorkEnds() async throws {
    let library = CalibrationLibrary(root: URL(fileURLWithPath: "/tmp/unused-audit-library"))
    let session = CalibrationLibrarySession(library: library, lights: [])
    var gate: CheckedContinuation<Void, Never>?
    var replacementRan = false
    session.perform { await withCheckedContinuation { gate = $0 } }
    let deadline = ContinuousClock.now.advanced(by: .seconds(5))
    while gate == nil && ContinuousClock.now < deadline { await Task.yield() }
    let finish = try #require(gate)
    session.cancel()
    #expect(session.busy)
    session.perform { replacementRan = true }
    #expect(!replacementRan)
    finish.resume()
    while session.busy && ContinuousClock.now < deadline { await Task.yield() }
    #expect(!session.busy && session.error == nil)
}

@Test @MainActor func auditMasterBuildCancellationWaitsForTaskCompletion() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditBuildCancel-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let session = CalibrationMasterBuildSession(library: CalibrationLibrary(root: root.appendingPathComponent("library")), entries: [])
    session.choose([root])
    session.cancel()
    #expect(session.busy)
    session.choose([URL(fileURLWithPath: "/missing")])
    let deadline = ContinuousClock.now.advanced(by: .seconds(5))
    while session.busy && ContinuousClock.now < deadline { await Task.yield() }
    #expect(!session.busy && session.error == nil && session.frames.isEmpty)
}

@Test @MainActor func auditBackgroundRecipeExportProtectsProjectJSON() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditBackgroundJSON-\(UUID())")
    defer { try? FileManager.default.removeItem(at: root) }
    let model = PhotonStackWorkspaceModel(autosaveEnabled: false, processingService: CLIProcessingService(executableURL: URL(fileURLWithPath: "/missing")), previewDirectory: root.appendingPathComponent("preview"))
    await model.saveProject(to: root)
    let projectFile = root.appendingPathComponent("project.json")
    let original = try Data(contentsOf: projectFile)
    #expect(throws: WorkspaceExportProtection.Failure.self) {
        try model.validateExportDestination(projectFile, includesSidecar: false)
    }
    let alias = root.appendingPathComponent("alias.json")
    try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: projectFile)
    #expect(throws: WorkspaceExportProtection.Failure.self) {
        try model.validateExportDestination(alias, includesSidecar: false)
    }
    try model.validateExportDestination(root.appendingPathComponent("background.json"), includesSidecar: false)
    let repository = WorkflowRepository(url: root.appendingPathComponent("workflows.json"))
    let workflow = ReusableWorkflow(name: "Portable", steps: [.init(kind: .stretch)])
    for protectedURL in [projectFile, alias] {
        await #expect(throws: WorkspaceExportProtection.Failure.self) {
            try await model.exportReusableWorkflow(workflow, to: protectedURL, repository: repository)
        }
    }
    let exported = root.appendingPathComponent("portable.json")
    try await model.exportReusableWorkflow(workflow, to: exported, repository: repository)
    #expect(try JSONDecoder().decode(ReusableWorkflow.self, from: Data(contentsOf: exported)) == workflow)
    #expect(try Data(contentsOf: projectFile) == original)
}

@Test @MainActor func auditMosaicFolderAdoptsAlreadyImportedAlias() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditMosaicAlias-\(UUID())")
    let folder = root.appendingPathComponent("panel/lights", isDirectory: true)
    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let image = folder.appendingPathComponent("Light_1.fits")
    try Data().write(to: image)
    let alias = root.appendingPathComponent("alias")
    try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: root.appendingPathComponent("panel"))
    var project = PhotonStackProject(name: "Mosaic", template: .mosaic)
    project.addAssets(from: [alias.appendingPathComponent("lights/Light_1.fits")], role: .mosaic)
    let originalID = project.assets[0].id
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false, processingService: CLIProcessingService(executableURL: URL(fileURLWithPath: "/missing")), previewDirectory: root.appendingPathComponent("preview"))
    model.importAssets(from: [root.appendingPathComponent("panel")])
    #expect(model.project.assets.count == 1)
    #expect(model.mosaicAssets.count == 1)
    #expect(model.mosaicAssets.first?.id == originalID)
    #expect(model.mosaicAssets.first?.mosaicFolder?.frames.map(PhotonStackProject.assetIdentityPath(for:)) == [PhotonStackProject.assetIdentityPath(for: image)])
}

@Test @MainActor func auditCancelledDerivedImageDoesNotChangeWorkspaceOrShowError() async throws {
    let model = PhotonStackWorkspaceModel(autosaveEnabled: false,
        processingService: CLIProcessingService(executableURL: URL(fileURLWithPath: "/missing")),
        previewDirectory: FileManager.default.temporaryDirectory.appendingPathComponent("AuditDerived-\(UUID())"))
    model.showDerivedImage(URL(fileURLWithPath: "/tmp/cancelled-result.fits"))
    model.cancelCurrentTask()
    let deadline = ContinuousClock.now.advanced(by: .seconds(5))
    while model.hasActiveProcessing && ContinuousClock.now < deadline { await Task.yield() }
    #expect(!model.hasActiveProcessing && model.errorMessage == nil)
    #expect(model.project.assets.isEmpty)
    #expect(model.jobs.last?.status == .cancelled)
}

@Test @MainActor func auditGHSOpeningResultRespondsToOperationCancellation() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditGHSOpen-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let executable = root.appendingPathComponent("tool")
    try "#!/bin/sh\ntouch started\nexec sleep 30\n".write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    let session = GeneralizedStretchSession(source: root.appendingPathComponent("input.fits"),
        service: CLIProcessingService(executableURL: executable, workingDirectoryURL: root))
    let operation = Task { try await session.open(root) }
    defer { operation.cancel(); session.close() }
    let deadline = ContinuousClock.now.advanced(by: .seconds(5))
    while !FileManager.default.fileExists(atPath: root.appendingPathComponent("started").path) && ContinuousClock.now < deadline {
        try await Task.sleep(for: .milliseconds(10))
    }
    #expect(session.busy)
    operation.cancel(); session.cancel()
    do { try await operation.value; Issue.record("Cancelled open completed") }
    catch is CancellationError { }
    #expect(!session.busy && session.report == nil && session.error == nil)
}
