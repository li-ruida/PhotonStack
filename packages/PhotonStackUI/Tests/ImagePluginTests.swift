import Foundation
import Testing
import CryptoKit
import PhotonStackProcessing
@testable import PhotonStackUI

@Test(.enabled(if:ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func imagePluginReviewRunAndChangedDefinitionAreValidated() async throws {
    let root=FileManager.default.temporaryDirectory.appendingPathComponent("ImagePlugin-\(UUID())")
    try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true)
    defer{try? FileManager.default.removeItem(at:root)}
    let input=root.appendingPathComponent("input.fits")
    let fields=[("SIMPLE","T"),("BITPIX","-32"),("NAXIS","2"),("NAXIS1","32"),("NAXIS2","32"),("PSCOLOR","'Linear'")]
    var header=fields.map{($0.0.padding(toLength:8,withPad:" ",startingAt:0)+"= "+$0.1).padding(toLength:80,withPad:" ",startingAt:0)}.joined()+"END"
    header=header.padding(toLength:2880,withPad:" ",startingAt:0);var data=Data(header.utf8)
    for n in 0..<1024 {var value=Float(n-100).bitPattern.bigEndian;withUnsafeBytes(of:&value){data.append(contentsOf:$0)}}
    data.append(Data(repeating:0,count:(2880-data.count%2880)%2880));try data.write(to:input)
    let executable=URL(fileURLWithPath:"/bin/cp")
    let sha=SHA256.hash(data:try Data(contentsOf:executable)).map{String(format:"%02x",$0)}.joined()
    let definition=root.appendingPathComponent("copy.psplugin")
    let text="""
    PhotonStack-ImagePlugin-1
    id "test.copy"
    name "Copy contract fixture"
    executable "/bin/cp"
    sha256 "\(sha)"
    domain "linear-fits"
    grid "preserve"
    units "preserve"
    timeout "10"
    argument "{input}"
    argument "{output}"

    """
    try text.write(to:definition,atomically:true,encoding:.utf8)
    let service=CLIProcessingService(executableURL:URL(fileURLWithPath:try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let session=ImagePluginSession();await session.inspect(definition,using:service)
    #expect(session.error==nil);#expect(session.info?.id=="test.copy")
    session.input=input;let output=root.appendingPathComponent("run")
    let result=try await session.run(output:output,using:service)
    #expect(try Data(contentsOf:result)==data);#expect(try Data(contentsOf:input)==data)
    do {_ = try await session.run(output:output,using:service);Issue.record("Plugin replaced an existing output")}catch{}
    try (text+"\n").write(to:definition,atomically:true,encoding:.utf8)
    do {_ = try await session.run(output:root.appendingPathComponent("changed"),using:service);Issue.record("Unreviewed plugin definition accepted")}catch{}
    #expect(!FileManager.default.fileExists(atPath:root.appendingPathComponent("changed").path))
}
