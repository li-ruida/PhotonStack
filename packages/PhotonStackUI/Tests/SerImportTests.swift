import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

private func serFixture(_ path:URL) throws {
    var data=Data(repeating:0,count:178)
    data.replaceSubrange(0..<14,with:Data("LUCAM-RECORDER".utf8))
    for (offset,value) in [(26,32),(30,32),(34,16),(38,4)] {
        var v=UInt32(value).littleEndian;withUnsafeBytes(of:&v){data.replaceSubrange(offset..<offset+4,with:$0)}
    }
    for frame in 0..<4 {for pixel in 0..<1024 {
        var v=UInt16(100+frame*10+pixel).littleEndian;withUnsafeBytes(of:&v){data.append(contentsOf:$0)}
    }}
    try data.write(to:path)
}
@Test(.enabled(if:ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func serSequenceImportsSelectedFramesAndVerifiesProducts() async throws {
    let root=FileManager.default.temporaryDirectory.appendingPathComponent("SerImport-\(UUID())")
    try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true)
    defer{try? FileManager.default.removeItem(at:root)}
    let input=root.appendingPathComponent("capture.ser");try serFixture(input);let original=try Data(contentsOf:input)
    let service=CLIProcessingService(executableURL:URL(fileURLWithPath:try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let model=PhotonStackWorkspaceModel(autosaveEnabled:false,processingService:service,previewDirectory:root.appendingPathComponent("preview"))
    let info=try await model.inspectSER(input,byteOrder:"compatibility")
    #expect(info.frames==4 && info.littleEndian && info.isValid && !info.timestamps)
    let options=SerImportOptions(input:input,first:1,count:2,step:2)
    let output=root.appendingPathComponent("extracted")
    model.startSERImport(options,output:output)
    let deadline=ContinuousClock.now + .seconds(15)
    repeat {try await Task.sleep(for:.milliseconds(25))} while (model.jobs.isEmpty || model.hasActiveProcessing || model.project.assets.count != 2) && ContinuousClock.now<deadline
    #expect(model.errorMessage==nil)
    #expect(Set(model.project.assets.map{$0.originalURL.lastPathComponent})==["frame-00000001.fits","frame-00000003.fits"])
    #expect(try Data(contentsOf:input)==original)
    #expect(try await SerImportProducts.verify(at:output,options:options)==2)
    do {_ = try await service.runCLI(arguments:["ser-extract"]+options.arguments+["--output-dir",output.path]);Issue.record("Existing SER destination replaced")}catch{}
    #expect(try await SerImportProducts.verify(at:output,options:options)==2)
    let frame=output.appendingPathComponent("frame-00000003.fits");var bytes=try Data(contentsOf:frame);bytes[bytes.count-1] ^= 1;try bytes.write(to:frame)
    do {_ = try await SerImportProducts.verify(at:output,options:options);Issue.record("Changed SER output accepted")}catch{}
}
