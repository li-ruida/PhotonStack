import Foundation
import PhotonStackProcessing

enum MasterRegistrationMode: String, CaseIterable {
    case stars, wcs
    func title(chinese: Bool) -> String {
        self == .stars ? (chinese ? "共同星点" : "Common stars") : (chinese ? "天球坐标 WCS" : "Celestial WCS")
    }
}
struct MasterRegistrationReport: Decodable {
    struct Master: Decodable {
        var name, source, image, validity: String
        var fitStars, checkStars: Int
        var fitRms, median, p95, validFraction: Double
        var transform: [Double]
    }
    var type, command, directory, mode, reference: String
    var masters: [Master]
    var isValid: Bool {
        type == "complete" && command == "channel-register" && directory.hasPrefix("/") &&
            MasterRegistrationMode(rawValue: mode) != nil && (1...16).contains(masters.count) &&
            Set(masters.map(\.name)).count == masters.count && masters.contains { $0.name == reference } &&
            masters.allSatisfy { m in
                !m.name.isEmpty && !m.name.contains("/") && m.source.hasPrefix("/") &&
                    m.image == m.name + ".fits" && m.validity == m.name + "-validity.fits" &&
                    m.fitStars >= 0 && m.checkStars >= 0 && m.fitRms.isFinite && m.fitRms >= 0 &&
                    m.median.isFinite && m.median >= 0 && m.p95.isFinite && m.p95 >= m.median &&
                    m.validFraction.isFinite && m.validFraction > 0 && m.validFraction <= 1 &&
                    m.transform.count == 4 && m.transform.allSatisfy(\.isFinite) && m.transform[0] > 0
            }
    }
}
@MainActor final class MasterRegistrationSession: ObservableObject {
    @Published var mode: MasterRegistrationMode = .stars
    @Published var reference = ""
    @Published private(set) var report: MasterRegistrationReport?
    func referenceName(in masters: [ChannelMaster]) -> String {
        masters.contains { $0.name == reference } ? reference : masters.first?.name ?? ""
    }
    func canRegister(_ masters: [ChannelMaster]) -> Bool {
        (1...16).contains(masters.count) && Set(masters.map(\.name)).count == masters.count &&
            masters.allSatisfy { !$0.name.isEmpty && $0.path.hasPrefix("/") }
    }
    private func inspect(_ directory: URL, using service: any ProcessingService) async throws -> MasterRegistrationReport {
        let result = try await service.runCLI(arguments: ["channel-registration-inspect", "--recipe", directory.appendingPathComponent("recipe.txt").path])
        try Task.checkCancellation()
        let decoded = try JSONDecoder().decode(MasterRegistrationReport.self, from: Data(result.standardOutput.utf8))
        guard decoded.isValid, URL(fileURLWithPath: decoded.directory).standardizedFileURL == directory.standardizedFileURL else {
            throw ProcessingServiceError.invalidReport(command: "channel-registration-inspect")
        }
        return decoded
    }
    func open(_ directory: URL, into composition: ChannelCompositionSession, using service: any ProcessingService) async throws -> URL {
        let decoded = try await inspect(directory, using: service)
        report = decoded; mode = MasterRegistrationMode(rawValue: decoded.mode) ?? .stars; reference = decoded.reference
        composition.newComposition()
        composition.masters = decoded.masters.map { ChannelMaster(name: $0.name, path: directory.appendingPathComponent($0.image).path) }
        composition.aligned = true
        return directory.appendingPathComponent(decoded.reference + ".fits")
    }
    func register(_ composition: ChannelCompositionSession, output: URL, using service: any ProcessingService) async throws -> URL {
        let masters = composition.masters
        guard canRegister(masters) else { throw ProcessingServiceError.invalidArguments("Choose named mono masters before registration.") }
        var args = ["channel-register", "--mode", mode.rawValue, "--reference", referenceName(in: masters), "--output-dir", output.path]
        for master in masters { args += ["--input", master.name + "=" + master.path] }
        _ = try await service.runCLI(arguments: args)
        try Task.checkCancellation()
        return try await open(output, into: composition, using: service)
    }
    func replay(output: URL, into composition: ChannelCompositionSession, using service: any ProcessingService) async throws -> URL {
        guard let report else { throw ProcessingServiceError.invalidArguments("Open a registration recipe first.") }
        _ = try await service.runCLI(arguments: ["channel-registration-replay", "--recipe", URL(fileURLWithPath: report.directory).appendingPathComponent("recipe.txt").path, "--output-dir", output.path])
        try Task.checkCancellation()
        return try await open(output, into: composition, using: service)
    }
    func verifiedMaster(_ name: String, validity: Bool = false, using service: any ProcessingService) async throws -> URL {
        guard let report, let master = report.masters.first(where: { $0.name == name }) else {
            throw ProcessingServiceError.invalidArguments("Unknown registered master.")
        }
        let directory = URL(fileURLWithPath: report.directory)
        _ = try await inspect(directory, using: service)
        return directory.appendingPathComponent(validity ? master.validity : master.image)
    }
}
