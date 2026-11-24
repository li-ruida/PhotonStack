import Foundation
import CryptoKit
import PhotonStackProcessing

struct XisfExchangeOptions:Sendable {
    var input:URL
    var importing=true
    var image=0
    var compression="zlib"
    var isValid:Bool {
        input.isFileURL && (importing ? (0..<1024).contains(image) : ["none", "zlib"].contains(compression))
    }
    var arguments:[String] {
        [importing ? "xisf-import":"xisf-export","--input",input.path] +
        (importing ? ["--image",String(image)]:["--compression",compression])
    }
}
struct XisfExchangeProducts:Decodable,Sendable {
    var version,image,width,height,channels:Int
    var source,sourceSHA256,command,sampleFormat,product,productSHA256,metadataSHA256:String
    var pixelValuesPreserved,displayFunctionsApplied:Bool
    static func verify(at directory:URL,options:XisfExchangeOptions) async throws -> URL {
        let task=Task.detached(priority:.userInitiated) {
            let manifest=directory.appendingPathComponent("exchange.json")
            guard (try manifest.resourceValues(forKeys:[.fileSizeKey]).fileSize ?? Int.max)<=1024*1024 else {throw ProcessingServiceError.invalidReport(command:"xisf")}
            let value=try JSONDecoder().decode(Self.self,from:Data(contentsOf:manifest))
            guard value.version==1,value.command==(options.importing ? "xisf-import":"xisf-export"),
                  value.product==(options.importing ? "image.fits":"image.xisf"),
                  value.image==(options.importing ? options.image:0),value.width>0,value.height>0,[1,3,4].contains(value.channels),
                  ["UInt8","UInt16","UInt32","Float32","Float64"].contains(value.sampleFormat),
                  value.pixelValuesPreserved,!value.displayFunctionsApplied,
                  URL(fileURLWithPath:value.source).resolvingSymlinksInPath()==options.input.resolvingSymlinksInPath() else {throw ProcessingServiceError.invalidReport(command:"xisf")}
            for (url,expected) in [(options.input,value.sourceSHA256),(directory.appendingPathComponent(value.product),value.productSHA256),(directory.appendingPathComponent("metadata.xml"),value.metadataSHA256)] {
                try Task.checkCancellation()
                let info=try url.resourceValues(forKeys:[.isRegularFileKey,.isSymbolicLinkKey])
                guard info.isRegularFile==true,info.isSymbolicLink != true,expected.count==64 else {throw ProcessingServiceError.invalidReport(command:"xisf")}
                let handle=try FileHandle(forReadingFrom:url);defer{try? handle.close()};var hash=SHA256()
                while let data=try handle.read(upToCount:1024*1024),!data.isEmpty {try Task.checkCancellation();hash.update(data:data)}
                guard hash.finalize().map({String(format:"%02x",$0)}).joined()==expected else {throw ProcessingServiceError.invalidArguments("XISF source or product changed: \(url.lastPathComponent)")}
            }
            return directory.appendingPathComponent(value.product)
        }
        return try await withTaskCancellationHandler(operation:{try await task.value},onCancel:{task.cancel()})
    }
}
