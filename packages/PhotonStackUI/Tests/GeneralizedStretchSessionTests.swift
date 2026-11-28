import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func ghsPreviewSaveReopenAndSourceGuards() async throws {
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("GhsTest-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let source = root.appendingPathComponent("linear.fits")
    let cards = [("SIMPLE","T"),("BITPIX","-32"),("NAXIS","2"),("NAXIS1","64"),("NAXIS2","64")]
    let header = (cards.map { ($0.0.padding(toLength: 8, withPad: " ", startingAt: 0)+"= "+$0.1).padding(toLength: 80, withPad: " ", startingAt: 0) }.joined()+"END").padding(toLength: 2880, withPad: " ", startingAt: 0)
    var data = Data(header.utf8)
    for i in 0..<4096 { var bits = Float(i-100).bitPattern.bigEndian; withUnsafeBytes(of: &bits) { data.append(contentsOf: $0) } }
    data.append(Data(repeating: 0, count: (2880-data.count%2880)%2880)); try data.write(to: source)
    let session = GeneralizedStretchSession(source: source, service: service); defer { session.close() }
    try await session.updatePreview()
    #expect(session.isCurrent && session.image != nil)
    #expect(session.settings.black == -100 && session.settings.white == 3995 && !session.settings.automaticRange)
    #expect(session.report?.sourceDomain == "linear")
    session.settings.logStretch = 5; #expect(!session.isCurrent)
    let saved = root.appendingPathComponent("saved")
    do { _ = try await session.saveResult(to: saved); Issue.record("Saved stale preview") } catch { }
    #expect(!FileManager.default.fileExists(atPath: saved.path))
    try await session.updatePreview(); let image = try await session.saveResult(to: saved)
    #expect(image == saved.appendingPathComponent("developed.tiff"))
    let originalResult = try Data(contentsOf: saved.appendingPathComponent("display.fits"))
    do { _ = try await session.saveResult(to: saved); Issue.record("Replaced existing GHS result") } catch { }
    #expect(try Data(contentsOf: saved.appendingPathComponent("display.fits")) == originalResult)
    let reopened = GeneralizedStretchSession(source: source, service: service); defer { reopened.close() }
    try await reopened.open(saved)
    #expect(reopened.isCurrent && reopened.settings == session.settings && reopened.image != nil)
    reopened.before = true; #expect(reopened.image != nil)
    _ = try await reopened.saveResult(to: root.appendingPathComponent("again"))
    #expect(try Data(contentsOf: root.appendingPathComponent("again/display.fits")) == originalResult)
    // App multi-step preview/save/reopen must replay from the original Float32
    // source without depending on a temporary preview or quantized TIFF.
    reopened.appendStep()
    #expect(!reopened.isCurrent && reopened.prefixSteps == 1 && reopened.settings.black == 0 && reopened.settings.white == 1)
    reopened.settings.inverse = true; reopened.settings.logStretch = 2; reopened.settings.mode = "saturation"
    try await reopened.updatePreview()
    #expect(reopened.isCurrent && reopened.report?.steps == 2 && reopened.report?.inverse == true && reopened.report?.saturation == true)
    let chain = root.appendingPathComponent("chain")
    _ = try await reopened.saveResult(to: chain)
    let chainedPixels = try Data(contentsOf: chain.appendingPathComponent("display.fits"))
    let restored = GeneralizedStretchSession(source: source, service: service); defer { restored.close() }
    try await restored.open(chain)
    #expect(restored.isCurrent && restored.prefixSteps == 1 && restored.settings.mode == "saturation" && restored.settings.inverse)
    _ = try await restored.saveResult(to: root.appendingPathComponent("chain-replay"))
    #expect(try Data(contentsOf: root.appendingPathComponent("chain-replay/display.fits")) == chainedPixels)
    restored.settings.logStretch = 1; try await restored.updatePreview()
    #expect(restored.report?.steps == 2) // Editing the final step must not append it twice.
    #expect(try Data(contentsOf: source) == data)
    data[data.count-1] ^= 1; try data.write(to: source)
    do { try await reopened.updatePreview(); Issue.record("Changed source accepted") } catch { }
    do { _ = try await reopened.saveResult(to: root.appendingPathComponent("changed")); Issue.record("Changed source saved") } catch { }
    #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("changed").path))
}
