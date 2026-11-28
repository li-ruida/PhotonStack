import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

@Test(.enabled(if:ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func xisfExchangePreservesSamplesImportsAndChecksProvenance() async throws {
    let root=FileManager.default.temporaryDirectory.appendingPathComponent("XisfExchange-\(UUID())")
    try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true)
    defer{try? FileManager.default.removeItem(at:root)}
    let input=root.appendingPathComponent("source.xisf")
    let xml="<xisf version=\"1.0\"><Image geometry=\"32:32:1\" sampleFormat=\"UInt16\" colorSpace=\"Gray\" location=\"attachment:4096:2048\"><ColorFilterArray width=\"2\" height=\"2\" pattern=\"RGGB\"/></Image></xisf>"
    var data=Data("XISF0100".utf8)
    var size=UInt32(xml.utf8.count).littleEndian;withUnsafeBytes(of:&size){data.append(contentsOf:$0)}
    data.append(Data(repeating:0,count:4));data.append(Data(xml.utf8));data.append(Data(repeating:0,count:4096-data.count))
    for pixel in 0..<1024 {var value=UInt16(pixel*63).littleEndian;withUnsafeBytes(of:&value){data.append(contentsOf:$0)}}
    try data.write(to:input)
    let service=CLIProcessingService(executableURL:URL(fileURLWithPath:try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let model=PhotonStackWorkspaceModel(autosaveEnabled:false,processingService:service,previewDirectory:root.appendingPathComponent("preview"))
    let options=XisfExchangeOptions(input:input);let output=root.appendingPathComponent("imported")
    model.startXisfExchange(options,output:output)
    let deadline=ContinuousClock.now + .seconds(15)
    repeat{try await Task.sleep(for:.milliseconds(25))}while (model.jobs.isEmpty || model.hasActiveProcessing || model.project.assets.count != 1) && ContinuousClock.now<deadline
    #expect(model.errorMessage==nil);#expect(model.project.assets.count==1)
    let fits=try await XisfExchangeProducts.verify(at:output,options:options)
    #expect(fits.lastPathComponent=="image.fits");#expect(try Data(contentsOf:input)==data)
    let exportOptions=XisfExchangeOptions(input:fits,importing:false);let exported=root.appendingPathComponent("exported")
    _ = try await service.runCLI(arguments:exportOptions.arguments+["--output-dir",exported.path])
    let xisf=try await XisfExchangeProducts.verify(at:exported,options:exportOptions)
    #expect(xisf.lastPathComponent=="image.xisf")
    do {_ = try await service.runCLI(arguments:exportOptions.arguments+["--output-dir",exported.path]);Issue.record("Existing XISF destination replaced")}catch{}
    #expect(try await XisfExchangeProducts.verify(at:exported,options:exportOptions)==xisf)
    let xmlURL=output.appendingPathComponent("metadata.xml");try Data("changed".utf8).write(to:xmlURL)
    do {_ = try await XisfExchangeProducts.verify(at:output,options:options);Issue.record("Changed XISF metadata accepted")}catch{}
}
