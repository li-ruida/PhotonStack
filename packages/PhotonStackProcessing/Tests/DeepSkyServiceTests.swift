import Foundation
import PhotonStackAppCore
import Testing
@testable import PhotonStackProcessing

private func deepSkyTestSchema() throws -> DeepSkySchema {
    try JSONDecoder().decode(DeepSkySchema.self, from: Data(#"{"version":1,"fields":[{"key":"decode.demosaic","label":"Demosaic","chineseLabel":"去马赛克","group":"decode","type":"choice","defaultValue":"malvar","minimum":0,"maximum":0,"choices":["bilinear","malvar"]},{"key":"selection.overrides","label":"Overrides","chineseLabel":"逐帧选择","group":"selection","type":"text","defaultValue":"","minimum":0,"maximum":0,"choices":[]}]}"#.utf8))
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
    exec /bin/sleep 60
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    let service = CLIProcessingService(executableURL: executable)
    let output = root.appendingPathComponent("run")
    let task = Task { try await service.deepSky(recipe: root.appendingPathComponent("recipe.txt"), outputDirectory: output, analyzeOnly: false) }
    for _ in 0..<100 {
        if FileManager.default.fileExists(atPath: output.appendingPathComponent("diagnostic.txt").path) { break }
        try await Task.sleep(for: .milliseconds(20))
    }
    #expect(FileManager.default.fileExists(atPath: output.appendingPathComponent("diagnostic.txt").path))
    task.cancel()
    do { _ = try await task.value; Issue.record("Cancelled processing must throw") }
    catch is CancellationError { }
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
func deepSkyAppRecipeIsAcceptedAndCanonicalizedByRealEngine() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let service = CLIProcessingService(executableURL: cli)
    let schema = try await service.deepSkySchema()
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackRecipeTest-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let inputs = ["亮场 one.fit", "quoted\".fit", "third.fit"].map { root.appendingPathComponent($0) }
    var settings = DeepSkySettings()
    settings.values["develop.saturation"] = "0.72"
    settings.overrides[inputs[1].path] = "reject"
    settings.values["calibration.sensor-pattern"] = "on"
    let url = root.appendingPathComponent("recipe.txt")
    try settings.recipe(schema: schema, inputs: inputs).write(to: url, atomically: true, encoding: .utf8)
    let imported = try await service.inspectDeepSkyRecipe(url)
    #expect(imported.inputs == inputs.map(\.path))
    #expect(imported.values["selection.overrides"] == "auto,reject,auto")
    #expect(imported.values["calibration.sensor-pattern"] == "on")
    #expect(Double(imported.values["develop.saturation"] ?? "0")! > 0.7199)
    #expect(Set(imported.values.keys) == Set(schema.fields.map(\.key)))
}
