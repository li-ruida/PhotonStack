import Foundation
import ImageIO
import PhotonStackAppCore
import PhotonStackProcessing
import Testing
@testable import PhotonStackUI

private func writeDeepSkyTestLight(_ url: URL, frame: Int, bayer: Bool = false) throws {
    func card(_ key: String, _ value: String) -> String {
        let text = key.padding(toLength: 8, withPad: " ", startingAt: 0) + "= " + value
        return text.padding(toLength: 80, withPad: " ", startingAt: 0)
    }
    var header = card("SIMPLE", "T") + card("BITPIX", "-32") + card("NAXIS", "2") + card("NAXIS1", "64") + card("NAXIS2", "64")
    if bayer { header += card("BAYERPAT", "'GRBG'") }
    header += "END".padding(toLength: 80, withPad: " ", startingAt: 0)
    header = header.padding(toLength: 2880, withPad: " ", startingAt: 0)
    var data = Data(header.utf8)
    for y in 0..<64 {
        for x in 0..<64 {
            let r2 = Double((x - 32) * (x - 32) + (y - 32) * (y - 32))
            let noise = Double((x * 13 + y * 17 + frame * 7) % 19) * 0.1
            let cfaGain = bayer ? (y % 2 == 0 ? (x % 2 == 0 ? 1.0 : 1.3) : (x % 2 == 0 ? 0.6 : 1.0)) : 1
            let value = Float((100 + Double(frame * 2) + 30 * exp(-r2 / 80) + noise) * cfaGain)
            var big = value.bitPattern.bigEndian
            withUnsafeBytes(of: &big) { data.append(contentsOf: $0) }
        }
    }
    data.append(Data(repeating: 0, count: (2880 - data.count % 2880) % 2880))
    try data.write(to: url)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func deepSkyReviewPreviewPreservesOriginalResolutionAndEditorState() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackReviewTest-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let input = root.appendingPathComponent("original.fits")
    try writeDeepSkyTestLight(input, frame: 0, bayer: true)
    let original = try Data(contentsOf: input)
    let model = PhotonStackWorkspaceModel(autosaveEnabled: false, processingService: CLIProcessingService(executableURL: cli), previewDirectory: root)
    var store: DeepSkyReviewPreviewStore? = DeepSkyReviewPreviewStore()
    let preview = try await store!.load(input.path, model: model)
    let cached = try await store!.load(input.path, model: model)
    #expect(cached == preview)
    let source = try #require(CGImageSourceCreateWithURL(preview as CFURL, nil))
    let image = try #require(CGImageSourceCreateImageAtIndex(source, 0, nil))
    #expect(image.width == 64 && image.height == 64)
    #expect(image.colorSpace?.model == .rgb)
    #expect(try Data(contentsOf: input) == original)
    #expect(model.previewURL == nil)
    #expect(model.project.editGraph.operations.isEmpty)
    #expect(image.bitsPerComponent == 8)
    #expect(try FileManager.default.contentsOfDirectory(atPath: preview.deletingLastPathComponent().path).count == 1)
    // A decoder setting or source revision must invalidate the cached preview.
    model.setDeepSkyValue("off", for: "decode.debayer")
    let mono = try await store!.load(input.path, model: model)
    #expect(mono != preview)
    let monoImage = try #require(NativePreviewImageDecoder.decodeCGImage(contentsOf: mono))
    #expect(monoImage.width == 64)
    try writeDeepSkyTestLight(input, frame: 3, bayer: true)
    let revised = try await store!.load(input.path, model: model)
    #expect(revised != mono)
    store = nil
    #expect(!FileManager.default.fileExists(atPath: preview.deletingLastPathComponent().path))
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func deepSkyAppWorkflowMatchesCLIAndReopensRecipe() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackDeepSkyAppTest-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    var project = PhotonStackProject(name: "Deep sky parity test")
    let inputs = (0..<7).map { root.appendingPathComponent("原片 \($0).fits") }
    for (frame, input) in inputs.enumerated() { try writeDeepSkyTestLight(input, frame: frame) }
    let originals = try inputs.map { try Data(contentsOf: $0) }
    project.addAssets(from: inputs)
    var settings = DeepSkySettings()
    settings.values = ["registration.mode": "none", "selection.measure-stars": "off", "background.model": "none",
                       "develop.stellar-balance": "off", "develop.tone-scale": "10", "develop.white-point": "200"]
    project.deepSkySettings = settings
    let service = CLIProcessingService(executableURL: cli)
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false, processingService: service,
                                        previewDirectory: root.appendingPathComponent("app"))
    await model.runDeepSkyWorkflow(analyzeOnly: true)
    #expect(model.errorMessage == nil)
    #expect(model.deepSkySettings.lastReport?.selectedCount == inputs.count)
    await model.runDeepSkyWorkflow(analyzeOnly: false)
    #expect(model.errorMessage == nil)
    let report = try #require(model.deepSkySettings.lastReport)
    #expect(report.success && !report.analysisOnly)
    #expect(report.crop?.width == 64 && report.crop?.height == 64)
    #expect(report.masterSHA256?.count == 64)
    #expect(report.developedSHA256?.count == 64)
    let operation = try #require(model.project.editGraph.operations.last)
    #expect(operation.parameters["mode"] == "deepSkyRecipe")
    #expect(operation.parameters["recipe"] == report.recipe)
    #expect(operation.parameters["report"]?.hasSuffix("app-report.json") == true)
    #expect(model.previewURL?.path == report.developed)
    let recipeURL = root.appendingPathComponent("cli-recipe.txt")
    try report.recipe.write(to: recipeURL, atomically: true, encoding: .utf8)
    let cliReport = try await service.deepSky(recipe: recipeURL, outputDirectory: root.appendingPathComponent("cli"), analyzeOnly: false)
    #expect(try Data(contentsOf: URL(fileURLWithPath: report.master)) == Data(contentsOf: URL(fileURLWithPath: cliReport.master)))
    #expect(try Data(contentsOf: URL(fileURLWithPath: report.developed)) == Data(contentsOf: URL(fileURLWithPath: cliReport.developed)))
    // On-disk report paths must survive the App processing service's output handling.
    let stored = try JSONDecoder().decode(DeepSkyReport.self, from: Data(contentsOf: URL(fileURLWithPath: report.master).deletingLastPathComponent().appendingPathComponent("report.json")))
    #expect(stored.master == report.master)
    let masterBytes = try Data(contentsOf: URL(fileURLWithPath: report.master))
    model.setDeepSkyValue("0.9", for: "denoise.multiscale-chroma")
    #expect(model.canRefinishDeepSkyMaster)
    #expect(DeepSkyReview.sameAnalysisRecipe(try model.deepSkyRecipeText(), report.recipe))
    #expect(!DeepSkyReview.sameResultRecipe(try model.deepSkyRecipeText(), report.recipe))
    model.updateDeepSkyOverrides([inputs[0].path: "reject"])
    #expect(!model.canRefinishDeepSkyMaster)
    let blockedJobs = model.jobs.count
    await model.refinishDeepSkyMaster()
    #expect(model.jobs.count == blockedJobs)
    model.updateDeepSkyOverrides([inputs[0].path: "auto"])
    model.setDeepSkyValue("2", for: "crop.inset")
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage == nil)
    let finished = try #require(model.deepSkySettings.lastReport)
    #expect(finished.master == report.master && finished.frames == report.frames)
    #expect(DeepSkyReview.sameResultRecipe(try model.deepSkyRecipeText(), finished.recipe))
    #expect(finished.developed != report.developed)
    #expect(finished.masterSHA256 == report.masterSHA256)
    #expect(finished.developedSHA256?.count == 64 && finished.developedSHA256 != report.developedSHA256)
    #expect(finished.crop?.x == 2 && finished.crop?.width == 60)
    let finishedOperation = try #require(model.project.editGraph.operations.last)
    let geometry = try await ResultComparisonProvenanceLoader().load(
        ResultComparisonSources(currentReport: URL(fileURLWithPath: try #require(finishedOperation.parameters["report"])),
                                referenceReport: URL(fileURLWithPath: try #require(operation.parameters["report"]))),
        currentURL: URL(fileURLWithPath: finished.developed), referenceURL: URL(fileURLWithPath: report.developed),
        currentSize: CGSize(width: 60, height: 60), referenceSize: CGSize(width: 64, height: 64))
    #expect(geometry.common == CGRect(x: 2, y: 2, width: 60, height: 60))
    #expect(model.previewURL?.path == finished.developed)
    #expect(try Data(contentsOf: URL(fileURLWithPath: finished.master)) == masterBytes)
    try finished.validate()
    // A file with the same path and length is not necessarily the same master.
    let masterURL = URL(fileURLWithPath: finished.master)
    var changedMaster = masterBytes; changedMaster[changedMaster.count - 1] ^= 1
    try changedMaster.write(to: masterURL)
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage?.contains("Linear master changed") == true)
    #expect(model.deepSkySettings.lastReport == finished)
    try masterBytes.write(to: masterURL)
    var changedSource = originals[0]; changedSource.append(0)
    try changedSource.write(to: inputs[0])
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage?.contains("Source files changed") == true)
    #expect(model.deepSkySettings.lastReport == finished)
    try originals[0].write(to: inputs[0])
    // Re-analysis replaces the report but must not lose access to the completed stack.
    await model.runDeepSkyWorkflow(analyzeOnly: true)
    #expect(model.deepSkySettings.lastReport?.analysisOnly == true)
    #expect(model.latestDeepSkyResultURL?.path == finished.developed)
    let historyBeforeNavigation = model.project.editGraph
    let jobsBeforeNavigation = model.jobs.count
    model.showLatestDeepSkyResult()
    #expect(model.previewURL?.path == finished.developed)
    #expect(model.project.editGraph == historyBeforeNavigation)
    #expect(model.jobs.count == jobsBeforeNavigation)
    let output = URL(fileURLWithPath: finished.developed)
    let hidden = output.appendingPathExtension("temporarily-missing")
    try FileManager.default.moveItem(at: output, to: hidden)
    #expect(model.latestDeepSkyResultURL == nil)
    model.showLatestDeepSkyResult()
    #expect(model.project.editGraph == historyBeforeNavigation)
    try FileManager.default.moveItem(at: hidden, to: output)
    let saved = root.appendingPathComponent("saved-project")
    await model.saveProject(to: saved)
    let reopened = try await ProjectRepository().load(from: saved)
    #expect(reopened.deepSkySettings == model.project.deepSkySettings)
    for (input, original) in zip(inputs, originals) { #expect(try Data(contentsOf: input) == original) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func deepSkyIndependentNoiseSurvivesReprocessingAndProjectReopen() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackNoiseApp-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let inputs = (0..<12).map { root.appendingPathComponent("light-\($0).fits") }
    for (frame, input) in inputs.enumerated() { try writeDeepSkyTestLight(input, frame: frame) }
    let originals = try inputs.map { try Data(contentsOf: $0) }
    var project = PhotonStackProject(name: "Independent luminance noise integration")
    project.addAssets(from: inputs)
    var settings = DeepSkySettings()
    settings.values = ["denoise.noise-model": "independent-luminance", "denoise.h": "0",
        "stack.sigma-low": "20", "stack.sigma-high": "20", "selection.apply": "off",
        "registration.mode": "none", "selection.measure-stars": "off", "develop.stellar-balance": "off",
        "develop.tone-scale": "10", "develop.white-point": "200"]
    project.deepSkySettings = settings
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false,
        processingService: CLIProcessingService(executableURL: cli), previewDirectory: root.appendingPathComponent("processing"))
    await model.runDeepSkyWorkflow(analyzeOnly: false)
    #expect(model.errorMessage == nil)
    let first = try #require(model.deepSkySettings.lastReport)
    try #require(first.success && first.independentLuminanceNoise == true && first.noiseReferencePaths.count == 3)
    let masterBytes = try Data(contentsOf: URL(fileURLWithPath: first.master))
    let noiseBytes = try first.noiseReferencePaths.map { try Data(contentsOf: URL(fileURLWithPath: $0)) }
    #expect(model.canRefinishDeepSkyMaster)
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage == nil)
    #expect(model.deepSkySettings.lastReport?.developedSHA256 == first.developedSHA256)
    model.setDeepSkyValue("scene", for: "denoise.noise-model")
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage == nil)
    #expect(model.deepSkySettings.lastReport?.independentLuminanceNoise == false)
    #expect(model.deepSkySettings.lastReport?.noiseReferencePaths == first.noiseReferencePaths)
    model.setDeepSkyValue("independent-luminance", for: "denoise.noise-model")
    #expect(model.canRefinishDeepSkyMaster)
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage == nil)
    let latest = try #require(model.deepSkySettings.lastReport)
    #expect(latest.developedSHA256 == first.developedSHA256)
    let saved = root.appendingPathComponent("saved")
    await model.saveProject(to: saved)
    let reopened = try await ProjectRepository().load(from: saved)
    #expect(reopened.deepSkySettings?.lastReport?.noiseReferencePaths == first.noiseReferencePaths)
    #expect(try Data(contentsOf: URL(fileURLWithPath: first.master)) == masterBytes)
    for (path, bytes) in zip(first.noiseReferencePaths, noiseBytes) {
        #expect(try Data(contentsOf: URL(fileURLWithPath: path)) == bytes)
    }
    let noiseURL = URL(fileURLWithPath: try #require(first.noiseReference))
    var damaged = noiseBytes[0]; damaged[damaged.count - 1] ^= 1
    try damaged.write(to: noiseURL)
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage != nil && model.deepSkySettings.lastReport == latest)
    for (input, original) in zip(inputs, originals) { #expect(try Data(contentsOf: input) == original) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_SENSOR_FIXTURE"] != nil && ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil), arguments: [false, true]) @MainActor
func deepSkySensorCorrectionRunsThroughAppAndReusesMaster(independentNoise: Bool) async throws {
    // The C++ workflow fixture generator supplies moving CFA stars and a known
    // fixed sensor component. This exercises the real service and App model.
    let fixture = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_SENSOR_FIXTURE"]))
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackSensorApp-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let inputs = try (0..<36).map { index in
        let url = root.appendingPathComponent("light-\(index).fits")
        try FileManager.default.copyItem(at: fixture.appendingPathComponent("\(index).fits"), to: url)
        return url
    }
    let originals = try inputs.map { try Data(contentsOf: $0) }
    var settings = DeepSkySettings()
    settings.values = ["calibration.sensor-pattern": "on", "registration.reference": "0", "registration.minimum-matches": "6",
                       "selection.measure-stars": "off", "selection.measure-trails": "off", "selection.apply": "off",
                       "background.model": "none", "develop.align-channels": "off", "develop.stellar-balance": "off",
                       "develop.tone-scale": "10", "develop.white-point": "700", "crop.coverage": "1",
                       "denoise.multiscale-luminance": "0", "denoise.multiscale-chroma": "0"]
    var project = PhotonStackProject(name: "Sensor correction integration")
    if independentNoise {
        settings.values["denoise.noise-model"] = "independent-luminance"
        settings.values["denoise.h"] = "0"
        settings.values["denoise.multiscale-luminance"] = "0.05"
    }
    project.addAssets(from: inputs); project.deepSkySettings = settings
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false,
        processingService: CLIProcessingService(executableURL: cli), previewDirectory: root.appendingPathComponent("processing"))
    await model.runDeepSkyWorkflow(analyzeOnly: false)
    #expect(model.errorMessage == nil)
    let first = try #require(model.deepSkySettings.lastReport)
    #expect(first.success && first.selectedCount == 36)
    #expect(first.frames.allSatisfy { !$0.warnings.contains("stellar-measurements-unavailable") })
    #expect(first.frames.allSatisfy { $0.sha256?.count == 64 && (0...2).contains($0.sensorPatternModel ?? -1) })
    #expect(first.sensorPatternReport?.isEmpty == false)
    if independentNoise { #expect(first.independentLuminanceNoise == true && first.noiseReferencePaths.count == 3) }
    let masterBytes = try Data(contentsOf: URL(fileURLWithPath: first.master))
    #expect(model.canRefinishDeepSkyMaster)
    model.setDeepSkyValue("0.05", for: "denoise.multiscale-luminance")
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage == nil)
    let finished = try #require(model.deepSkySettings.lastReport)
    #expect(finished.master == first.master && finished.developed != first.developed)
    #expect(finished.frames == first.frames && finished.sensorPatternReport == first.sensorPatternReport)
    #expect(finished.recipe.contains("calibration.sensor-pattern \"on\""))
    #expect(try Data(contentsOf: URL(fileURLWithPath: finished.master)) == masterBytes)
    #expect(try inputs.map { try Data(contentsOf: $0) } == originals)
    try finished.validate()
}
