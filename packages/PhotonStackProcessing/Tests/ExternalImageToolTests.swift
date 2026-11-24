import Foundation
import Testing
@testable import PhotonStackProcessing

private func externalFixture(_ script: String) throws -> (URL, URL) {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("External image ' space \(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let executable = root.appendingPathComponent("processor")
    try ("#!/bin/sh\n" + script).write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    return (root, executable)
}
private let tinyPNG = Data(base64Encoded: "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLttAAAAABJRU5ErkJggg==")!

@Test func externalImageToolStagesPositionalOutputAndRejectsExistingResult() async throws {
    let (root, executable) = try externalFixture("cp \"$1\" \"$2\"\nprintf '%s' \"$PWD\" > cwd.txt\n")
    defer { try? FileManager.default.removeItem(at: root) }
    let input = root.appendingPathComponent("original $quote.png"), output = root.appendingPathComponent("output.png")
    try tinyPNG.write(to: input)
    let tool = ExternalImageTool(name: "Test copy", executable: executable, arguments: ["{input}", "{output}"])
    _ = try await tool.run(input: input, output: output, workingDirectory: root)
    #expect(try Data(contentsOf: output) == tinyPNG)
    #expect(try String(contentsOf: root.appendingPathComponent("cwd.txt"), encoding: .utf8) == root.path)
    do { _ = try await tool.run(input: input, output: output, workingDirectory: root); Issue.record("Must not replace existing output") }
    catch { #expect(try Data(contentsOf: output) == tinyPNG) }
}

@Test func externalImageToolRejectsInvalidOutputAndRemovesTemporaryFile() async throws {
    let (root, executable) = try externalFixture("printf 'not an image' > \"$1\"\n")
    defer { try? FileManager.default.removeItem(at: root) }
    let input = root.appendingPathComponent("source.png"), output = root.appendingPathComponent("out.png")
    try tinyPNG.write(to: input)
    let tool = ExternalImageTool(name: "Invalid", executable: executable, arguments: ["{output}", "{input}"])
    do { _ = try await tool.run(input: input, output: output, workingDirectory: root); Issue.record("Must reject corrupt image") }
    catch {}
    #expect(!FileManager.default.fileExists(atPath: output.path))
    #expect(try FileManager.default.contentsOfDirectory(atPath: root.path).allSatisfy { !$0.contains("photonstack-output-") })
}

@Test func externalImageToolCancellationPreservesInputAndRemovesStaging() async throws {
    let (root, executable) = try externalFixture("cp \"$1\" \"$2\"\nprintf ready > ready\ntrap 'exit 1' TERM\nwhile :; do sleep .1; done\n")
    defer { try? FileManager.default.removeItem(at: root) }
    let input = root.appendingPathComponent("source.png"), output = root.appendingPathComponent("out.png")
    try tinyPNG.write(to: input)
    let tool = ExternalImageTool(name: "Slow", executable: executable, arguments: ["{input}", "{output}"])
    let task = Task { try await tool.run(input: input, output: output, workingDirectory: root) }
    // Match the other process fixtures' bounded startup allowance. Concurrent
    // subprocess tests may delay launch; the readiness marker, not a fixed
    // sleep, still proves that cancellation happens after output was staged.
    let deadline = ContinuousClock.now + .seconds(30)
    while !FileManager.default.fileExists(atPath: root.appendingPathComponent("ready").path) && ContinuousClock.now < deadline {
        try await Task.sleep(for: .milliseconds(20))
    }
    #expect(FileManager.default.fileExists(atPath: root.appendingPathComponent("ready").path))
    task.cancel()
    do { _ = try await task.value; Issue.record("Expected cancellation") } catch is CancellationError {} catch { Issue.record("\(error)") }
    #expect(try Data(contentsOf: input) == tinyPNG)
    #expect(!FileManager.default.fileExists(atPath: output.path))
    #expect(try FileManager.default.contentsOfDirectory(atPath: root.path).allSatisfy { !$0.contains("photonstack-output-") })
}

@Test func externalLinearToolRequiresCurrentLicenseAcceptance() throws {
    let (root, executable) = try externalFixture("exit 0\n")
    defer { try? FileManager.default.removeItem(at: root) }
    var tool = ExternalImageTool(name: "StarNet linear", executable: executable,
                                 arguments: ["{input}", "{output}"], linearFITS: true)
    #expect(throws: (any Error).self) { try tool.validate() }
    let license = root.appendingPathComponent("LICENSE.txt")
    try "Fixture terms, not a real product license.".write(to: license, atomically: true, encoding: .utf8)
    tool.licenseFile = license
    #expect(throws: (any Error).self) { try tool.validate() }
    tool.acceptedLicenseSHA256 = try ExternalImageTool.license(at: license).sha256
    try tool.validate()
    let encoded = try JSONEncoder().encode(tool)
    let restored = try JSONDecoder().decode(ExternalImageTool.self, from: encoded)
    #expect(restored == tool)
    try "Changed fixture terms.".write(to: license, atomically: true, encoding: .utf8)
    #expect(throws: (any Error).self) { try restored.validate() }
    let oldJSON = "{\"version\":1,\"name\":\"copy\",\"executable\":\"/bin/cp\",\"arguments\":[\"{input}\",\"{output}\"]}"
    let old = try JSONDecoder().decode(ExternalImageTool.self, from: Data(oldJSON.utf8))
    #expect(old.linearFITS == nil && old.licenseFile == nil)
}
