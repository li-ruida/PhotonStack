import AppKit
import Foundation
import Testing
import PhotonStackAppCore
import PhotonStackProcessing
@testable import PhotonStackUI

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func starLayerSessionRestoresRecipeAndRejectsChangedBases() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("StarLayerSession-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let source = root.appendingPathComponent("original.tiff"), removed = root.appendingPathComponent("removed.tiff")
    for (path, base) in [(source, 120), (removed, 80)] {
        let image = try #require(NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 64, pixelsHigh: 64,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
            bytesPerRow: 256, bitsPerPixel: 32))
        let bytes = try #require(image.bitmapData)
        for p in 0..<4096 {
            for c in 0..<3 { bytes[4*p+c] = UInt8(base + (p%3)*10) }
            bytes[4*p+3] = 255
        }
        try #require(image.representation(using: .tiff, properties: [:])).write(to: path)
    }
    let service = CLIProcessingService(executableURL: cli)
    let session = StarLayerSession()
    let directory = root.appendingPathComponent("layers")
    let original = try Data(contentsOf: source)
    _ = try await session.separate(input: source, starless: removed, output: directory, using: service)
    session.settings.starsEV = -1
    session.settings.nebulaGamma = 1.3
    session.settings.nebulaSaturation = 1.12
    _ = try await session.render(using: service)
    let expected = try Data(contentsOf: try #require(session.rendered).appendingPathComponent("combined.fits"))
    let reopened = StarLayerSession()
    try reopened.open(directory)
    #expect(reopened.settings == session.settings)
    _ = try await reopened.render(using: service)
    #expect(try Data(contentsOf: try #require(reopened.rendered).appendingPathComponent("combined.fits")) == expected)
    #expect(try Data(contentsOf: source) == original)
    let model = PhotonStackWorkspaceModel(autosaveEnabled: false, processingService: service, previewDirectory: root.appendingPathComponent("preview"))
    model.startStarLayerJob { _ in try #require(reopened.rendered).appendingPathComponent("combined.tiff") }
    let deadline = ContinuousClock.now + .seconds(5)
    repeat { try await Task.sleep(for: .milliseconds(25)) } while (model.jobs.isEmpty || model.hasActiveProcessing) && ContinuousClock.now < deadline
    #expect(model.errorMessage == nil)
    #expect(model.currentInputURL == reopened.rendered?.appendingPathComponent("combined.tiff"))
    #expect(model.canExportCurrentImage)
    let exported = root.appendingPathComponent("exported.tiff")
    await model.exportCurrentImage(to: exported)
    #expect(model.errorMessage == nil)
    #expect(FileManager.default.fileExists(atPath: exported.path))
    // Existing layer sessions must survive both invalid sources and same-output retries.
    do {
        _ = try await session.separate(input: source, starless: removed, output: directory, using: service)
        Issue.record("Existing directory must be preserved")
    } catch {}
    #expect(FileManager.default.fileExists(atPath: directory.appendingPathComponent("session.json").path))
    let changed = directory.appendingPathComponent("stars.fits")
    var data = try Data(contentsOf: changed); data[data.count-1] ^= 1
    try data.write(to: changed)
    do { _ = try await reopened.render(using: service); Issue.record("Expected changed-base rejection") } catch {}
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func linearStarLayerSessionPreservesSourceAndRejectsWrongToolDomain() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("LinearStarSession-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let source = root.appendingPathComponent("linear.fits"), removed = root.appendingPathComponent("removed.fits")
    let cards = [("SIMPLE","T"),("BITPIX","-32"),("NAXIS","2"),("NAXIS1","64"),("NAXIS2","64")]
    let header = (cards.map { ($0.0.padding(toLength: 8, withPad: " ", startingAt: 0)+"= "+$0.1).padding(toLength: 80, withPad: " ", startingAt: 0) }.joined()+"END").padding(toLength: 2880, withPad: " ", startingAt: 0)
    for (path, offset) in [(source, 0), (removed, -10)] {
        var data = Data(header.utf8)
        for i in 0..<4096 {
            var bits = (i == 0 ? Float.nan : Float(i - 100 + offset)).bitPattern.bigEndian
            withUnsafeBytes(of: &bits) { data.append(contentsOf: $0) }
        }
        data.append(Data(repeating: 0, count: (2880-data.count%2880)%2880)); try data.write(to: path)
    }
    let original = try Data(contentsOf: source)
    let service = CLIProcessingService(executableURL: cli)
    let session = StarLayerSession()
    _ = try await session.separate(input: source, starless: removed, output: root.appendingPathComponent("layers"), using: service)
    #expect(session.linear)
    #expect(try Data(contentsOf: try #require(session.rendered).appendingPathComponent("combined.fits")) == original)
    session.settings.starsEV = -1
    _ = try await session.render(using: service)
    let expected = try Data(contentsOf: try #require(session.rendered).appendingPathComponent("combined.fits"))
    let reopened = StarLayerSession()
    try reopened.open(try #require(session.directory))
    #expect(reopened.linear && reopened.settings == session.settings)
    _ = try await reopened.render(using: service)
    #expect(try Data(contentsOf: try #require(reopened.rendered).appendingPathComponent("combined.fits")) == expected)
    reopened.settings.nebulaGamma = 2
    do { _ = try await reopened.render(using: service); Issue.record("Gamma must not alter linear science") } catch {}
    // Copy is a domain-conversion fixture, not evidence of model quality.
    session.tool = ExternalImageTool(name: "Fixture copy", executable: URL(fileURLWithPath: "/bin/cp"),
                                     arguments: ["{input}", "{output}"], linearFITS: true)
    _ = try await session.separate(input: source, starless: nil, output: root.appendingPathComponent("external"), using: service)
    #expect(try Data(contentsOf: try #require(session.rendered).appendingPathComponent("combined.fits")) == original)
    let preview = try #require(session.rendered).appendingPathComponent("combined.tiff")
    do { _ = try await session.separate(input: preview, starless: nil, output: root.appendingPathComponent("wrong-display"), using: service); Issue.record("Linear tool must reject display input") } catch {}
    session.tool?.linearFITS = false
    do { _ = try await session.separate(input: source, starless: nil, output: root.appendingPathComponent("wrong-linear"), using: service); Issue.record("Display tool must reject linear input") } catch {}
    #expect(try Data(contentsOf: source) == original)
}
