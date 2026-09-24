import Foundation
import PhotonStackAppCore
import Testing
@testable import PhotonStackProcessing

@Test func continuumToneSettingsRoundTripAndRejectCorruptRecords() {
    var settings = ContinuumToneSettings()
    #expect(settings.isValid)
    #expect(ContinuumToneSettings(operationParameters: settings.operationParameters) == settings)
    #expect(ContinuumToneSettings(operationParameters: ["amount":"0.25", "radius":"8"]) == nil)
    for (key, value) in [("mode","future"),("amount","nan"),("radius","3"),("starChroma","2"),
                         ("points","0:0,0.5:0.7,0.8:0.6,1:1"),("points","0:0,0.5:0.5,0.5000001:0.6,1:1"),
                         ("points","0:0,nan:0.5,1:1"),("points","0:0,0.8:0.9")] {
        var p = settings.operationParameters; p[key] = value
        #expect(ContinuumToneSettings(operationParameters: p) == nil)
    }
    var missing = settings.operationParameters; missing.removeValue(forKey: "points")
    #expect(ContinuumToneSettings(operationParameters: missing) == nil)
    settings.setCurveStrength(0)
    #expect(settings.isValid)
    #expect(settings.curvePoints.split(separator: ",").allSatisfy { pair in
        let v = pair.split(separator: ":"); return Double(v[0]) == Double(v[1])
    })
    settings.setCurveStrength(0.5)
    #expect(settings.isValid && ContinuumToneSettings(operationParameters: settings.operationParameters) == settings)
    settings.setCurveStrength(.infinity)
    #expect(!settings.isValid)
}

@Test func continuumToneServicePassesAllParametersAndRejectsFITSBeforeStaging() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackContinuum-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let executable = directory.appendingPathComponent("photonstack")
    let arguments = directory.appendingPathComponent("arguments.txt")
    try "#!/bin/sh\nprintf '%s\\n' \"$@\" > '\(arguments.path)'\nexit 0\n".write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    let service = CLIProcessingService(executableURL: executable)
    let output = directory.appendingPathComponent("output.tiff")
    var bad = ContinuumToneSettings(); bad.fineRadius = bad.radius
    do {
        _ = try await service.continuumTone(input: directory.appendingPathComponent("input.tiff"), output: output, settings: bad)
        Issue.record("Invalid settings must fail before launching the CLI")
    } catch let error as ContinuumToneError { #expect(error == .invalidSettings) }
    for (i, o) in [("input.FITS","out.tiff"),("input.tiff","out.fit")] {
        do {
            _ = try await service.continuumTone(input: directory.appendingPathComponent(i), output: directory.appendingPathComponent(o), settings: ContinuumToneSettings())
            Issue.record("FITS boundary must fail")
        } catch let error as ContinuumToneError { #expect(error == .displayImageRequired) }
    }
    #expect(!FileManager.default.fileExists(atPath: arguments.path))
    do {
        _ = try await service.continuumTone(input: directory.appendingPathComponent("input.tiff"), output: output, settings: ContinuumToneSettings())
        Issue.record("Success without output must fail")
    } catch let error as ProcessingServiceError { #expect(error == .outputMissing(path: output.path)) }
    let args = try String(contentsOf: arguments, encoding: .utf8).split(separator: "\n").map(String.init)
    for (flag,value) in [("--protect-structure","on"),("--amount","0.95"),("--radius","12"),
                         ("--fine-radius","3"),("--star-chroma","0.7"),("--continuum-curve",ContinuumToneSettings.defaultCurve)] {
        let index = try #require(args.firstIndex(of: flag)); #expect(args[index+1] == value)
    }
}
