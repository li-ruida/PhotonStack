import Foundation
import CryptoKit
import PhotonStackProcessing

struct ImagePluginInfo:Decodable,Sendable {
    var type,command,id,name,executable,executableSHA256,definitionSHA256,domain,grid,units:String
    var timeout:Int
    var arguments:[String]
    var isValid:Bool {
        type=="complete" && command=="plugin-inspect" && !id.isEmpty && !name.isEmpty && executable.hasPrefix("/") &&
        executableSHA256.count==64 && definitionSHA256.count==64 && domain=="linear-fits" && grid=="preserve" && units=="preserve" &&
        (1...600).contains(timeout) && arguments.count<=64 && arguments.filter{$0=="{input}"}.count==1 && arguments.filter{$0=="{output}"}.count==1
    }
}
@MainActor final class ImagePluginSession:ObservableObject {
    @Published private(set) var definition:URL?
    @Published private(set) var info:ImagePluginInfo?
    @Published var input:URL?
    @Published var inspecting=false
    @Published var error:String?
    func inspect(_ url:URL,using service:any ProcessingService) async {
        guard !inspecting else{return}
        inspecting=true;defer{inspecting=false};definition=nil;info=nil;error=nil
        do {
            let report=try await service.runCLI(arguments:["plugin-inspect","--definition",url.path])
            let value=try JSONDecoder().decode(ImagePluginInfo.self,from:Data(report.standardOutput.utf8))
            guard value.isValid else {throw ProcessingServiceError.invalidReport(command:"plugin-inspect")}
            try Task.checkCancellation();definition=url;info=value
        }catch {self.error=error.localizedDescription}
    }
    func run(output:URL,using service:any ProcessingService) async throws -> URL {
        guard let definition,let info,let input else {throw ProcessingServiceError.invalidArguments("Choose a reviewed image plugin and linear FITS source.")}
        _ = try await service.runCLI(arguments:["plugin-run","--definition",definition.path,"--definition-sha256",info.definitionSHA256,"--input",input.path,"--output-dir",output.path])
        let task=Task.detached(priority:.userInitiated) {
            struct Manifest:Decodable {
                var version:Int
                var source,sourceSHA256,definitionSHA256,resultSHA256,executableSHA256,id,domain,grid,units:String
                var sameGridAndUnitsDeclared,coveragePreserved,wcsPreserved:Bool
            }
            let manifest=output.appendingPathComponent("plugin.json")
            guard (try manifest.resourceValues(forKeys:[.fileSizeKey]).fileSize ?? Int.max)<65536 else {throw ProcessingServiceError.invalidReport(command:"plugin-run")}
            let report=try JSONDecoder().decode(Manifest.self,from:Data(contentsOf:manifest))
            guard report.version==1,report.id==info.id,report.domain==info.domain,report.grid==info.grid,report.units==info.units,
                  report.executableSHA256==info.executableSHA256,report.definitionSHA256==info.definitionSHA256,
                  report.sameGridAndUnitsDeclared,report.coveragePreserved,report.wcsPreserved,
                  URL(fileURLWithPath:report.source).resolvingSymlinksInPath()==input.resolvingSymlinksInPath() else {throw ProcessingServiceError.invalidReport(command:"plugin-run")}
            for (url,sha) in [(input,report.sourceSHA256),(output.appendingPathComponent("input.fits"),report.sourceSHA256),(output.appendingPathComponent("definition.psplugin"),report.definitionSHA256),(output.appendingPathComponent("result.fits"),report.resultSHA256)] {
                let values=try url.resourceValues(forKeys:[.isRegularFileKey,.isSymbolicLinkKey]);guard values.isRegularFile==true,values.isSymbolicLink != true,sha.count==64 else {throw ProcessingServiceError.invalidReport(command:"plugin-run")}
                let handle=try FileHandle(forReadingFrom:url);defer{try? handle.close()};var hash=SHA256()
                while let data=try handle.read(upToCount:1024*1024),!data.isEmpty {try Task.checkCancellation();hash.update(data:data)}
                guard hash.finalize().map({String(format:"%02x",$0)}).joined()==sha else {throw ProcessingServiceError.invalidArguments("Plugin source or product changed.")}
            }
            return output.appendingPathComponent("result.fits")
        }
        return try await withTaskCancellationHandler(operation:{try await task.value},onCancel:{task.cancel()})
    }
}
