import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

private func registrationFixture(_ path: URL, shift: Double) throws {
    let pairs = [("SIMPLE","T"),("BITPIX","-32"),("NAXIS","2"),("NAXIS1","12"),("NAXIS2","10"),
                 ("CTYPE1","'RA---TAN'"),("CTYPE2","'DEC--TAN'"),("CRVAL1","313.5"),("CRVAL2","30"),
                 ("CRPIX1",String(6.5+shift)),("CRPIX2","5.5"),("CD1_1","-0.001"),("CD1_2","0"),("CD2_1","0"),("CD2_2","0.001")]
    let header=(pairs.map { ($0.0.padding(toLength: 8, withPad: " ", startingAt: 0)+"= "+$0.1).padding(toLength: 80, withPad: " ", startingAt: 0) }.joined()+"END").padding(toLength: 2880, withPad: " ", startingAt: 0)
    var data=Data(header.utf8)
    for y in 0..<10 { for x in 0..<12 {
        var word=Float(Double(x)-shift+Double(y)-5).bitPattern.bigEndian
        withUnsafeBytes(of: &word) { data.append(contentsOf: $0) }
    }}
    data.append(Data(repeating: 0, count: (2880-data.count%2880)%2880));try data.write(to:path)
}
@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func masterRegistrationRestoresReferenceGridAndFeedsCompositionWithoutOverwritingResults() async throws {
    let cli=URL(fileURLWithPath:try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let service=CLIProcessingService(executableURL:cli)
    let root=FileManager.default.temporaryDirectory.appendingPathComponent("RegisteredMasters-\(UUID())",isDirectory:true)
    try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true)
    defer { try? FileManager.default.removeItem(at:root) }
    let ha=root.appendingPathComponent("Ha.fits"),o=root.appendingPathComponent("OIII.fits")
    try registrationFixture(ha,shift:0);try registrationFixture(o,shift:2)
    let original=try Data(contentsOf:o)
    let composition=ChannelCompositionSession();composition.expressions=["Ha","0.3*Ha+0.7*OIII","OIII"]
    composition.masters=[ChannelMaster(name:"Ha",path:ha.path),ChannelMaster(name:"OIII",path:o.path)]
    let session=composition.registration;session.mode = .wcs;session.reference="Ha"
    let output=root.appendingPathComponent("registered")
    let primary=try await session.register(composition,output:output,using:service)
    #expect(primary==output.appendingPathComponent("Ha.fits"))
    #expect(composition.canCompose && composition.expressions[1]=="0.3*Ha+0.7*OIII")
    #expect(composition.masters[1].path==output.appendingPathComponent("OIII.fits").path)
    #expect(session.report?.masters[1].validFraction == 10.0/12)
    #expect(session.report?.masters[1].checkStars == 0)
    let saved=try Data(contentsOf:output.appendingPathComponent("OIII.fits"))
    let mixed=try await composition.compose(output:root.appendingPathComponent("mixed"),using:service)
    #expect(FileManager.default.fileExists(atPath:mixed.path))
    let fresh=ChannelCompositionSession()
    _ = try await fresh.registration.open(output,into:fresh,using:service)
    #expect(fresh.registration.mode == .wcs && fresh.registration.reference=="Ha")
    #expect(fresh.canCompose)
    _ = try await fresh.registration.replay(output:root.appendingPathComponent("replayed"),into:fresh,using:service)
    #expect(try Data(contentsOf:root.appendingPathComponent("replayed/OIII.fits"))==saved)
    do { _ = try await fresh.registration.replay(output:output,into:fresh,using:service);Issue.record("Registration replaced an existing result") } catch {}
    #expect(try Data(contentsOf:output.appendingPathComponent("OIII.fits"))==saved)
    #expect(try Data(contentsOf:o)==original)
    var changed=original;changed[changed.count-1] ^= 1;try changed.write(to:o)
    do { _ = try await fresh.registration.open(output,into:fresh,using:service);Issue.record("Changed registration source accepted") } catch {}
}
