import Foundation
import PhotonStackAppCore
import Testing
@testable import PhotonStackProcessing

@Test func astroDevelopSettingsReplayRoundTrips() {
    var settings = AstroDevelopSettings()
    settings.stellarBalance = false
    settings.brightness = 2.3
    settings.background = 0.06
    settings.saturation = 1.2
    settings.shadowNeutralization = 0.65
    settings.starExposure = 0.2
    settings.starPeakThreshold = 0.75
    settings.toneCurve = "asinh"
    settings.toneScale = 20
    settings.whitePoint = 25000
    settings.redGain = 0.92
    settings.blueGain = 1.17
    #expect(AstroDevelopSettings(operationParameters: settings.operationParameters) == settings)
    #expect(settings.operationParameters["mode"] == "astro-v1")
}

@Test func astroDevelopServiceRequiresCreatedOutput() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackDevelop-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let executable = directory.appendingPathComponent("photonstack")
    let arguments = directory.appendingPathComponent("arguments.txt")
    try "#!/bin/sh\nprintf '%s\\n' \"$@\" > '\(arguments.path)'\nexit 0\n".write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    let output = directory.appendingPathComponent("result.tiff")
    let service = CLIProcessingService(executableURL: executable)
    var settings = AstroDevelopSettings()
    settings.starExposure = 0.35
    settings.starPeakThreshold = 0.75
    do {
        _ = try await service.develop(input: directory.appendingPathComponent("linear.fits"), output: output, settings: settings)
        Issue.record("Missing development output must fail")
    } catch let error as ProcessingServiceError {
        #expect(error == .outputMissing(path: output.path))
    }
    let args = try String(contentsOf: arguments, encoding: .utf8).split(separator: "\n").map(String.init)
    #expect(args.first == "develop")
    #expect(args.contains("--stellar-balance"))
    #expect(args.contains("0.35"))
    #expect(args.contains("--star-exposure"))
    let thresholdIndex = try #require(args.firstIndex(of: "--star-peak-threshold"))
    #expect(args[thresholdIndex + 1] == "0.75")
}

@Test func legacyAstroDevelopKeepsOriginalCurve() {
    let old = AstroDevelopSettings(operationParameters: ["mode": "astro-v1", "starExposure": "0.25"])
    #expect(old.toneCurve == "rational")
    #expect(old.starExposure == 0.25)
    #expect(old.starPeakThreshold == 0)
    #expect(old.toneScale == 0)
    #expect(old.shadowNeutralization == 0)
    #expect(AstroDevelopSettings().toneCurve == "asinh")
    #expect(AstroDevelopSettings().starExposure == 1)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_M31_DEVELOP_INPUT"] != nil))
func astroDevelopRealM31BrightStarThresholdMatchesReference() async throws {
    let env = ProcessInfo.processInfo.environment
    let input = URL(fileURLWithPath: try #require(env["PHOTONSTACK_M31_DEVELOP_INPUT"]))
    let reference = URL(fileURLWithPath: try #require(env["PHOTONSTACK_M31_DEVELOP_REFERENCE"]))
    let cli = URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_CLI"]))
    let folder = URL(fileURLWithPath: try #require(env["PHOTONSTACK_M31_TEST_OUTPUT"]))
    let output = folder.appendingPathComponent("develop-\(UUID().uuidString).tiff")
    var settings = AstroDevelopSettings()
    settings.stellarBalance = false
    // This reference used the CLI's default brightness (1), not the App's 1.5.
    settings.brightness = 1
    settings.toneCurve = "asinh"
    settings.toneScale = 45
    settings.whitePoint = 3300
    settings.background = 0.075
    settings.saturation = 0.55
    settings.shadowNeutralization = 0 // Preserve the historical reference rendering.
    settings.starExposure = 0.5
    settings.starPeakThreshold = 0.75
    settings.redGain = 0.644356
    settings.blueGain = 0.774812
    _ = try await CLIProcessingService(executableURL: cli).develop(input: input, output: output, settings: settings)
    #expect(try Data(contentsOf: output) == Data(contentsOf: reference))
    print("M31 development integration artifact: \(output.path)")
}
