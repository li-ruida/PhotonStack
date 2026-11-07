import Foundation
import PhotonStackAppCore
import Testing
@testable import PhotonStackProcessing

private func deepSkyTestSchema() throws -> DeepSkySchema {
    try JSONDecoder().decode(DeepSkySchema.self, from: Data(#"{"version":1,"fields":[{"key":"decode.demosaic","label":"Demosaic","chineseLabel":"去马赛克","group":"decode","type":"choice","defaultValue":"malvar","minimum":0,"maximum":0,"choices":["bilinear","malvar"]},{"key":"selection.overrides","label":"Overrides","chineseLabel":"逐帧选择","group":"selection","type":"text","defaultValue":"","minimum":0,"maximum":0,"choices":[]}]}"#.utf8))
}

private func writeDeepSkyProductStub(_ root: URL, output: URL, master: URL? = nil, developed: URL? = nil, linkDeveloped: Bool = false) throws -> CLIProcessingService {
    let report: [String: Any] = [
        "version": 1, "success": true, "analysisOnly": false,
        "inputCount": 0, "selectedCount": 0, "referenceIndex": 0,
        "recipe": "PHOTONSTACK_DEEP_SKY_RECIPE 1\n", "errorCode": "", "message": "",
        "master": (master ?? output.appendingPathComponent("stack-linear.fits")).path,
        "backgroundMaster": output.appendingPathComponent("stack-background.fits").path,
        "developed": (developed ?? output.appendingPathComponent("developed.tiff")).path,
        "rejectionLow": "", "rejectionHigh": "", "frames": [],
    ]
    try JSONSerialization.data(withJSONObject: report).write(to: root.appendingPathComponent("reply.json"))
    let executable = root.appendingPathComponent("fake-engine")
    let script = """
    #!/bin/sh
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output-dir" ]; then output="$2"; shift; fi
      shift
    done
    mkdir -p "$output"
    printf master > "$output/stack-linear.fits"
    printf background > "$output/stack-background.fits"
    \(linkDeveloped ? "ln -s \"$PWD/outside.tiff\" \"$output/developed.tiff\"" : "printf developed > \"$output/developed.tiff\"")
    cat "$PWD/reply.json"
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    return CLIProcessingService(executableURL: executable)
}

@Test(arguments: [false, true])
func deepSkyProductsAcceptBothDirectoryURLRepresentations(directoryHint: Bool) async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackDeepSkyPaths-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let recipe = root.appendingPathComponent("recipe.txt")
    try "PHOTONSTACK_DEEP_SKY_RECIPE 1\n".write(to: recipe, atomically: true, encoding: .utf8)
    let output = root.appendingPathComponent("stack", isDirectory: directoryHint)
    let service = try writeDeepSkyProductStub(root, output: output)
    let report = try await service.deepSky(recipe: recipe, outputDirectory: output, analyzeOnly: false)
    #expect(report.masterSHA256?.count == 64 && report.developedSHA256?.count == 64)
    #expect(FileManager.default.fileExists(atPath: output.appendingPathComponent("app-report.json").path))
    let master = URL(fileURLWithPath: report.master)
    let original = try Data(contentsOf: master)
    let finish = root.appendingPathComponent("finish", isDirectory: directoryHint)
    _ = try writeDeepSkyProductStub(root, output: finish, master: master)
    let refined = try await service.finishDeepSky(recipe: recipe, master: master, outputDirectory: finish, previous: report)
    #expect(refined.masterSHA256 == report.masterSHA256)
    #expect(refined.developed == finish.appendingPathComponent("developed.tiff").path)
    #expect(try Data(contentsOf: master) == original)
}

@Test(arguments: ["outside", "traversal", "symlink"])
func deepSkyProductsRejectEscapedOrLinkedOutputs(escape: String) async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackDeepSkyEscape-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let output = root.appendingPathComponent("stack", isDirectory: false)
    let outside = root.appendingPathComponent("outside.tiff")
    try Data("unrelated product".utf8).write(to: outside)
    let developed = escape == "symlink" ? output.appendingPathComponent("developed.tiff")
        : escape == "traversal" ? output.appendingPathComponent("../outside.tiff") : outside
    let service = try writeDeepSkyProductStub(root, output: output, developed: developed, linkDeveloped: escape == "symlink")
    do {
        _ = try await service.deepSky(recipe: root.appendingPathComponent("recipe.txt"), outputDirectory: output, analyzeOnly: false)
        Issue.record("Engine products must remain regular files inside the owned output directory")
    } catch DeepSkyRecipeError.invalidReport { }
    #expect(!FileManager.default.fileExists(atPath: output.appendingPathComponent("app-report.json").path))
    #expect(try String(contentsOf: outside, encoding: .utf8) == "unrelated product")
}

@Test func deepSkySettingsKeepOverridesAttachedToOriginalPaths() throws {
    let schema = try deepSkyTestSchema()
    let first = URL(fileURLWithPath: "/tmp/first 亮场.fit"), second = URL(fileURLWithPath: "/tmp/quoted\".fit")
    var settings = DeepSkySettings()
    settings.overrides[PhotonStackProject.assetIdentityPath(for: first)] = "reject"
    let forward = try settings.recipe(schema: schema, inputs: [first, second])
    let reverse = try settings.recipe(schema: schema, inputs: [second, first])
    #expect(forward.contains("selection.overrides \"reject,auto\""))
    #expect(reverse.contains("selection.overrides \"auto,reject\""))
    #expect(forward.contains("quoted\\\".fit"))
    settings.values["unknown"] = "value"
    #expect(throws: DeepSkyRecipeError.self) { try settings.recipe(schema: schema, inputs: [first]) }
}

@Test func deepSkySettingsPersistWithProjectAndOldProjectsStillDecode() throws {
    var project = PhotonStackProject()
    var settings = DeepSkySettings()
    settings.values["develop.saturation"] = "0.72"
    settings.values["develop.lock-denoise-stretch"] = "on"
    settings.overrides["/tmp/a.fit"] = "keep"
    project.deepSkySettings = settings
    let data = try JSONEncoder().encode(project)
    #expect(try JSONDecoder().decode(PhotonStackProject.self, from: data).deepSkySettings == settings)
    project.deepSkySettings = nil
    let legacy = try JSONEncoder().encode(project)
    #expect(try JSONDecoder().decode(PhotonStackProject.self, from: legacy).deepSkySettings == nil)
}

@Test func deepSkyRecipeRelinksQuotedPathsWithoutChangingSimilarNames() throws {
    let schema = try deepSkyTestSchema()
    let old = URL(fileURLWithPath: "/tmp/亮场 \"one\".fits")
    let new = URL(fileURLWithPath: "/tmp/new.fits")
    var settings = DeepSkySettings()
    settings.overrides[old.path] = "reject"
    let text = try settings.recipe(schema: schema, inputs: [old, URL(fileURLWithPath: old.path + ".fits")])
    let updated = DeepSkySettings.replacingPath(inRecipe: text, oldPath: old.path, newPath: new.path)
    #expect(updated.contains("input \"/tmp/new.fits\""))
    #expect(updated.contains("one\\\".fits.fits"))
    settings.relink(oldURL: old, newURL: new)
    #expect(settings.overrides[new.path] == "reject")
    #expect(settings.overrides[old.path] == nil)
}

@Test func deepSkyCancellationCleansOnlyOwnedIntermediates() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackDeepSkyCancel-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let executable = root.appendingPathComponent("fake-engine")
    let script = """
    #!/bin/sh
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output-dir" ]; then output="$2"; shift; fi
      shift
    done
    mkdir -p "$output/.aligned-work"
    echo partial > "$output/.aligned-work/frame.fits"
    echo retained > "$output/diagnostic.txt"
    printf '%s\\n' '{"type":"progress","task":"deep-sky","command":"deep-sky","stage":"ready","progress":0}'
    exec /bin/sleep 15
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    let service = CLIProcessingService(executableURL: executable)
    let output = root.appendingPathComponent("run")
    let jobID = UUID()
    let ready = AsyncStream<Void>.makeStream()
    let observer = NotificationCenter.default.addObserver(forName: .photonStackProcessingProgress, object: nil, queue: nil) { notification in
        if let event = notification.object as? ProcessingProgressEvent,
           event.jobID == jobID, event.stage == "ready" {
            ready.continuation.yield(())
        }
    }
    defer {
        ready.continuation.finish()
        NotificationCenter.default.removeObserver(observer)
    }
    let task = Task {
        try await ProcessingProgressContext.$jobID.withValue(jobID) {
            try await service.deepSky(recipe: root.appendingPathComponent("recipe.txt"), outputDirectory: output, analyzeOnly: false)
        }
    }
    defer { task.cancel() }
    guard await waitForEngineTestSignal(ready.stream) else {
        task.cancel()
        _ = try? await task.value
        Issue.record("Engine did not create intermediates and signal readiness")
        return
    }
    #expect(FileManager.default.fileExists(atPath: output.appendingPathComponent("diagnostic.txt").path))
    let cancelledAt = ContinuousClock.now
    task.cancel()
    do { _ = try await task.value; Issue.record("Cancelled processing must throw") }
    catch is CancellationError { }
    #expect(cancelledAt.duration(to: .now) < .seconds(3))
    #expect(!FileManager.default.fileExists(atPath: output.appendingPathComponent(".aligned-work").path))
    #expect(try String(contentsOf: output.appendingPathComponent("diagnostic.txt"), encoding: .utf8) == "retained\n")
    // A later run cannot replace the diagnostic or take ownership of this directory.
    do {
        _ = try await service.deepSky(recipe: root.appendingPathComponent("recipe.txt"), outputDirectory: output, analyzeOnly: false)
        Issue.record("Existing output must be refused")
    } catch let error as ProcessingServiceError {
        if case .outputCommitFailed = error {} else { Issue.record("Unexpected error: \(error)") }
    }
    #expect(FileManager.default.fileExists(atPath: output.appendingPathComponent("diagnostic.txt").path))
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func stackModeTransitionsProduceValidEngineRecipes() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let service = CLIProcessingService(executableURL: cli)
    let schema = try await service.deepSkySchema()
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackModes-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let url = root.appendingPathComponent("recipe.txt")
    let inputs = (0..<3).map { root.appendingPathComponent("\($0).fits") }
    #expect(schema.fields.first(where: { $0.key == "stack.method" })?.choices.contains("maximum") == true)
    for method in StackMethod.allCases {
        var settings = DeepSkySettings()
        settings.setCompatibleValue(method.rawValue, for: "stack.method", schema: schema)
        try settings.recipe(schema: schema, inputs: inputs).write(to: url, atomically: true, encoding: .utf8)
        #expect(try await service.inspectDeepSkyRecipe(url).values["stack.method"] == method.rawValue)
    }
    var trails = DeepSkySettings()
    trails.configureStarTrailComposition(schema: schema)
    try trails.recipe(schema: schema, inputs: inputs).write(to: url, atomically: true, encoding: .utf8)
    let inspected = try await service.inspectDeepSkyRecipe(url)
    #expect(inspected.values["registration.mode"] == "none")
    #expect(inspected.values["stack.method"] == "maximum")
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func deepSkyAppRecipeIsAcceptedAndCanonicalizedByRealEngine() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let service = CLIProcessingService(executableURL: cli)
    let schema = try await service.deepSkySchema()
    let toneReference = try #require(schema.fields.first { $0.key == "develop.lock-denoise-stretch" })
    #expect(toneReference.type == "bool" && toneReference.defaultValue == "off" && toneReference.group == "develop")
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackRecipeTest-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let inputs = ["亮场 one.fit", "quoted\".fit", "third.fit"].map { root.appendingPathComponent($0) }
    var settings = DeepSkySettings()
    settings.values["develop.saturation"] = "0.72"
    settings.overrides[inputs[1].path] = "reject"
    settings.values["calibration.sensor-pattern"] = "on"
    settings.values["develop.lock-denoise-stretch"] = "on"
    let url = root.appendingPathComponent("recipe.txt")
    try settings.recipe(schema: schema, inputs: inputs).write(to: url, atomically: true, encoding: .utf8)
    let imported = try await service.inspectDeepSkyRecipe(url)
    #expect(imported.inputs == inputs.map(\.path))
    #expect(imported.values["selection.overrides"] == "auto,reject,auto")
    #expect(imported.values["calibration.sensor-pattern"] == "on")
    #expect(imported.values["develop.lock-denoise-stretch"] == "on")
    #expect(Double(imported.values["develop.saturation"] ?? "0")! > 0.7199)
    #expect(Set(imported.values.keys) == Set(schema.fields.map(\.key)))
}
