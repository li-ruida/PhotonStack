import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

private func intensityFixture(_ path: URL, gain: Float, offset: Float) throws {
    let pairs=[("SIMPLE","T"),("BITPIX","-32"),("NAXIS","2"),("NAXIS1","64"),("NAXIS2","64")]
    let header=(pairs.map { ($0.0.padding(toLength:8,withPad:" ",startingAt:0)+"= "+$0.1).padding(toLength:80,withPad:" ",startingAt:0) }.joined()+"END").padding(toLength:2880,withPad:" ",startingAt:0)
    var data=Data(header.utf8)
    for y in 0..<64 { for x in 0..<64 {
        var bits=(gain*(Float(x)+Float(y)*0.25-32)+offset).bitPattern.bigEndian
        withUnsafeBytes(of:&bits) {data.append(contentsOf:$0)}
    }}
    data.append(Data(repeating:0,count:(2880-data.count%2880)%2880));try data.write(to:path)
}
@Test(.enabled(if:ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func intensityMatchingPreservesCompositionAndRestoresVerifiedRecipes() async throws {
    let service=CLIProcessingService(executableURL:URL(fileURLWithPath:try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let root=FileManager.default.temporaryDirectory.appendingPathComponent("IntensityMatching-\(UUID())",isDirectory:true)
    try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true)
    defer {try? FileManager.default.removeItem(at:root)}
    let ha=root.appendingPathComponent("Ha.fits"),o=root.appendingPathComponent("OIII.fits")
    try intensityFixture(ha,gain:2,offset:7);try intensityFixture(o,gain:1,offset:0)
    let original=try Data(contentsOf:o)
    let composition=ChannelCompositionSession();composition.masters=[ChannelMaster(name:"Ha",path:ha.path),ChannelMaster(name:"OIII",path:o.path),ChannelMaster(name:"L",path:ha.path)]
    composition.expressions=["Ha","0.25*Ha+0.75*OIII","L"]
    let session=composition.intensity
    #expect(!session.canMatch(composition));composition.aligned=true;#expect(session.canMatch(composition))
    _ = try await composition.compose(output:root.appendingPathComponent("original-composition"),using:service)
    var tampered=original;tampered[tampered.count-1] ^= 1;try tampered.write(to:o)
    do {
        _ = try await session.match(composition,output:root.appendingPathComponent("changed-source"),using:service)
        Issue.record("Intensity matching bypassed the opened composition source hash")
    } catch {}
    #expect(!FileManager.default.fileExists(atPath:root.appendingPathComponent("changed-source").path))
    try original.write(to:o)
    let output=root.appendingPathComponent("matched")
    let image=try await session.match(composition,output:output,using:service)
    #expect(image==output.appendingPathComponent("matched.fits"))
    #expect(composition.masters.count==3 && composition.masters[2].name=="L")
    #expect(composition.expressions==["Ha","0.25*Ha+0.75*OIII","L"] && composition.canCompose)
    #expect(composition.masters[1].path==image.path)
    let report=try #require(session.report);#expect(abs(report.gain-2)<1e-10 && abs(report.offset-7)<1e-10)
    #expect(report.checkSamples>=64 && report.afterRms<1e-8)
    _ = try await composition.compose(output:root.appendingPathComponent("rgb"),using:service)
    let fresh=ChannelCompositionSession();let reopened=fresh.intensity
    _ = try await reopened.open(output,using:service)
    #expect(!fresh.aligned) // Reviewing an old match does not silently replace current masters.
    _ = try await reopened.restorePair(into:fresh,using:service)
    #expect(fresh.masters.count==2 && fresh.canCompose && fresh.expressions==["Ha","OIII","OIII"])
    let saved=try Data(contentsOf:image)
    _ = try await reopened.replay(output:root.appendingPathComponent("replay"),using:service)
    #expect(try Data(contentsOf:root.appendingPathComponent("replay/matched.fits"))==saved)
    do {_ = try await reopened.replay(output:output,using:service);Issue.record("Existing intensity product overwritten")}catch{}
    #expect(try Data(contentsOf:image)==saved && Data(contentsOf:o)==original)
    var changed=original;changed[changed.count-1] ^= 1;try changed.write(to:o)
    do {_ = try await reopened.open(output,using:service);Issue.record("Changed intensity source accepted")}catch{}
}
