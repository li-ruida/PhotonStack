import Foundation
import ImageIO
import PhotonStackAppCore
import PhotonStackProcessing
import Testing
@testable import PhotonStackUI

private func workflowTestRoot() throws -> URL {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("ReusableWorkflowTests-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    return root
}

private func workflowTestFrames(in folder: URL, count: Int = 3, sky: Double = 100, starAmplitude: Double = 30) throws -> [URL] {
    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    return try (0..<count).map { frame in
        let url = folder.appendingPathComponent("light-\(frame).fits")
        func card(_ key: String, _ value: String) -> String {
            (key.padding(toLength: 8, withPad: " ", startingAt: 0) + "= " + value).padding(toLength: 80, withPad: " ", startingAt: 0)
        }
        let header = (card("SIMPLE", "T") + card("BITPIX", "-32") + card("NAXIS", "2") + card("NAXIS1", "64") + card("NAXIS2", "64") + "END".padding(toLength: 80, withPad: " ", startingAt: 0)).padding(toLength: 2880, withPad: " ", startingAt: 0)
        var data = Data(header.utf8)
        for y in 0..<64 {
            for x in 0..<64 {
                let r2 = Double((x - 32) * (x - 32) + (y - 32) * (y - 32))
                let value = Float(sky + Double(frame * 2) + starAmplitude * exp(-r2 / 80) + Double((x * 13 + y * 17 + frame * 7) % 19) * 0.1)
                var big = value.bitPattern.bigEndian
                withUnsafeBytes(of: &big) { data.append(contentsOf: $0) }
            }
        }
        data.append(Data(repeating: 0, count: (2880 - data.count % 2880) % 2880))
        try data.write(to: url)
        return url
    }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil), arguments: [false, true]) @MainActor
func reusableWorkflowLegacyStackSurvivesSourceRemovalAndRebindsCalibration(calibrated: Bool) async throws {
    let root = try workflowTestRoot()
    defer { try? FileManager.default.removeItem(at: root) }
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let source = root.appendingPathComponent("source")
    let sourceCache = root.appendingPathComponent("source-cache")
    var project = PhotonStackProject(name: "original")
    project.addAssets(from: try workflowTestFrames(in: source.appendingPathComponent("lights")))
    if calibrated {
        project.addAssets(from: try workflowTestFrames(in: source.appendingPathComponent("darks"), sky: 1, starAmplitude: 0), role: .dark)
    }
    let parameters = ProcessingParameters(workflowStackMethod: .maximum, workflowAlignment: .none)
    let sourceModel = PhotonStackWorkspaceModel(project: project, parameters: parameters, autosaveEnabled: false,
        exportOptions: .init(bitDepth: .sixteen), processingService: service, previewDirectory: sourceCache)
    await sourceModel.runStackWorkflow()
    try #require(sourceModel.errorMessage == nil, Comment(rawValue: sourceModel.errorMessage ?? ""))
    let captured = try await sourceModel.captureReusableWorkflow(name: "Legacy full finishing")
    #expect(captured.steps.first?.kind == .stack)
    #expect(captured.steps.last?.kind == .crop)
    #expect(captured.steps.contains { $0.kind == .curves })
    #expect(captured.requiredCalibration == (calibrated ? [.darks] : []))
    let repositoryURL = root.appendingPathComponent("library.json")
    _ = try await WorkflowRepository(url: repositoryURL).save(captured)
    let loaded = try #require(try await WorkflowRepository(url: repositoryURL).load().first)
    // This proves the saved workflow has no hidden dependency on the source/cache.
    try FileManager.default.removeItem(at: source)
    try FileManager.default.removeItem(at: sourceCache)

    let target = root.appendingPathComponent("new target")
    let targetLights = try workflowTestFrames(in: target.appendingPathComponent("lights"), sky: 120)
    var targetProject = PhotonStackProject(name: "new target")
    targetProject.addAssets(from: targetLights)
    if calibrated {
        let darks = try workflowTestFrames(in: target.appendingPathComponent("darks"), sky: 5, starAmplitude: 0)
        targetProject.addAssets(from: darks, role: .dark)
    }
    let allInputs = targetProject.assets.map(\.originalURL)
    let inputBytes = try allInputs.map { try Data(contentsOf: $0) }
    let direct = PhotonStackWorkspaceModel(project: targetProject, parameters: parameters, autosaveEnabled: false,
        exportOptions: loaded.exportOptions, processingService: service, previewDirectory: root.appendingPathComponent("direct"))
    await direct.runStackWorkflow()
    try #require(direct.errorMessage == nil, Comment(rawValue: direct.errorMessage ?? ""))
    let expected = root.appendingPathComponent("expected.tiff")
    await direct.exportCurrentImage(to: expected)
    try #require(direct.errorMessage == nil, Comment(rawValue: direct.errorMessage ?? ""))

    let batch = WorkflowBatchRun()
    try await batch.run(workflow: loaded, folders: [target], output: root) { plan, inputs, directory, onStep in
        try await sourceModel.executeReusableWorkflow(loaded, plan: plan, inputs: inputs, directory: directory, onStep: onStep)
    }
    let item = try #require(batch.items.first)
    try #require(item.status == .succeeded, Comment(rawValue: item.message))
    let output = try #require(item.output)
    #expect(try workflowPixels(output) == workflowPixels(expected))
    #expect(try allInputs.map { try Data(contentsOf: $0) } == inputBytes)
}

private func workflowPixels(_ url: URL) throws -> Data {
    let source = try #require(CGImageSourceCreateWithURL(url as CFURL, nil))
    let image = try #require(CGImageSourceCreateImageAtIndex(source, 0, nil))
    return try #require(image.dataProvider?.data) as Data
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func reusableWorkflowCapturesOnlyActiveBranchAndHonorsUndo() async throws {
    let root = try workflowTestRoot()
    defer { try? FileManager.default.removeItem(at: root) }
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    var project = PhotonStackProject(name: "branches")
    project.addAssets(from: try workflowTestFrames(in: root.appendingPathComponent("inputs")))
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false, processingService: service,
                                        previewDirectory: root.appendingPathComponent("cache"))
    model.select(project.assets[0])
    await model.normalizePreview(targetBackground: 0.2, targetScale: 1)
    try #require(model.errorMessage == nil)
    model.select(project.assets[1])
    await model.autoStretchPreview()
    await model.colorSaturatePreview(amount: 0.1)
    try #require(model.errorMessage == nil)
    let current = try await model.captureReusableWorkflow(name: "active")
    #expect(current.steps.map(\.kind) == [.stretch, .colorSaturate])
    model.stepBackPreview()
    let undone = try await model.captureReusableWorkflow(name: "undone")
    #expect(undone.steps.map(\.kind) == [.stretch])
    let operation = try #require(model.project.editGraph.operations.last(where: { $0.kind == .stretch }))
    model.toggleEditOperation(operation.id)
    await #expect(throws: WorkflowError.self) { try await model.captureReusableWorkflow(name: "pending") }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func reusableWorkflowDeepSkyCalibrationUsesNewFolderPathsAndMinimumFrames() async throws {
    let root = try workflowTestRoot()
    defer { try? FileManager.default.removeItem(at: root) }
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let target = root.appendingPathComponent("目标 \"two\"")
    let lights = try workflowTestFrames(in: target.appendingPathComponent("lights"), count: 7)
    let darks = try workflowTestFrames(in: target.appendingPathComponent("darks"), sky: 2, starAmplitude: 0)
    let schema = try await service.deepSkySchema()
    var settings = DeepSkySettings()
    settings.values = ["registration.mode": "none", "selection.measure-stars": "off", "background.model": "none",
                       "develop.stellar-balance": "off", "develop.tone-scale": "10", "develop.white-point": "200",
                       "calibration.darks": "\"/old/camera/dark.fits\"", "selection.overrides": "reject,keep",
                       "registration.reference": "15", "stack.weights": "0.1,0.9"]
    let capture = try WorkflowStep.deepSky(values: settings.values)
    let workflow = ReusableWorkflow(name: "calibration", steps: [capture.step])
    let plan = try WorkflowInputPlan.inspect(folder: target, workflow: workflow)
    let recipe = try plan.deepSkyRecipe(for: capture.step, schema: schema)
    let recipeURL = root.appendingPathComponent("rebound.txt")
    try recipe.write(to: recipeURL, atomically: true, encoding: .utf8)
    let inspected = try await service.inspectDeepSkyRecipe(recipeURL)
    #expect(inspected.inputs == lights.map(\.path))
    #expect(!inspected.recipe.contains("/old/camera"))
    #expect(inspected.values["registration.reference"] == "-1")
    #expect(inspected.values["selection.overrides"] == Array(repeating: "auto", count: lights.count).joined(separator: ","))
    #expect(inspected.values["stack.weights"] == "")
    let report = try await service.deepSky(recipe: recipeURL, outputDirectory: root.appendingPathComponent("run"), analyzeOnly: false)
    #expect(report.success)
    #expect(report.inputCount == lights.count)
    #expect(darks.allSatisfy { FileManager.default.fileExists(atPath: $0.path) })
    let tooSmall = root.appendingPathComponent("two-frames")
    _ = try workflowTestFrames(in: tooSmall.appendingPathComponent("lights"), count: 2)
    #expect(throws: WorkflowError.self) { try WorkflowInputPlan.inspect(folder: tooSmall, workflow: workflow) }
}

@Test @MainActor func reusableWorkflowBatchIsolatesFailuresAndNeverOverwritesResults() async throws {
    let root = try workflowTestRoot()
    defer { try? FileManager.default.removeItem(at: root) }
    let folder = root.appendingPathComponent("target")
    let inputs = try workflowTestFrames(in: folder)
    let originals = try inputs.map { try Data(contentsOf: $0) }
    let workflow = ReusableWorkflow(name: "per-image", steps: [.init(kind: .stretch)])
    let batch = WorkflowBatchRun()
    let execute: WorkflowBatchRun.Executor = { _, inputs, directory, progress in
        if inputs[0].lastPathComponent == "light-1.fits" { throw WorkflowError("test failure") }
        progress(1, "export")
        let output = directory.appendingPathComponent("result.tiff")
        try Data([1, 2, 3]).write(to: output)
        return output
    }
    try await batch.run(workflow: workflow, folders: [folder, root.appendingPathComponent("missing")], output: root, execute: execute)
    #expect(batch.items.filter { $0.status == .succeeded }.count == 2)
    #expect(batch.items.filter { $0.status == .failed }.count == 2)
    #expect(batch.items.filter { $0.status == .succeeded }.allSatisfy { $0.directory?.lastPathComponent.contains("light-") == true })
    let firstOutput = try #require(batch.outputDirectory)
    let firstManifest = try Data(contentsOf: firstOutput.appendingPathComponent("run.json"))
    try await batch.run(workflow: workflow, folders: [folder], output: root, execute: execute)
    #expect(batch.outputDirectory != firstOutput)
    #expect(try Data(contentsOf: firstOutput.appendingPathComponent("run.json")) == firstManifest)
    #expect(try inputs.map { try Data(contentsOf: $0) } == originals)
}

@Test func reusableWorkflowResultDirectoryNamesStaySafeAndRecognizable() {
    let item = WorkflowRunItem(folder: URL(fileURLWithPath: "/targets/M31"), inputs: [URL(fileURLWithPath: "/targets/M31/照片 001.fits")])
    let name = item.directoryName(index: 0)
    #expect(name.hasPrefix("0001-照片001-"))
    #expect(!name.contains("/") && !name.contains(".."))
    let long = WorkflowRunItem(folder: URL(fileURLWithPath: "/" + String(repeating: "星", count: 200)), inputs: [])
    #expect(long.directoryName(index: 1).utf8.count < 255)
}

@Test @MainActor func reusableWorkflowCancellationKeepsCompletedResults() async throws {
    let root = try workflowTestRoot()
    defer { try? FileManager.default.removeItem(at: root) }
    let folder = root.appendingPathComponent("target")
    _ = try workflowTestFrames(in: folder)
    let batch = WorkflowBatchRun()
    let workflow = ReusableWorkflow(name: "cancel", steps: [.init(kind: .stretch)])
    await #expect(throws: CancellationError.self) {
        try await batch.run(workflow: workflow, folders: [folder], output: root) { _, inputs, directory, _ in
            if inputs[0].lastPathComponent == "light-1.fits" { throw CancellationError() }
            let result = directory.appendingPathComponent("result.tiff")
            try Data([1]).write(to: result)
            return result
        }
    }
    #expect(!batch.isRunning)
    #expect(batch.items.map(\.status) == [.succeeded, .cancelled, .cancelled])
    #expect(FileManager.default.fileExists(atPath: try #require(batch.items.first?.output).path))
}

@Test @MainActor func reusableWorkflowTaskCancellationStopsPendingJobs() async throws {
    let root = try workflowTestRoot()
    defer { try? FileManager.default.removeItem(at: root) }
    let folder = root.appendingPathComponent("target")
    _ = try workflowTestFrames(in: folder)
    let batch = WorkflowBatchRun()
    let workflow = ReusableWorkflow(name: "cancel task", steps: [.init(kind: .stretch)])
    var secondStarted = false
    let task = Task { @MainActor in
        try await batch.run(workflow: workflow, folders: [folder], output: root) { _, inputs, directory, _ in
            if inputs[0].lastPathComponent == "light-1.fits" {
                secondStarted = true
                try await Task.sleep(for: .seconds(10))
            }
            let result = directory.appendingPathComponent("result.tiff")
            try Data([1]).write(to: result)
            return result
        }
    }
    defer { task.cancel() }
    for _ in 0..<1_000 {
        if secondStarted { break }
        try await Task.sleep(for: .milliseconds(1))
    }
    try #require(secondStarted)
    task.cancel()
    await #expect(throws: CancellationError.self) { try await task.value }
    #expect(!batch.isRunning)
    #expect(batch.items.map(\.status) == [.succeeded, .cancelled, .cancelled])
    let manifest = try #require(batch.outputDirectory).appendingPathComponent("run.json")
    let saved = try JSONDecoder().decode([WorkflowRunItem].self, from: Data(contentsOf: manifest))
    #expect(saved.map(\.status) == batch.items.map(\.status))
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func reusableWorkflowCapturesStackAndFinishingAndRunsNewTargetsAtFullResolution() async throws {
    let root = try workflowTestRoot()
    defer { try? FileManager.default.removeItem(at: root) }
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let source = root.appendingPathComponent("source")
    let sourceInputs = try workflowTestFrames(in: source, count: 7)
    var project = PhotonStackProject(name: "source")
    project.addAssets(from: sourceInputs)
    var settings = DeepSkySettings()
    settings.values = ["registration.mode": "none", "selection.measure-stars": "off", "background.model": "none", "develop.stellar-balance": "off", "develop.tone-scale": "10", "develop.white-point": "200", "stack.method": "maximum"]
    settings.setCompatibleValue("maximum", for: "stack.method", schema: try await service.deepSkySchema())
    project.deepSkySettings = settings
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false, processingService: service,
                                        previewDirectory: root.appendingPathComponent("source-cache"))
    await model.runDeepSkyWorkflow(analyzeOnly: false)
    #expect(model.errorMessage == nil)
    await model.colorSaturatePreview(amount: 0.15)
    #expect(model.errorMessage == nil)
    let workflow = try await model.captureReusableWorkflow(name: "Maximum + saturation")
    #expect(workflow.steps.map(\.kind) == [.stack, .colorSaturate])
    #expect(workflow.steps.first?.deepSkyValues?["stack.method"] == "maximum")
    #expect(workflow.steps.last?.parameters["amount"] == "0.15")
    #expect(!String(decoding: try JSONEncoder().encode(workflow), as: UTF8.self).contains(root.path))
    let sourceProject = model.project
    let sourcePreview = model.previewURL
    let targetA = root.appendingPathComponent("target-a")
    let targetB = root.appendingPathComponent("target-b")
    let targets = try workflowTestFrames(in: targetA, count: 7) + workflowTestFrames(in: targetB, count: 7)
    let before = try targets.map { try Data(contentsOf: $0) }
    let batch = WorkflowBatchRun()
    try await batch.run(workflow: workflow, folders: [targetA, targetB], output: root) { plan, inputs, directory, progress in
        try await model.executeReusableWorkflow(workflow, plan: plan, inputs: inputs, directory: directory, onStep: progress)
    }
    #expect(batch.items.map(\.status) == [.succeeded, .succeeded])
    for item in batch.items {
        let output = try #require(item.output, Comment(rawValue: item.message))
        let imageSource = try #require(CGImageSourceCreateWithURL(output as CFURL, nil))
        let image = try #require(CGImageSourceCreateImageAtIndex(imageSource, 0, nil))
        #expect(image.width == 64 && image.height == 64)
    }
    #expect(model.project == sourceProject)
    #expect(model.previewURL == sourcePreview)
    #expect(try targets.map { try Data(contentsOf: $0) } == before)
    // Fresh targets contain identical fixtures, so the replay must have equal pixels.
    let a = try #require(batch.items[0].output)
    let b = try #require(batch.items[1].output)
    #expect(try workflowPixels(a) == workflowPixels(b))
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func reusableWorkflowPostOnlyUsesEachNewImageAndRejectsFlatCapture() async throws {
    let root = try workflowTestRoot()
    defer { try? FileManager.default.removeItem(at: root) }
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let model = PhotonStackWorkspaceModel(autosaveEnabled: false, processingService: service, previewDirectory: root.appendingPathComponent("cache"))
    await #expect(throws: WorkflowError.self) { try await model.captureReusableWorkflow(name: "no history") }
    let folder = root.appendingPathComponent("target")
    _ = try workflowTestFrames(in: folder)
    let workflow = ReusableWorkflow(name: "stretch", steps: [.init(kind: .stretch, parameters: ["targetBackground": "0.25"])], exportOptions: .init(bitDepth: .sixteen))
    let batch = WorkflowBatchRun()
    try await batch.run(workflow: workflow, folders: [folder], output: root) { plan, inputs, directory, progress in
        try await model.executeReusableWorkflow(workflow, plan: plan, inputs: inputs, directory: directory, onStep: progress)
    }
    #expect(batch.items.count == 3)
    #expect(batch.items.allSatisfy { $0.status == .succeeded })
    for item in batch.items {
        let output = try #require(item.output, Comment(rawValue: item.message))
        let expected = root.appendingPathComponent("expected-\(item.id).tiff")
        let stretched = root.appendingPathComponent("stretched-\(item.id).tiff")
        _ = try await service.autoStretch(input: item.inputs[0], output: stretched, targetBackground: 0.25)
        _ = try await service.convert(input: stretched, output: expected, options: workflow.exportOptions)
        #expect(try workflowPixels(output) == workflowPixels(expected))
    }
}
