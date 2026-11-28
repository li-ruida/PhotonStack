import Foundation
import Testing
import PhotonStackAppCore
import PhotonStackProcessing
@testable import PhotonStackUI

@Test @MainActor func auditWorkflowDirectoryAliasesExecuteEachInputOnce() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditWorkflow-\(UUID())")
    let folder = root.appendingPathComponent("target", isDirectory: true)
    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let input = folder.appendingPathComponent("light.fits")
    try Data("source".utf8).write(to: input)
    let alias = root.appendingPathComponent("alias")
    try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: folder)
    let folders = [folder, URL(fileURLWithPath: folder.path, isDirectory: false), alias]
    let batch = WorkflowBatchRun()
    var executions = 0
    try await batch.run(workflow: ReusableWorkflow(name: "audit", steps: [.init(kind: .curves)]),
                        folders: folders, output: root) { _, inputs, directory, _ in
        executions += 1
        #expect(inputs.map(\.path) == [input.resolvingSymlinksInPath().path])
        let result = directory.appendingPathComponent("result.fits")
        try Data("result".utf8).write(to: result)
        return result
    }
    #expect(executions == 1)
    #expect(batch.items.count == 1)
    #expect(batch.items.first?.status == .succeeded)
    #expect(try String(contentsOf: input, encoding: .utf8) == "source")
}

@Test @MainActor func auditRecipeImportCannotWriteIntoReplacementProject() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditImport-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let executable = root.appendingPathComponent("inspect")
    let script = """
    #!/bin/sh
    touch started
    while [ ! -f release ]; do sleep 0.02; done
    printf '%s\n' '{"version":1,"values":{"stack.method":"maximum"},"inputs":[],"recipe":"PHOTONSTACK_DEEP_SKY_RECIPE 1\\n"}'
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    let service = CLIProcessingService(executableURL: executable, workingDirectoryURL: root)
    let model = PhotonStackWorkspaceModel(autosaveEnabled: false, processingService: service,
        previewDirectory: root.appendingPathComponent("previews"))
    let task = Task { await model.importDeepSkyRecipe(from: root.appendingPathComponent("recipe.txt")) }
    defer { task.cancel(); try? Data().write(to: root.appendingPathComponent("release")) }
    let deadline = ContinuousClock.now.advanced(by: .seconds(10))
    while !FileManager.default.fileExists(atPath: root.appendingPathComponent("started").path), ContinuousClock.now < deadline {
        try await Task.sleep(for: .milliseconds(10))
    }
    #expect(FileManager.default.fileExists(atPath: root.appendingPathComponent("started").path))
    model.createProject(template: .singleFrame, name: "Replacement")
    let replacementID = model.project.id
    try Data().write(to: root.appendingPathComponent("release"))
    await task.value
    #expect(model.project.id == replacementID)
    #expect(model.project.deepSkySettings == nil)
    #expect(model.errorMessage == nil)
}

@Test @MainActor func auditNonImageExportsPreserveProjectSources() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditExport-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let source = root.appendingPathComponent("source.txt")
    try Data("original".utf8).write(to: source)
    var project = PhotonStackProject(); project.addAssets(from: [source])
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false,
        processingService: UnavailableProcessingService(), previewDirectory: root.appendingPathComponent("previews"))
    model.exportCustomCurvePresets(to: source)
    #expect(model.errorMessage != nil)
    model.exportDeepSkyRecipe(to: source)
    #expect(model.errorMessage?.contains("source.txt") == true)
    await model.exportProductManifest(to: source)
    #expect(try String(contentsOf: source, encoding: .utf8) == "original")
}

@Test @MainActor func auditDerivedImageAliasSelectsOwnerWithoutRedundantUndo() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditDerived-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let other = root.appendingPathComponent("other.png"), image = root.appendingPathComponent("image.png"), alias = root.appendingPathComponent("alias.png")
    let png = try #require(Data(base64Encoded: "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="))
    try png.write(to: other); try png.write(to: image)
    try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: image)
    var project = PhotonStackProject(); project.addAssets(from: [other, image])
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false,
        processingService: UnavailableProcessingService(), previewDirectory: root.appendingPathComponent("previews"))
    model.showDerivedImage(alias)
    let deadline = ContinuousClock.now.advanced(by: .seconds(5))
    while model.hasActiveProcessing, ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(10)) }
    #expect(!model.hasActiveProcessing)
    #expect(model.project.assets.count == 2)
    #expect(model.selectedAssetID == project.assets[1].id)
    #expect(!model.canStepBack)
}
