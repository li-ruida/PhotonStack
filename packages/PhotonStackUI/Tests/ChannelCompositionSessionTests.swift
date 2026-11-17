import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

private func channelFixture(_ path: URL, base: Float) throws {
    let pairs = [("SIMPLE", "T"), ("BITPIX", "-32"), ("NAXIS", "2"), ("NAXIS1", "8"), ("NAXIS2", "6")]
    let header = (pairs.map { ($0.0.padding(toLength: 8, withPad: " ", startingAt: 0) + "= " + $0.1).padding(toLength: 80, withPad: " ", startingAt: 0) }.joined() + "END").padding(toLength: 2880, withPad: " ", startingAt: 0)
    var data = Data(header.utf8)
    for i in 0..<48 {
        var word = (base + Float(i)).bitPattern.bigEndian
        withUnsafeBytes(of: &word) { data.append(contentsOf: $0) }
    }
    data.append(Data(repeating: 0, count: (2880 - data.count % 2880) % 2880))
    try data.write(to: path)
}
@Test @MainActor
func channelPresetsRetainChosenMastersAndRequireRegistrationConfirmation() throws {
    let session = ChannelCompositionSession()
    #expect(!session.canCompose)
    session.masters[0].path = "/Ha.fits"; session.masters[1].path = "/OIII.fits"
    #expect(!session.canCompose)
    session.aligned = true; #expect(session.canCompose)
    session.apply(.sho)
    #expect(session.masters.map(\.name) == ["SII", "Ha", "OIII"])
    #expect(session.masters[1].path == "/Ha.fits" && session.masters[2].path == "/OIII.fits")
    #expect(!session.canCompose)
    session.masters[0].path = "/SII.fits"
    let args = try session.arguments(output: URL(fileURLWithPath: "/result"))
    #expect(args.contains("SII=/SII.fits") && args.contains("--aligned"))
    session.apply(.lrgb)
    #expect(session.masters.map(\.name) == ["L", "R", "G", "B"])
    #expect(session.expressions[0] == "L*R/(0.2126*R+0.7152*G+0.0722*B)")
    session.apply(.custom)
    #expect(session.masters.count == 4)
    session.newComposition(); #expect(!session.aligned)
}
@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func channelCompositionSavesRestoresAndReplaysMappingsWithoutChangingOriginals() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let service = CLIProcessingService(executableURL: cli)
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("Channels-\(UUID())", isDirectory: true)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let ha = root.appendingPathComponent("Ha with spaces.fits"), oxygen = root.appendingPathComponent("OIII.fits")
    try channelFixture(ha, base: -15); try channelFixture(oxygen, base: 20)
    let original = try Data(contentsOf: ha)
    let session = ChannelCompositionSession()
    session.masters = [ChannelMaster(name: "Ha", path: ha.path), ChannelMaster(name: "OIII", path: oxygen.path)]
    session.expressions = ["Ha", "0.25*Ha+0.75*OIII", "OIII"]
    session.aligned = true
    let folder = root.appendingPathComponent("result"), repeatFolder = root.appendingPathComponent("repeat")
    let output = try await session.compose(output: folder, using: service)
    #expect(output.lastPathComponent == "RGB.fits")
    #expect(session.lastRun?.invalidPixels == 0)
    let data = try Data(contentsOf: output)
    let restored = ChannelCompositionSession()
    _ = try await restored.open(folder, using: service)
    #expect(restored.expressions == session.expressions)
    #expect(restored.masters.map(\.name) == ["Ha", "OIII"] && restored.aligned)
    let replay = try await restored.compose(output: repeatFolder, using: service)
    #expect(try Data(contentsOf: replay) == data)
    restored.expressions[1] = "0.5*Ha+0.5*OIII"
    _ = try await restored.verifiedOutput("validity.fits", using: service)
    #expect(restored.expressions[1] == "0.5*Ha+0.5*OIII")
    let changed = try await restored.compose(output: root.appendingPathComponent("changed"), using: service)
    #expect(try Data(contentsOf: changed) != data)
    #expect(try Data(contentsOf: ha) == original)
    do { _ = try await restored.compose(output: folder, using: service); Issue.record("Existing result replaced") } catch {}
    #expect(try Data(contentsOf: output) == data)
    var corrupt = data; corrupt[corrupt.count-1] ^= 1; try corrupt.write(to: changed)
    do { _ = try await restored.verifiedOutput("RGB.fits", using: service); Issue.record("Changed result accepted") } catch {}
    try data.write(to: changed)
    var sourceChanged = original; sourceChanged[sourceChanged.count-1] ^= 1; try sourceChanged.write(to: ha)
    do { _ = try await session.compose(output: root.appendingPathComponent("bad-source"), using: service); Issue.record("Changed source accepted") } catch {}
}
