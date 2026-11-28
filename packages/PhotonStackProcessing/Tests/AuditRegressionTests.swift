import Foundation
import Testing
import PhotonStackAppCore
@testable import PhotonStackProcessing

@Test func auditDirectoryPublicationPreservesRegularFile() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
    defer { try? FileManager.default.removeItem(at: root) }
    let target = root.appendingPathComponent("existing")
    let tx = try #require(AtomicOutputTransaction(arguments: ["register-batch", "--output-dir", target.path], workingDirectory: root))
    try FileManager.default.createDirectory(at: tx.temporaryURL, withIntermediateDirectories: false)
    let original = Data("keep".utf8)
    try original.write(to: target)
    #expect(throws: ProcessingServiceError.self) { try tx.commit() }
    #expect(try Data(contentsOf: target) == original)
}

@Test(arguments: ["calibration-declare", "sensor-quality", "ser-extract", "xisf-import", "xisf-export", "plugin-run", "time-series"])
func newProductCommandsPreserveExistingDirectories(command: String) throws {
    let fm = FileManager.default
    let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try fm.createDirectory(at: root, withIntermediateDirectories: false)
    defer { try? fm.removeItem(at: root) }
    let target = root.appendingPathComponent("result")
    let tx = try #require(AtomicOutputTransaction(arguments: [command, "--output-dir", target.path], workingDirectory: root))
    try fm.createDirectory(at: tx.temporaryURL, withIntermediateDirectories: false)
    try fm.createDirectory(at: target, withIntermediateDirectories: false)
    let original = Data("original observation".utf8)
    let file = target.appendingPathComponent("science.fits")
    try original.write(to: file)
    #expect(throws: ProcessingServiceError.self) { try tx.commit() }
    tx.cleanUp()
    #expect(try Data(contentsOf: file) == original)
}

@Test(arguments: ["review-preview", "denoise-nonlocal", "develop"])
func auditReviewPreviewPublicationPreservesExistingAndConcurrentFiles(command: String) throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditPublish-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let output = root.appendingPathComponent("review.tiff")
    for existingBeforeStart in [true, false] {
        if existingBeforeStart { try Data("original".utf8).write(to: output) }
        var arguments = [command, "--output", output.path]
        if command == "develop" { arguments += ["--statistics-reference", "reference.fits"] }
        let transaction = try #require(AtomicOutputTransaction(arguments: arguments, workingDirectory: root))
        if !existingBeforeStart { try Data("original".utf8).write(to: output) }
        try Data("replacement".utf8).write(to: transaction.temporaryURL)
        #expect(throws: ProcessingServiceError.self) { try transaction.commit() }
        #expect(try String(contentsOf: output, encoding: .utf8) == "original")
        transaction.cleanUp()
        try FileManager.default.removeItem(at: output)
    }
}

@Test func auditOrdinaryDevelopRetainsReplaceableOutputContract() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditDevelop-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let output = root.appendingPathComponent("output.fits")
    try Data("old".utf8).write(to: output)
    let transaction = try #require(AtomicOutputTransaction(arguments: ["develop", "--output", output.path], workingDirectory: root))
    try Data("new".utf8).write(to: transaction.temporaryURL)
    try transaction.commit()
    #expect(try String(contentsOf: output, encoding: .utf8) == "new")
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil),
      arguments: ["denoise-nonlocal", "develop"])
func auditRealCLIServicePreservesExistingScienceOutputs(command: String) async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditScienceOutput-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let input = root.appendingPathComponent("rgb.fits"), mask = root.appendingPathComponent("mask.fits")
    try auditWriteFits(to: input, channels: 3)
    try auditWriteFits(to: mask, channels: 1)
    let output = root.appendingPathComponent("output.fits")
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let service = CLIProcessingService(executableURL: cli, workingDirectoryURL: root)
    var arguments = [command, "--input", input.path, "--output", output.path]
    arguments += command == "develop"
        ? ["--statistics-reference", input.path, "--stellar-balance", "off"]
        : ["--blend-map", mask.path, "--h", "0"]
    _ = try await service.runCLI(arguments: arguments)
    #expect(FileManager.default.fileExists(atPath: output.path))
    let original = Data("original science file".utf8)
    try original.write(to: output)
    do {
        _ = try await service.runCLI(arguments: arguments)
        Issue.record("The service replaced an existing output for \(command)")
    } catch let error as ProcessingServiceError {
        guard case .outputCommitFailed = error else { throw error }
    }
    #expect(try Data(contentsOf: output) == original)
    #expect(try FileManager.default.contentsOfDirectory(atPath: root.path).allSatisfy { !$0.contains("photonstack-output-") })
}

private func auditWriteFits(to url: URL, channels: Int) throws {
    let fields = [("SIMPLE", "T"), ("BITPIX", "-32"), ("NAXIS", "3"),
                  ("NAXIS1", "32"), ("NAXIS2", "32"), ("NAXIS3", String(channels))]
    var header = fields.map { key, value in
        (key.padding(toLength: 8, withPad: " ", startingAt: 0) + "= " + value)
            .padding(toLength: 80, withPad: " ", startingAt: 0)
    }.joined() + "END"
    header = header.padding(toLength: 2880, withPad: " ", startingAt: 0)
    var bytes = Data(header.utf8)
    for index in 0..<(32 * 32 * channels) {
        let value: Float = channels == 1 ? 1 : 0.1 + Float(index % 32) * 0.01
        var encoded = value.bitPattern.bigEndian
        withUnsafeBytes(of: &encoded) { bytes.append(contentsOf: $0) }
    }
    bytes.append(Data(repeating: 0, count: (2880 - bytes.count % 2880) % 2880))
    try bytes.write(to: url)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func auditChangedLightRecipesPassRealEngineValidation() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditRecipeSequence-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let service = CLIProcessingService(executableURL: cli)
    let schema = try await service.deepSkySchema()
    for action in ["remove", "change-role", "add", "remove-reference"] {
        var project = PhotonStackProject()
        project.addAssets(from: ["A", "B", "C"].map { root.appendingPathComponent($0 + ".fits") })
        var settings = DeepSkySettings()
        settings.values["registration.reference"] = "1"
        settings.values["stack.weights"] = "1,2,3"
        project.deepSkySettings = settings
        switch action {
        case "remove": project.removeAsset(project.assets[0].id)
        case "change-role": project.updateRole(for: project.assets[0].id, role: .dark)
        case "add": project.addAssets(from: [root.appendingPathComponent("D.fits")])
        default: project.removeAsset(project.assets[1].id)
        }
        let lights = project.assets.filter { $0.role == .light }.map(\.originalURL)
        let recipe = root.appendingPathComponent(action + ".txt")
        try #require(project.deepSkySettings).recipe(schema: schema, inputs: lights)
            .write(to: recipe, atomically: true, encoding: .utf8)
        let parsed = try await service.inspectDeepSkyRecipe(recipe)
        #expect(parsed.inputs == lights.map { PhotonStackProject.assetIdentityPath(for: $0) })
        #expect(parsed.values["stack.weights"]?.split(separator: ",").count == lights.count)
        let referenceText = try #require(parsed.values["registration.reference"])
        let reference = try #require(Int(referenceText))
        if action == "remove-reference" { #expect(reference == -1) }
        else {
            #expect(lights.indices.contains(reference))
            if lights.indices.contains(reference) { #expect(lights[reference].lastPathComponent == "B.fits") }
        }
    }
}

@Test func auditCurvePreviewRejectsNonfiniteControlsBeforeWriting() throws {
    let output = FileManager.default.temporaryDirectory.appendingPathComponent("AuditCurve-\(UUID()).png")
    try Data("original".utf8).write(to: output)
    defer { try? FileManager.default.removeItem(at: output) }
    var raw = RawProcessingOptions(); raw.exposureBias = .infinity
    #expect(throws: ProcessingServiceError.self) {
        try EngineCurvePreview.runIfAvailable(using: CLIProcessingService(executableURL: URL(fileURLWithPath: "/missing")),
            input: output, output: output, width: 10, rawOptions: raw, points: [], channel: .rgb)
    }
    #expect(try String(contentsOf: output, encoding: .utf8) == "original")
}

@Test func auditCurvePreviewEmptyCurveReportsNoApplicationAndPreservesOutputOnFailure() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AuditCurveOutput-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let input = root.appendingPathComponent("input.png"), output = root.appendingPathComponent("output.png")
    let png = try #require(Data(base64Encoded: "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="))
    try png.write(to: input)
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: "/missing"))
    let response = try #require(try EngineCurvePreview.runIfAvailable(using: service, input: input, output: output,
        width: 1, rawOptions: RawProcessingOptions(), points: [], channel: .rgb))
    let json = try #require(JSONSerialization.jsonObject(with: Data(response.standardOutput.utf8)) as? [String: Any])
    #expect(json["curveApplied"] as? Bool == false)
    let original = try Data(contentsOf: output)
    #expect(throws: ProcessingServiceError.self) {
        try EngineCurvePreview.runIfAvailable(using: service, input: root.appendingPathComponent("missing.png"), output: output,
            width: 1, rawOptions: RawProcessingOptions(), points: [], channel: .rgb)
    }
    #expect(try Data(contentsOf: output) == original)
    #expect(try FileManager.default.contentsOfDirectory(atPath: root.path).allSatisfy { !$0.contains("photonstack-output-") })
}
