import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

private func psfFixture(_ path:URL) throws {
    let pairs=[("SIMPLE","T"),("BITPIX","-32"),("NAXIS","2"),("NAXIS1","256"),("NAXIS2","256")]
    let header=(pairs.map {($0.0.padding(toLength:8,withPad:" ",startingAt:0)+"= "+$0.1).padding(toLength:80,withPad:" ",startingAt:0)}.joined()+"END").padding(toLength:2880,withPad:" ",startingAt:0)
    var data=Data(header.utf8)
    for y in 0..<256 {for x in 0..<256 {
        var value=20.0
        for sy in stride(from:28,to:256,by:48) {for sx in stride(from:28,to:256,by:48) {
            let dx=Double(x)-Double(sx)-0.3,dy=Double(y)-Double(sy)-0.2
            if abs(dx)<12 && abs(dy)<12 {value += 500*exp(-0.5*(dx*dx/(1.7*1.7)+dy*dy/(1.3*1.3)))}
        }}
        var bits=Float(value).bitPattern.bigEndian;withUnsafeBytes(of:&bits){data.append(contentsOf:$0)}
    }}
    data.append(Data(repeating:0,count:(2880-data.count%2880)%2880));try data.write(to:path)
}
@Test(.enabled(if:ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func measuredPsfCanBeReviewedExcludedReopenedAndNeverOverwritesSources() async throws {
    let service=CLIProcessingService(executableURL:URL(fileURLWithPath:try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let root=FileManager.default.temporaryDirectory.appendingPathComponent("PsfMeasurement-\(UUID())",isDirectory:true)
    try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true);defer {try? FileManager.default.removeItem(at:root)}
    let source=root.appendingPathComponent("source.fits");try psfFixture(source);let original=try Data(contentsOf:source)
    let session=PsfMeasurementSession();session.choose(source);#expect(session.canMeasure)
    let output=root.appendingPathComponent("measurement");let preview=try await session.measure(output:output,using:service)
    #expect(preview==output.appendingPathComponent("stars-preview.tiff"))
    let report=try #require(session.report);#expect(report.accepted==25)
    #expect(abs(report.majorFwhm-2.354820045*1.7)<1e-4)
    let id=try #require(report.stars.first(where:{$0.status=="accepted"})?.id)
    session.excluded.insert(id)
    let second=root.appendingPathComponent("excluded");_ = try await session.measure(output:second,using:service)
    #expect(session.report?.accepted==24)
    #expect(session.report?.stars[id].status=="excluded")
    let fresh=PsfMeasurementSession();_ = try await fresh.open(second,using:service)
    #expect(fresh.excluded==[id] && fresh.canMeasure && fresh.report?.accepted==24)
    let kernel=try await fresh.verifiedOutput("psf.fits",using:service);let bytes=try Data(contentsOf:kernel)
    do {_ = try await fresh.measure(output:second,using:service);Issue.record("Replaced existing PSF output")}catch{}
    #expect(try Data(contentsOf:kernel)==bytes)
    fresh.sigma=6;#expect(fresh.excluded.isEmpty && !fresh.canEditExclusions)
    fresh.excluded.insert(id);#expect(!fresh.canMeasure);fresh.excluded=[]
    #expect(try Data(contentsOf:source)==original)
    var changed=original;changed[changed.count-1] ^= 1;try changed.write(to:source)
    do {_ = try await fresh.measure(output:root.appendingPathComponent("changed"),using:service);Issue.record("Changed PSF source accepted")}catch{}
    #expect(!FileManager.default.fileExists(atPath:root.appendingPathComponent("changed").path))
}

@Test(.enabled(if:ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func measuredPsfRestorationPreservesSourcesAndRestoresSavedSettings() async throws {
    let service=CLIProcessingService(executableURL:URL(fileURLWithPath:try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let root=FileManager.default.temporaryDirectory.appendingPathComponent("PsfRestoration-\(UUID())",isDirectory:true)
    try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true);defer {try? FileManager.default.removeItem(at:root)}
    let source=root.appendingPathComponent("source.fits");try psfFixture(source);let before=try Data(contentsOf:source)
    let session=PsfMeasurementSession();#expect(!session.canRestore);session.choose(source)
    let measured=root.appendingPathComponent("measurement");_ = try await session.measure(output:measured,using:service)
    #expect(session.canRestore)
    session.excluded=[0];#expect(!session.canRestore);session.excluded=[]
    session.sigma=6;#expect(!session.canRestore);session.sigma=5
    session.restoreIterations=5;session.restoreAmount=0.6;session.restoreNoise=2;session.restoreProtect=300
    let directory=root.appendingPathComponent("restored");let output=try await session.restore(output:directory,using:service)
    #expect(output.lastPathComponent=="restored.fits")
    let report=try #require(session.restoration);#expect(report.iterations==5 && report.protectedPixels>0)
    let reopened=PsfMeasurementSession();_ = try await reopened.openRestoration(directory,using:service)
    #expect(reopened.restoreIterations==5 && reopened.restoreAmount==0.6 && reopened.restoreNoise==2 && reopened.restoreProtect==300)
    #expect(reopened.canRestore && URL(fileURLWithPath:reopened.report?.directory ?? "").resolvingSymlinksInPath()==measured.resolvingSymlinksInPath())
    let residual=try await reopened.restoredOutput("residual.fits",using:service);#expect(FileManager.default.fileExists(atPath:residual.path))
    do {_ = try await reopened.restore(output:directory,using:service);Issue.record("Overwrote restoration")}catch{}
    #expect(try Data(contentsOf:source)==before)
    reopened.restoreAmount=Double.nan;#expect(!reopened.canRestore)
    reopened.choose(source);#expect(reopened.restoration==nil && !reopened.canRestore)
    var changed=try Data(contentsOf:residual);changed[changed.count-1] ^= 1;try changed.write(to:residual)
    do {_ = try await session.restoredOutput("restored.fits",using:service);Issue.record("Changed restoration product accepted")}catch{}
}
