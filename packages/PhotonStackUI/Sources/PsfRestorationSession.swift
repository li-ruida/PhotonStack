import Foundation
import PhotonStackProcessing

struct PsfRestorationReport:Decodable {
    var type,command,directory,measurement,source,channel,backend:String
    var iterations,threads,coveredPixels,protectedPixels,fftWidth,fftHeight:Int
    var amount,pedestal,noiseSigma,protectAbove,beforeRms,afterRms,beforeFlux,afterFlux:Double
    var spatiallyVariable:Bool
    var isValid:Bool {
        type=="complete" && command=="psf-restore" && directory.hasPrefix("/") && measurement.hasPrefix("/") && source.hasPrefix("/") &&
        ["luminance","red","green","blue"].contains(channel) && backend=="CPU pocketfft double" && (1...100).contains(iterations) && (1...16).contains(threads) &&
        coveredPixels>0 && coveredPixels<=16*1024*1024 && protectedPixels>=0 && protectedPixels<=coveredPixels && (1...32768).contains(fftWidth) && (1...32768).contains(fftHeight) &&
        [amount,pedestal,noiseSigma,protectAbove,beforeRms,afterRms].allSatisfy {$0.isFinite && $0>=0} && amount<=1 && beforeFlux.isFinite && afterFlux.isFinite
    }
}
extension PsfMeasurementSession {
    var canRestore:Bool {
        guard let report,canEditExclusions,excluded==Set(report.stars.filter{$0.status=="excluded"}.map(\.id)) else {return false}
        return (1...100).contains(restoreIterations) && [restoreAmount,restorePedestal,restoreNoise,restoreProtect].allSatisfy{$0.isFinite && $0>=0} && restoreAmount<=1
    }
    private func inspectRestoration(_ directory:URL,using service:any ProcessingService) async throws -> PsfRestorationReport {
        let output=try await service.runCLI(arguments:["psf-restoration-inspect","--recipe",directory.appendingPathComponent("recipe.txt").path])
        try Task.checkCancellation()
        let value=try JSONDecoder().decode(PsfRestorationReport.self,from:Data(output.standardOutput.utf8))
        guard value.isValid,URL(fileURLWithPath:value.directory).standardizedFileURL==directory.standardizedFileURL else {throw ProcessingServiceError.invalidReport(command:"psf-restoration-inspect")}
        return value
    }
    func restore(output:URL,using service:any ProcessingService) async throws -> URL {
        guard canRestore,let report else {throw ProcessingServiceError.invalidArguments("Measure or save the current PSF selection before restoring its channel.")}
        let measurement=URL(fileURLWithPath:report.directory).appendingPathComponent("recipe.txt")
        _ = try await service.runCLI(arguments:["psf-restore","--measurement",measurement.path,"--output-dir",output.path,
            "--iterations",String(restoreIterations),"--amount",String(restoreAmount),"--pedestal",String(restorePedestal),
            "--noise-sigma",String(restoreNoise),"--protect-above",String(restoreProtect)])
        try Task.checkCancellation()
        restoration=try await inspectRestoration(output,using:service)
        return output.appendingPathComponent("restored.fits")
    }
    func openRestoration(_ directory:URL,using service:any ProcessingService) async throws -> URL {
        let value=try await inspectRestoration(directory,using:service)
        _ = try await open(URL(fileURLWithPath:value.measurement).deletingLastPathComponent(),using:service)
        restoreIterations=value.iterations;restoreAmount=value.amount;restorePedestal=value.pedestal
        restoreNoise=value.noiseSigma;restoreProtect=value.protectAbove;restoration=value
        return directory.appendingPathComponent("restored.fits")
    }
    func restoredOutput(_ name:String,using service:any ProcessingService) async throws -> URL {
        guard let restoration,["channel.fits","restored.fits","difference.fits","residual.fits","protection.fits","original"].contains(name) else {throw ProcessingServiceError.invalidArguments("Open a PSF restoration first.")}
        let directory=URL(fileURLWithPath:restoration.directory)
        _ = try await inspectRestoration(directory,using:service)
        return name=="original" ? URL(fileURLWithPath:restoration.source) : directory.appendingPathComponent(name)
    }
}
