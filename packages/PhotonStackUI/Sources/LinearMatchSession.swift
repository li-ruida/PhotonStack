import Foundation
import PhotonStackProcessing

struct LinearMatchReport: Decodable {
    struct Source: Decodable { var name, path: String }
    struct Region: Decodable { var count: Int; var beforeMean, afterMean, afterRms: Double }
    var type, command, directory: String
    var reference, target: Source
    var low, high, gain, offset: Double
    var candidates, fitSamples, checkSamples, downweighted: Int
    var beforeRms, afterRms, medianResidual, p95AbsoluteResidual, referenceSpan, validFraction: Double
    var regions: [Region]
    var isValid: Bool {
        type == "complete" && command == "channel-match" && directory.hasPrefix("/") &&
            [reference,target].allSatisfy { !$0.name.isEmpty && !$0.name.contains("/") && $0.path.hasPrefix("/") } &&
            reference.name != target.name && low.isFinite && high.isFinite && low >= 0 && high <= 1 && high-low >= 0.1 &&
            gain.isFinite && gain > 0 && offset.isFinite && medianResidual.isFinite &&
            [beforeRms,afterRms,p95AbsoluteResidual,referenceSpan,validFraction].allSatisfy { $0.isFinite && $0 >= 0 } &&
            validFraction > 0 && validFraction <= 1 && candidates <= 65536 && fitSamples >= 64 && checkSamples >= 64 &&
            fitSamples+checkSamples <= candidates && downweighted >= 0 && downweighted <= fitSamples && regions.count == 16 &&
            regions.allSatisfy { $0.count >= 0 && $0.count <= checkSamples && $0.beforeMean.isFinite && $0.afterMean.isFinite && $0.afterRms.isFinite && $0.afterRms >= 0 } &&
            regions.reduce(0) { $0+$1.count } == checkSamples
    }
}
@MainActor final class LinearMatchSession: ObservableObject {
    @Published var reference = ""
    @Published var target = ""
    @Published var low = 0.02
    @Published var high = 0.98
    @Published private(set) var report: LinearMatchReport?
    func referenceName(_ masters: [ChannelMaster]) -> String { masters.contains { $0.name == reference } ? reference : masters.first?.name ?? "" }
    func targetName(_ masters: [ChannelMaster]) -> String {
        let others = masters.filter { $0.name != referenceName(masters) }
        return others.contains { $0.name == target } ? target : others.first?.name ?? ""
    }
    func canMatch(_ composition: ChannelCompositionSession) -> Bool {
        composition.aligned && composition.masters.count >= 2 && Set(composition.masters.map(\.name)).count == composition.masters.count &&
            composition.masters.allSatisfy { !$0.name.isEmpty && $0.path.hasPrefix("/") } &&
            low.isFinite && high.isFinite && low >= 0 && high <= 1 && high-low >= 0.1
    }
    private func inspect(_ directory: URL, using service: any ProcessingService) async throws -> LinearMatchReport {
        let result = try await service.runCLI(arguments: ["channel-match-inspect","--recipe",directory.appendingPathComponent("recipe.txt").path])
        try Task.checkCancellation()
        let value = try JSONDecoder().decode(LinearMatchReport.self,from:Data(result.standardOutput.utf8))
        guard value.isValid, URL(fileURLWithPath:value.directory).standardizedFileURL == directory.standardizedFileURL else {
            throw ProcessingServiceError.invalidReport(command:"channel-match-inspect")
        }
        return value
    }
    func open(_ directory: URL, using service: any ProcessingService) async throws -> URL {
        let value = try await inspect(directory,using:service)
        report=value;reference=value.reference.name;target=value.target.name;low=value.low;high=value.high
        return directory.appendingPathComponent("matched.fits")
    }
    func match(_ composition: ChannelCompositionSession, output: URL, using service: any ProcessingService) async throws -> URL {
        guard canMatch(composition),
              let ref=composition.masters.first(where:{$0.name == referenceName(composition.masters)}),
              let input=composition.masters.first(where:{$0.name == targetName(composition.masters)}) else {
            throw ProcessingServiceError.invalidArguments("Select two registered mono masters and a valid quantile range.")
        }
        if composition.report != nil {
            _ = try await composition.verifiedOutput("RGB.fits",using:service)
        }
        _ = try await service.runCLI(arguments:["channel-match","--aligned","--reference",ref.name+"="+ref.path,"--input",input.name+"="+input.path,"--low",String(low),"--high",String(high),"--output-dir",output.path])
        try Task.checkCancellation()
        let image = try await open(output,using:service)
        guard let index=composition.masters.firstIndex(where:{$0.id == input.id && $0.path == input.path}),
              composition.masters.contains(where:{$0.id == ref.id && $0.path == ref.path}), composition.aligned else {
            throw ProcessingServiceError.invalidArguments("The master selection changed. Open the saved intensity result to continue.")
        }
        composition.newComposition();composition.masters[index].path=image.path;composition.aligned=true
        return image
    }
    func replay(output: URL, using service: any ProcessingService) async throws -> URL {
        guard let report else { throw ProcessingServiceError.invalidArguments("Open an intensity recipe first.") }
        _ = try await service.runCLI(arguments:["channel-match-replay","--recipe",URL(fileURLWithPath:report.directory).appendingPathComponent("recipe.txt").path,"--output-dir",output.path])
        return try await open(output,using:service)
    }
    func restorePair(into composition: ChannelCompositionSession, using service: any ProcessingService) async throws -> URL {
        guard let report else { throw ProcessingServiceError.invalidArguments("Open an intensity recipe first.") }
        let image=try await verifiedOutput("matched.fits",using:service)
        composition.newComposition()
        composition.masters=[ChannelMaster(name:report.reference.name,path:report.reference.path),ChannelMaster(name:report.target.name,path:image.path)]
        composition.expressions=[report.reference.name,report.target.name,report.target.name];composition.aligned=true
        return image
    }
    func verifiedOutput(_ name: String, using service: any ProcessingService) async throws -> URL {
        guard let report, ["matched.fits","validity.fits","residual.fits","original"].contains(name) else {
            throw ProcessingServiceError.invalidArguments("Unknown intensity result.")
        }
        let directory=URL(fileURLWithPath:report.directory)
        _ = try await inspect(directory,using:service)
        return name == "original" ? URL(fileURLWithPath:report.target.path) : directory.appendingPathComponent(name)
    }
}
