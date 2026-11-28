import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

@Test(.enabled(if:ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func timeSeriesRecoversVariationReopensAndRejectsTamperedProducts() async throws {
    let root=FileManager.default.temporaryDirectory.appendingPathComponent("TimeSeries-\(UUID())")
    try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true)
    defer{try? FileManager.default.removeItem(at:root)}
    var frames:[URL]=[]
    for index in 0..<3 {
        let fields=[("SIMPLE","T"),("BITPIX","-32"),("NAXIS","2"),("NAXIS1","192"),("NAXIS2","192"),("PSCOLOR","'Linear'"),("BUNIT","'ADU'"),("DATE-OBS","'2024-02-29T12:0\(index):00'"),("EXPTIME","10"),("FILTER","'Test'")]
        var header=fields.map{($0.0.padding(toLength:8,withPad:" ",startingAt:0)+"= "+$0.1).padding(toLength:80,withPad:" ",startingAt:0)}.joined()+"END"
        header=header.padding(toLength:2880,withPad:" ",startingAt:0);var data=Data(header.utf8)
        for y in 0..<192 {for x in 0..<192 {
            var sample=100.0 + 0.01*sin(Double(x+y)*0.2)
            for (n,star) in [(48.0,48.0),(136.0,48.0),(48.0,136.0)].enumerated() {
                let amplitude=n==0 ? 500.0*(0.98+Double(index)*0.02):500.0
                sample += amplitude*exp(-(pow(Double(x)-star.0,2)+pow(Double(y)-star.1,2))/4.5)
            }
            var value=Float(sample).bitPattern.bigEndian;withUnsafeBytes(of:&value){data.append(contentsOf:$0)}
        }}
        data.append(Data(repeating:0,count:(2880-data.count%2880)%2880))
        let url=root.appendingPathComponent("frame\(index).fits");try data.write(to:url);frames.append(url)
    }
    let service=CLIProcessingService(executableURL:URL(fileURLWithPath:try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let session=TimeSeriesSession();await session.choose(frames,using:service)
    #expect(session.error==nil);#expect(session.image != nil);#expect(session.width==192)
    session.tap(x:48,y:48);session.tap(x:136,y:48);session.tap(x:48,y:136)
    session.configuration.filter="Test";session.configuration.gain=1.5;session.configuration.aligned=true
    #expect(session.configuration.isValid)
    let output=root.appendingPathComponent("measurement")
    try await session.run(output:output,using:service)
    let report=try #require(session.report);#expect(report.accepted==3)
    for (i,point) in report.points.enumerated(){#expect(abs((point.relativeFlux ?? 0)-(0.98+Double(i)*0.02))<0.0001)}
    let reopened=TimeSeriesSession();try await reopened.open(output,using:service)
    #expect(reopened.configuration==session.configuration);#expect(reopened.report?.accepted==3);#expect(reopened.image != nil)
    reopened.selection=0;reopened.tap(x:48.1,y:48)
    #expect(reopened.report==nil);#expect(reopened.configuration.stars[0].x==48.1)
    do {try await session.run(output:output,using:service);Issue.record("Existing photometry result replaced")}catch{}
    let curve=output.appendingPathComponent("lightcurve.csv");let original=try Data(contentsOf:curve)
    try Data("changed".utf8).write(to:curve)
    do {_ = try await TimeSeriesReport.load(output);Issue.record("Changed CSV accepted")}catch{}
    try original.write(to:curve)
    let reportURL=output.appendingPathComponent("report.json")
    var bytes=try Data(contentsOf:reportURL);bytes.append(32);try bytes.write(to:reportURL)
    do {_ = try await TimeSeriesReport.load(output);Issue.record("Changed report accepted")}catch{}
}
