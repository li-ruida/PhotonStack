import Foundation
import PhotonStackAppCore
import Testing
@testable import PhotonStackProcessing

@Test func greenCastSettingsKeepLegacyAndNewSemantics() {
    let legacy = GreenCastSettings(operationParameters: ["mode": "removeGreen", "amount": "0.7", "backgroundLimit": "0.34"])
    #expect(legacy.method == .background)
    #expect(legacy.greenThreshold == 0.01)
    #expect(!legacy.preserveLightness)
    #expect(legacy.amount == 0.7)
    var current = GreenCastSettings()
    current.amount = 0.42
    current.greenThreshold = 0.025
    #expect(GreenCastSettings(operationParameters: current.operationParameters) == current)
    #expect(current.method == .averageNeutral && current.preserveLightness && current.isValid)
    current.amount = .nan
    #expect(!current.isValid)
}

@Test func greenCastServicePassesStyleAndRequiresOutput() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackGreen-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let executable = directory.appendingPathComponent("photonstack")
    let arguments = directory.appendingPathComponent("arguments.txt")
    try "#!/bin/sh\nprintf '%s\\n' \"$@\" > '\(arguments.path)'\nexit 0\n".write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    let output = directory.appendingPathComponent("result.tiff")
    let service = CLIProcessingService(executableURL: executable)
    do {
        _ = try await service.removeGreenCast(input: directory.appendingPathComponent("display.tiff"), output: output, settings: GreenCastSettings())
        Issue.record("A successful exit without an output must fail")
    } catch let error as ProcessingServiceError {
        #expect(error == .outputMissing(path: output.path))
    }
    let args = try String(contentsOf: arguments, encoding: .utf8).split(separator: "\n").map(String.init)
    for (flag, value) in [("--green-method", "average-neutral"), ("--green-threshold", "0.0"), ("--preserve-lightness", "on")] {
        let i = try #require(args.firstIndex(of: flag))
        #expect(args[i + 1] == value)
    }
}
