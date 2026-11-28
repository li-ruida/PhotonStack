import AppKit
import Foundation
import CryptoKit
import PhotonStackProcessing

struct TimeSeriesStar:Codable,Sendable,Identifiable,Equatable {
    var id:String
    var x,y:Double
}
struct TimeSeriesConfiguration:Codable,Sendable,Equatable {
    var version=1
    var frames:[URL]=[]
    var stars:[TimeSeriesStar]=[]
    var channel="mono"
    var filter=""
    var gain=0.0
    var radius=8.0
    var skyInner=11.0
    var skyOuter=15.0
    var saturation=0.0
    var minSNR=5.0
    var maxShift=3.0
    var aligned=false
    var isValid:Bool { validationMessage(chinese:false) == nil }
    func validationMessage(chinese:Bool)->String? {
        func t(_ zh:String,_ en:String)->String {chinese ? zh:en}
        guard version==1 else {return t("不支持此测光设置版本。","Unsupported photometry settings version.")}
        guard (3...10000).contains(frames.count), Set(frames).count==frames.count,
              frames.allSatisfy({$0.isFileURL && $0.path.hasPrefix("/")}) else {
            return t("请选择 3–10000 张不同的 FITS。","Choose 3–10000 distinct FITS files.")
        }
        guard (3...17).contains(stars.count), Set(stars.map(\.id)).count==stars.count, stars.first?.id=="target",
              stars.allSatisfy({!$0.id.isEmpty && $0.id.utf8.allSatisfy{(48...57).contains($0)||(65...90).contains($0)||(97...122).contains($0)||$0==45||$0==95} && $0.x.isFinite && $0.y.isFinite && $0.x>=0 && $0.y>=0}) else {
            return t("请在图上选择目标星和至少两颗比较星。","Select a target and at least two comparison stars.")
        }
        guard aligned else {return t("请确认照片已对齐，并使用相同滤镜。","Confirm the frames are aligned and use the same filter.")}
        guard ["mono","red","green","blue"].contains(channel) else {return t("请选择测量通道。","Choose a measurement channel.")}
        guard !filter.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty, filter.utf8.count<=64,
              !filter.contains("\n"), !filter.contains("\r"), !filter.contains("\0") else {return t("请填写滤镜名称（最多 64 字节，不可换行）。","Enter a filter name (up to 64 UTF-8 bytes, no line breaks).")}
        guard gain.isFinite, gain>0, gain<=1e9 else {return t("有效增益须大于 0，最多 10⁹。","Effective gain must be greater than 0 and at most 10⁹.")}
        guard radius.isFinite, (3...20).contains(radius), skyInner.isFinite, skyInner>radius+1,
              skyOuter.isFinite, skyOuter>=skyInner+2, skyOuter<=40 else {
            return t("孔径半径须为 3–20 px；背景内半径须大于孔径半径 + 1，外半径至少比内半径大 2，且不超过 40 px。","Aperture: 3–20 px. Sky inner radius must exceed aperture + 1; outer radius must be at least inner + 2 and at most 40 px.")
        }
        guard saturation.isFinite, saturation>=0, minSNR.isFinite, minSNR>=1,
              maxShift.isFinite, maxShift>0, maxShift<=5 else {
            return t("饱和值须 ≥ 0，信噪比须 ≥ 1，质心偏移须大于 0 且不超过 5 px。","Saturation must be ≥ 0, SNR ≥ 1, and center shift greater than 0 and at most 5 px.")
        }
        let separation=2*(skyOuter+maxShift+1)
        for i in stars.indices {
            for j in stars.indices where j>i {
                if hypot(stars[i].x-stars[j].x,stars[i].y-stars[j].y)<=separation {
                    return t("星点测量区域重叠，请选择距离更远的比较星，或减小背景半径。","Star measurement regions overlap. Choose more separated stars or reduce the sky radius.")
                }
            }
        }
        return nil
    }
    func recipe() throws -> String {
        guard isValid else {throw ProcessingServiceError.invalidArguments("Choose at least 3 frames, a target, two comparisons, filter, effective gain and valid apertures; confirm the common grid.")}
        func quote(_ value:String)throws->String {
            guard !value.contains("\n"),!value.contains("\r"),!value.contains("\0") else {throw ProcessingServiceError.invalidArguments("Recipe values cannot contain line breaks.")}
            return "\""+value.replacingOccurrences(of:"\\",with:"\\\\").replacingOccurrences(of:"\"",with:"\\\"")+"\""
        }
        var text="PhotonStack-TimeSeries-1\naligned \"true\"\n"
        for (name,value) in [("filter",filter),("channel",channel),("effective-gain",String(gain)),("aperture",String(radius)),("sky-inner",String(skyInner)),("sky-outer",String(skyOuter)),("saturation",String(saturation)),("min-snr",String(minSNR)),("max-shift",String(maxShift))] {text += name+" "+(try quote(value))+"\n"}
        for (i,star) in stars.enumerated(){text += (i==0 ? "target":"comparison")+" "+(try quote(star.id))+" \(star.x) \(star.y)\n"}
        for frame in frames {text += "frame "+(try quote(frame.path))+"\n"}
        return text
    }
}
struct TimeSeriesReport:Decodable,Sendable {
    struct Source:Decodable,Sendable {var path,sha256:String;var explicitTiming:Bool}
    struct Point:Decodable,Sendable,Identifiable {var index:Int;var status,detail:String;var jdUTC,relativeFlux,relativeError:Double?;var id:Int{index}}
    struct Comparison:Decodable,Sendable,Identifiable {var id:String;var samples:Int;var leaveOneOutRelativeRMS:Double?}
    var version,frames,accepted:Int
    var recipeSHA256,channel,filter,timing,uncertainty:String
    var effectiveGain:Double
    var normalization:Double?
    var saturationKnown:Bool
    var sources:[Source]
    var points:[Point]
    var comparisons:[Comparison]
    var products:[String:String]
    var limitations:[String]
    var isValid:Bool {
        version==1 && frames>=3 && frames<=10000 && accepted>=0 && accepted<=frames && sources.count==frames && points.count==frames &&
        Set(points.map(\.index))==Set(0..<frames) && points.filter{$0.status=="measured"}.count==accepted &&
        sources.allSatisfy{$0.path.hasPrefix("/") && $0.sha256.count==64} && Set(sources.map(\.path)).count==frames &&
        (accepted==0 ? normalization==nil : ((normalization?.isFinite ?? false) && (normalization ?? 0)>0)) &&
        comparisons.allSatisfy{$0.samples==accepted && ($0.leaveOneOutRelativeRMS.map{$0.isFinite && $0>=0} ?? true)} &&
        recipeSHA256.count==64 && effectiveGain.isFinite && effectiveGain>0 && timing=="JD_UTC_exposure_midpoint" &&
        Set(products.keys)==["lightcurve.csv","measurements.csv"] && comparisons.count>=2 && comparisons.count<=16 &&
        points.allSatisfy {p in
            ["measured","star-rejected","frame-rejected"].contains(p.status) && (p.jdUTC?.isFinite ?? true) &&
            (p.status=="measured" ? (p.jdUTC != nil && (p.relativeFlux?.isFinite ?? false) && (p.relativeFlux ?? 0)>0 && (p.relativeError?.isFinite ?? false) && (p.relativeError ?? -1)>=0) : (p.relativeFlux==nil && p.relativeError==nil))
        }
    }
    static func load(_ directory:URL) async throws -> Self {
        let task=Task.detached(priority:.userInitiated) {
            let url=directory.appendingPathComponent("report.json")
            guard (try url.resourceValues(forKeys:[.fileSizeKey]).fileSize ?? Int.max)<=16*1024*1024 else {throw ProcessingServiceError.invalidReport(command:"time-series")}
            let bytes=try Data(contentsOf:url)
            let digestURL=directory.appendingPathComponent("report.sha256")
            guard (try digestURL.resourceValues(forKeys:[.fileSizeKey]).fileSize ?? Int.max)==65 else {throw ProcessingServiceError.invalidReport(command:"time-series")}
            let digest=try String(contentsOf:digestURL,encoding:.utf8).trimmingCharacters(in:.whitespacesAndNewlines)
            guard SHA256.hash(data:bytes).map({String(format:"%02x",$0)}).joined()==digest else {throw ProcessingServiceError.invalidArguments("Photometry report changed.")}
            let report=try JSONDecoder().decode(Self.self,from:bytes)
            guard report.isValid else {throw ProcessingServiceError.invalidReport(command:"time-series")}
            var files=report.products.map{(directory.appendingPathComponent($0.key),$0.value)}
            files.append((directory.appendingPathComponent("recipe.txt"),report.recipeSHA256))
            files += report.sources.map{(URL(fileURLWithPath:$0.path),$0.sha256)}
            for (file,sha) in files {
                let info=try file.resourceValues(forKeys:[.isRegularFileKey,.isSymbolicLinkKey])
                guard info.isRegularFile==true,info.isSymbolicLink != true,sha.count==64 else {throw ProcessingServiceError.invalidReport(command:"time-series")}
                let handle=try FileHandle(forReadingFrom:file);defer{try? handle.close()};var hash=SHA256()
                while let bytes=try handle.read(upToCount:1024*1024),!bytes.isEmpty {try Task.checkCancellation();hash.update(data:bytes)}
                guard hash.finalize().map({String(format:"%02x",$0)}).joined()==sha else {throw ProcessingServiceError.invalidArguments("Photometry input or product changed: \(file.lastPathComponent)")}
            }
            return report
        }
        return try await withTaskCancellationHandler(operation:{try await task.value},onCancel:{task.cancel()})
    }
}
@MainActor final class TimeSeriesSession:ObservableObject {
    @Published var configuration=TimeSeriesConfiguration(){didSet{if oldValue != configuration {report=nil}}}
    @Published var image:NSImage?
    @Published var width=0
    @Published var height=0
    @Published var selection:Int?
    @Published var report:TimeSeriesReport?
    @Published var directory:URL?
    @Published var error:String?
    @Published var busy=false
    private let work=FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStack-TimeSeries-"+UUID().uuidString)
    deinit{try? FileManager.default.removeItem(at:work)}
    func choose(_ frames:[URL],using service:any ProcessingService) async {
        guard !busy else{return};busy=true;defer{busy=false};error=nil;image=nil;report=nil;directory=nil
        configuration.frames=frames.sorted{$0.path.localizedStandardCompare($1.path) == .orderedAscending};configuration.stars=[];selection=nil
        do {try await preview(using:service,selectChannel:true)}catch is CancellationError {}catch {self.error=error.localizedDescription}
    }
    private func preview(using service:any ProcessingService,selectChannel:Bool) async throws {
        try Task.checkCancellation()
        guard let first=configuration.frames.first else{return}
        struct Info:Decodable{var width,height,channels:Int;var format:String}
        let response=try await service.runCLI(arguments:["inspect",first.path])
        let info=try JSONDecoder().decode(Info.self,from:Data(response.standardOutput.utf8))
        guard info.format=="FITS",info.width>0,info.height>0,[1,3,4].contains(info.channels) else {throw ProcessingServiceError.invalidArguments("Choose mono or RGB FITS frames.")}
        width=info.width;height=info.height
        if selectChannel {configuration.channel=info.channels==1 ? "mono":"green"}
        try FileManager.default.createDirectory(at:work,withIntermediateDirectories:true)
        let output=work.appendingPathComponent(UUID().uuidString+".tiff")
        _ = try await service.runCLI(arguments:["review-preview","--input",first.path,"--output",output.path,"--fits-debayer","off"])
        try Task.checkCancellation()
        guard let loaded=NSImage(contentsOf:output) else {throw ProcessingServiceError.invalidReport(command:"review-preview")}
        image=loaded
    }
    func tap(x:Double,y:Double){
        guard !busy,x>=0,y>=0,x<Double(width),y<Double(height) else{return}
        if let selection,configuration.stars.indices.contains(selection) {configuration.stars[selection].x=x;configuration.stars[selection].y=y;self.selection=nil}
        else if configuration.stars.count<17 {
            let names=Set(configuration.stars.map(\.id));let id=configuration.stars.isEmpty ? "target":(1...16).map{"c\($0)"}.first{!names.contains($0)}!
            configuration.stars.append(.init(id:id,x:x,y:y))
        }
    }
    func run(output:URL,using service:any ProcessingService) async throws {
        guard !busy else {throw ProcessingServiceError.invalidArguments("Photometry is busy.")};busy=true;defer{busy=false};error=nil
        let snapshot=configuration;let text=try snapshot.recipe();try FileManager.default.createDirectory(at:work,withIntermediateDirectories:true)
        let recipe=work.appendingPathComponent(UUID().uuidString+".txt");try text.write(to:recipe,atomically:true,encoding:.utf8)
        _ = try await service.runCLI(arguments:["time-series","--recipe",recipe.path,"--output-dir",output.path])
        let report=try await TimeSeriesReport.load(output);try Task.checkCancellation()
        try JSONEncoder().encode(snapshot).write(to:output.appendingPathComponent("session.json"),options:.atomic)
        self.report=report;directory=output
    }
    func open(_ output:URL,using service:(any ProcessingService)?=nil) async throws {
        guard !busy else{return};busy=true;defer{busy=false}
        let value=try await TimeSeriesReport.load(output)
        let state=output.appendingPathComponent("session.json")
        if FileManager.default.fileExists(atPath:state.path) {
            guard (try state.resourceValues(forKeys:[.fileSizeKey]).fileSize ?? Int.max)<=4*1024*1024 else {throw ProcessingServiceError.invalidReport(command:"time-series")}
            let config=try JSONDecoder().decode(TimeSeriesConfiguration.self,from:Data(contentsOf:state))
            let digest=SHA256.hash(data:Data(try config.recipe().utf8)).map{String(format:"%02x",$0)}.joined()
            guard digest==value.recipeSHA256 else {throw ProcessingServiceError.invalidArguments("Saved photometry settings disagree with the recipe.")}
            configuration=config
        } else {configuration=TimeSeriesConfiguration()}
        image=nil;selection=nil;report=value;directory=output;error=nil
        if let service,!configuration.frames.isEmpty {try await preview(using:service,selectChannel:false)}
    }
}
