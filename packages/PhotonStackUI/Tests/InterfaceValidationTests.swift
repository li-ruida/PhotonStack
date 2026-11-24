import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

@Test func xisfExportIgnoresHiddenImportIndex() {
    let input=URL(fileURLWithPath:"/tmp/source.fits")
    for index in [-1,1024,Int.max] {
        #expect(!XisfExchangeOptions(input:input,image:index).isValid)
        let options=XisfExchangeOptions(input:input,importing:false,image:index)
        #expect(options.isValid)
        #expect(!options.arguments.contains("--image"))
    }
    #expect(XisfExchangeOptions(input:input,image:1023).isValid)
    #expect(!XisfExchangeOptions(input:input,importing:false,compression:"lossy").isValid)
    #expect(!XisfExchangeOptions(input:URL(string:"https://example.com/source.xisf")!).isValid)
}

@Test func photometryExplainsPrerequisitesAndRejectsOverlappingRegions() {
    var config=TimeSeriesConfiguration()
    #expect(config.validationMessage(chinese:true) != nil)
    config.frames=(0..<3).map{URL(fileURLWithPath:"/tmp/frame\($0).fits")}
    config.stars=[.init(id:"target",x:48,y:48),.init(id:"c1",x:136,y:48),.init(id:"c2",x:48,y:136)]
    config.filter="Test"; config.gain=1.5; config.aligned=true
    #expect(config.isValid)
    #expect(config.validationMessage(chinese:true)==nil)
    config.filter="  "
    #expect(!config.isValid)
    config.filter=String(repeating:"滤",count:22)
    #expect(!config.isValid)
    config.filter="Test"
    // Native requires separation strictly greater than 2*(skyOuter+maxShift+1).
    config.stars[1].x=86
    #expect(!config.isValid)
    #expect(config.validationMessage(chinese:false)?.contains("overlap")==true)
    config.stars[1].x=86.01
    #expect(config.isValid)
    config.skyOuter=40
    #expect(!config.isValid)
    config.skyOuter=15;config.gain = .nan
    #expect(!config.isValid)
}

@Test func processingDiagnosticsSurviveLogPrefixesAndQuotedBraces() {
    let raw="""
    Starting registration...
    {"type":"progress","message":"Reading input"}
    {
      "type":"error",
      "code":"MosaicInsufficientOverlap",
      "message":"Cannot register file {test} and \\\"quoted\\\" path"
    }
    Closing worker.
    """
    let result=ProcessingFailureSummary.diagnostic(in:raw)
    #expect(result?.code=="MosaicInsufficientOverlap")
    #expect(result?.message.contains("{test}")==true)
    #expect(ProcessingFailureSummary.message(raw,chinese:true).contains("共同星点"))
    #expect(ProcessingFailureSummary.diagnostic(in:"{\"type\":\"progress\",\"code\":\"MemoryAllocationFailed\",\"message\":\"Not an error\"}")==nil)
    #expect(ProcessingFailureSummary.diagnostic(in:"{broken}")==nil)
    #expect(ProcessingFailureSummary.diagnostic(in:String(repeating:"x",count:65536)+raw)==nil)
    #expect(ProcessingFailureSummary.message("Output already exists: /tmp/image.fits",chinese:true).contains("另选位置"))
    #expect(ProcessingFailureSummary.message("请选择线性 FITS。",chinese:true)=="请选择线性 FITS。")
    #expect(ProcessingFailureSummary.message("An undocumented failure",chinese:true)=="操作未完成，请查看错误详情。")
}

@Test @MainActor func cancelledPhotometryPreviewDoesNotPublishAnError() async {
    let session=TimeSeriesSession()
    let task=Task { await session.choose([URL(fileURLWithPath:"/tmp/no-preview.fits")],using:UnavailableProcessingService()) }
    task.cancel()
    await task.value
    #expect(!session.busy)
    #expect(session.error==nil)
    #expect(session.image==nil)
}
