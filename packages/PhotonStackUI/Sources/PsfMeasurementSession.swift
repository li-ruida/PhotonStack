import Foundation
import PhotonStackProcessing

struct PsfMeasurementReport: Decodable {
    struct Star: Decodable, Identifiable {
        var id,sampleX,sampleY: Int
        var x,y,xx,yy,xy,residual: Double
        var status: String
        var fwhm: Double { xx*yy-xy*xy > 0 ? 2.354820045*pow(xx*yy-xy*xy,0.25) : 0 }
    }
    struct Region: Decodable { var count: Int; var fwhm,xx,yy,xy: Double }
    var type,command,directory,source,sha,channel: String
    var sigma,saturation: Double
    var width,height,detected,accepted: Int
    var xx,yy,xy,majorFwhm,minorFwhm,angle,fwhmP10,fwhmP50,fwhmP90,medianResidual: Double
    var spatiallyVariable: Bool
    var regions: [Region]
    var stars: [Star]
    var isValid: Bool {
        guard type=="complete",command=="psf-measure",directory.hasPrefix("/"),source.hasPrefix("/"),
              sha.count==64,sha.allSatisfy({"0123456789abcdef".contains($0)}),["luminance","red","green","blue"].contains(channel),
              sigma.isFinite,(2...30).contains(sigma),saturation.isFinite,saturation>=0,width>0,height>0,
              accepted>=12,accepted<=stars.count,stars.count<=2048,detected>=stars.count,regions.count==16 else {return false}
        guard [xx,yy,majorFwhm,minorFwhm,fwhmP10,fwhmP50,fwhmP90,medianResidual].allSatisfy({$0.isFinite && $0>=0}),
              xy.isFinite,angle.isFinite,abs(angle)<=90,xx*yy-xy*xy>0,minorFwhm>0,majorFwhm>=minorFwhm,
              fwhmP10>0,fwhmP10<=fwhmP50,fwhmP50<=fwhmP90 else {return false}
        let statuses:Set<String>=["accepted","excluded","edge","masked","saturated","crowded","fit-failed","shape-outlier"]
        guard stars.enumerated().allSatisfy({ i,s in s.id==i && s.sampleX>=0 && s.sampleX<width && s.sampleY>=0 && s.sampleY<height && statuses.contains(s.status) &&
            [s.x,s.y,s.xx,s.yy,s.xy,s.residual].allSatisfy(\.isFinite) && s.x>=0 && s.x<Double(width) && s.y>=0 && s.y<Double(height) && s.residual>=0 &&
            (s.status != "accepted" || s.xx*s.yy-s.xy*s.xy>0) }),
            stars.filter({$0.status=="accepted"}).count==accepted,
            regions.allSatisfy({$0.count>=0 && $0.count<=accepted && $0.fwhm.isFinite && $0.fwhm>=0 && [$0.xx,$0.yy,$0.xy].allSatisfy(\.isFinite) && ($0.count==0 || $0.xx*$0.yy-$0.xy*$0.xy>0)}),
            regions.reduce(0,{$0+$1.count})==accepted else {return false}
        return true
    }
}
@MainActor final class PsfMeasurementSession: ObservableObject {
    @Published private(set) var source = ""
    @Published var channel = "luminance" { didSet { if oldValue != channel {excluded=[]} } }
    @Published var sigma = 5.0 { didSet { if oldValue != sigma {excluded=[]} } }
    @Published var saturation = 0.0 { didSet { if oldValue != saturation {excluded=[]} } }
    @Published var excluded:Set<Int> = []
    @Published private(set) var report:PsfMeasurementReport?
    @Published var restoration:PsfRestorationReport?
    @Published var restoreIterations=8
    @Published var restoreAmount=0.7
    @Published var restorePedestal=0.0
    @Published var restoreNoise=0.0
    @Published var restoreProtect=0.0
    var canEditExclusions:Bool {
        guard let report else {return false}
        return channel==report.channel && sigma==report.sigma && saturation==report.saturation
    }
    var canMeasure:Bool {
        guard source.hasPrefix("/"),sigma.isFinite,(2...30).contains(sigma),saturation.isFinite,saturation>=0 else {return false}
        if excluded.isEmpty {return true}
        guard canEditExclusions,let report else {return false}
        return excluded.allSatisfy {report.stars.indices.contains($0) && ["accepted","excluded"].contains(report.stars[$0].status)}
    }
    func choose(_ url:URL) {source=url.path;report=nil;excluded=[];restoration=nil}
    private func inspect(_ directory:URL,using service:any ProcessingService) async throws -> PsfMeasurementReport {
        let result=try await service.runCLI(arguments:["psf-inspect","--recipe",directory.appendingPathComponent("recipe.txt").path])
        try Task.checkCancellation()
        let value=try JSONDecoder().decode(PsfMeasurementReport.self,from:Data(result.standardOutput.utf8))
        guard value.isValid,URL(fileURLWithPath:value.directory).standardizedFileURL==directory.standardizedFileURL else {throw ProcessingServiceError.invalidReport(command:"psf-inspect")}
        return value
    }
    func open(_ directory:URL,using service:any ProcessingService) async throws -> URL {
        let value=try await inspect(directory,using:service)
        if report?.directory != value.directory {restoration=nil}
        report=value;source=value.source;channel=value.channel;sigma=value.sigma;saturation=value.saturation
        excluded=Set(value.stars.filter{$0.status=="excluded"}.map(\.id))
        return directory.appendingPathComponent("stars-preview.tiff")
    }
    func measure(output:URL,using service:any ProcessingService) async throws -> URL {
        guard canMeasure else {throw ProcessingServiceError.invalidArguments("Choose a linear FITS master and valid PSF options.")}
        var args=["psf-measure","--input",source,"--channel",channel,"--sigma",String(sigma),"--saturation",String(saturation),"--output-dir",output.path]
        if let report {
            _ = try await inspect(URL(fileURLWithPath:report.directory),using:service)
            args += ["--expected-sha",report.sha]
        }
        if !excluded.isEmpty {args += ["--exclude",excluded.sorted().map(String.init).joined(separator:",")]}
        _ = try await service.runCLI(arguments:args)
        try Task.checkCancellation()
        return try await open(output,using:service)
    }
    func verifiedOutput(_ name:String,using service:any ProcessingService) async throws -> URL {
        guard let report,["original","psf.fits","stars-preview.tiff"].contains(name) else {throw ProcessingServiceError.invalidArguments("Open a PSF measurement first.")}
        let directory=URL(fileURLWithPath:report.directory);_ = try await inspect(directory,using:service)
        return name=="original" ? URL(fileURLWithPath:report.source) : directory.appendingPathComponent(name)
    }
}
