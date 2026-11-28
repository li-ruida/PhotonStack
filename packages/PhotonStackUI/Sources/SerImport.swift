import Foundation
import CryptoKit
import PhotonStackProcessing

struct SerSequenceInfo: Decodable, Sendable {
    var type,command,source,byteOrder,bayer:String
    var width,height,frames,channels,colorID,depth,endianFlag,frameBytes:Int
    var littleEndian,timestamps:Bool
    var observer,instrument,telescope,localStartTicks,utcStartTicks:String
    var isValid:Bool {
        type=="complete" && command=="ser-inspect" && source.hasPrefix("/") &&
        width>0 && width<=32768 && height>0 && height<=32768 && width*height<=32*1024*1024 &&
        (1...10_000_000).contains(frames) && [1,3].contains(channels) && (1...16).contains(depth) &&
        [0,8,9,10,11,100,101].contains(colorID) && [0,1].contains(endianFlag) &&
        ["compatibility","specification","little","big"].contains(byteOrder) &&
        frameBytes==width*height*channels*(depth<=8 ? 1:2)
    }
}
struct SerImportOptions: Sendable {
    var input:URL
    var first=0,count=200,step=1
    var byteOrder="compatibility"
    func isValid(total:Int)->Bool {
        first>=0 && first<total && (1...10000).contains(count) && (1...10_000_000).contains(step) &&
        first+(count-1)*step<total && ["compatibility","specification","little","big"].contains(byteOrder)
    }
    var arguments:[String] { ["--input",input.path,"--first",String(first),"--count",String(count),"--step",String(step),"--byte-order",byteOrder] }
}
struct SerImportProducts: Decodable, Sendable {
    struct Frame:Decodable,Sendable {var index:Int;var file,sha256:String;var utcTicks:String?}
    var version:Int
    var source,sourceSHA256,byteOrder:String
    var selected:[Frame]
    static func verify(at directory:URL,options:SerImportOptions) async throws -> Int {
        let task=Task.detached(priority:.userInitiated) {
            let manifest=directory.appendingPathComponent("sequence.json")
            guard (try manifest.resourceValues(forKeys:[.fileSizeKey]).fileSize ?? Int.max)<=8*1024*1024 else {throw ProcessingServiceError.invalidReport(command:"ser-extract")}
            let value=try JSONDecoder().decode(Self.self,from:Data(contentsOf:manifest))
            guard value.version==1,value.selected.count==options.count,value.byteOrder==options.byteOrder,
                  URL(fileURLWithPath:value.source).resolvingSymlinksInPath()==options.input.resolvingSymlinksInPath(),
                  value.sourceSHA256.count==64 else {throw ProcessingServiceError.invalidReport(command:"ser-extract")}
            for (position,frame) in value.selected.enumerated() {
                try Task.checkCancellation()
                let index=options.first+position*options.step
                guard frame.index==index,frame.file==String(format:"frame-%08d.fits",index) else {throw ProcessingServiceError.invalidReport(command:"ser-extract")}
                let url=directory.appendingPathComponent(frame.file)
                let info=try url.resourceValues(forKeys:[.isRegularFileKey,.isSymbolicLinkKey])
                guard info.isRegularFile==true,info.isSymbolicLink != true else {throw ProcessingServiceError.invalidReport(command:"ser-extract")}
                let handle=try FileHandle(forReadingFrom:url);defer{try? handle.close()}
                var hash=SHA256()
                while let data=try handle.read(upToCount:1_048_576),!data.isEmpty {try Task.checkCancellation();hash.update(data:data)}
                guard hash.finalize().map({String(format:"%02x",$0)}).joined()==frame.sha256 else {throw ProcessingServiceError.invalidArguments("Extracted SER frame changed: \(frame.file)")}
            }
            return value.selected.count
        }
        return try await withTaskCancellationHandler(operation:{try await task.value},onCancel:{task.cancel()})
    }
}
