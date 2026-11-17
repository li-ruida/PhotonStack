import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

private func writeDualBandFixture(_ path: URL, constant: Float? = nil) throws -> Data {
    let cards = [("SIMPLE", "T"), ("BITPIX", "-32"), ("NAXIS", "2"), ("NAXIS1", "32"), ("NAXIS2", "32"), ("BAYERPAT", "'RGGB'")]
    var text = cards.map { key, value in
        (key.padding(toLength: 8, withPad: " ", startingAt: 0) + "= " + value).padding(toLength: 80, withPad: " ", startingAt: 0)
    }.joined() + "END"
    text = text.padding(toLength: 2880, withPad: " ", startingAt: 0)
    var bytes = Data(text.utf8)
    for y in 0..<32 { for x in 0..<32 {
        let value = constant ?? Float((x % 2 == 0 && y % 2 == 0 ? -10 : 20) + x + y)
        var word = value.bitPattern.bigEndian
        withUnsafeBytes(of: &word) { bytes.append(contentsOf: $0) }
    }}
    bytes.append(Data(repeating: 0, count: (2880 - bytes.count % 2880) % 2880))
    try bytes.write(to: path); return bytes
}
@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func narrowbandSessionExtractRemixReopenAndReplayProtectsInputsAndDestinations() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let service = CLIProcessingService(executableURL: cli)
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("Narrowband-\(UUID())", isDirectory: true)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let source = root.appendingPathComponent("original with spaces.fits")
    let original = try writeDualBandFixture(source)
    let lines = root.appendingPathComponent("lines", isDirectory: true)
    let session = NarrowbandSession()
    let ha = try await session.run(["narrowband-extract", "--input", source.path, "--output-dir", lines.path], output: lines, using: service)
    #expect(ha.lastPathComponent == "Ha.fits")
    #expect(session.report?.operation == "extract")
    #expect(session.directory == lines)
    #expect(try await session.verifiedOutput("Ha-valid.fits", using: service) == lines.appendingPathComponent("Ha-valid.fits"))
    session.greenHydrogen = 0.35
    let mixed = root.appendingPathComponent("mixed", isDirectory: true)
    let image = try await session.remix(output: mixed, using: service)
    #expect(session.report?.operation == "compose" && session.report?.greenHydrogen == 0.35)
    #expect(image.lastPathComponent == "HOO.fits")
    let content = try Data(contentsOf: image)
    session.greenHydrogen = 0.7
    _ = try await session.verifiedOutput("HOO.fits", using: service)
    #expect(session.greenHydrogen == 0.7)
    let restored = NarrowbandSession()
    _ = try await restored.open(mixed, using: service)
    #expect(restored.greenHydrogen == 0.35)
    let replay = root.appendingPathComponent("replay", isDirectory: true)
    let replayed = try await restored.replay(output: replay, using: service)
    #expect(try Data(contentsOf: replayed) == content)
    do { _ = try await restored.replay(output: mixed, using: service); Issue.record("Existing destination replaced") } catch {}
    #expect(try Data(contentsOf: image) == content)
    #expect(try Data(contentsOf: source) == original)
    var changed = original; changed[changed.count - 1] ^= 1; try changed.write(to: source)
    do { _ = try await restored.open(lines, using: service); Issue.record("Modified source accepted") } catch {}
    try original.write(to: source)
    var corrupt = content; corrupt[corrupt.count - 1] ^= 1; try corrupt.write(to: image)
    do { _ = try await restored.open(mixed, using: service); Issue.record("Modified output accepted") } catch {}
}


@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func narrowbandCalibrationRestoresMastersAndPreservesPendingChoices() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let service = CLIProcessingService(executableURL: cli)
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("NarrowbandCalibration-\(UUID())", isDirectory: true)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let source = root.appendingPathComponent("light.fits"), bias = root.appendingPathComponent("master bias.fits")
    _ = try writeDualBandFixture(source)
    let master = try writeDualBandFixture(bias, constant: 10)
    let session = NarrowbandSession()
    session.useCalibration = true
    #expect(!session.canCalibrate)
    session.calibration.bias = bias.path
    #expect(session.canCalibrate)
    let result = root.appendingPathComponent("result", isDirectory: true)
    let image = try await session.run(["narrowband-extract", "--input", source.path, "--output-dir", result.path] + session.calibrationArguments,
                                      output: result, using: service)
    let bytes = try Data(contentsOf: image)
    let word = bytes[2880..<2884].reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
    #expect(Float(bitPattern: word) == -20)
    let restored = NarrowbandSession()
    _ = try await restored.open(result, using: service)
    #expect(restored.useCalibration)
    #expect(URL(fileURLWithPath: restored.calibration.bias).resolvingSymlinksInPath() == bias.resolvingSymlinksInPath())
    restored.useCalibration = false
    restored.calibration.bias = "/pending/master.fits"
    _ = try await restored.verifiedOutput("OIII.fits", using: service)
    #expect(!restored.useCalibration && restored.calibration.bias == "/pending/master.fits")
    let replay = root.appendingPathComponent("replay", isDirectory: true)
    let replayed = try await restored.replay(output: replay, using: service)
    #expect(try Data(contentsOf: replayed) == bytes)
    #expect(restored.useCalibration)
    #expect(URL(fileURLWithPath: restored.calibration.bias).resolvingSymlinksInPath() == bias.resolvingSymlinksInPath())
    var changed = master; changed[changed.count - 1] ^= 1; try changed.write(to: bias)
    do { _ = try await restored.open(result, using: service); Issue.record("Changed bias master accepted") } catch {}
}

@Test func narrowbandCalibrationRequiresExplicitBiasHandling() {
    var options = NarrowbandCalibrationReport()
    options.dark = "/dark.fits"
    #expect(!options.isUsable)
    options.darkBiasState = "included"
    #expect(options.isUsable)
    options.darkBiasState = "removed"
    #expect(!options.isUsable)
    options.bias = "/bias.fits"
    #expect(options.isUsable)
    options.flat = "/flat.fits"
    #expect(!options.isUsable)
    options.flatBiasState = "included"
    #expect(options.isUsable)
    options.bias = options.flat
    #expect(!options.isUsable)
}
