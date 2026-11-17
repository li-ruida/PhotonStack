import Foundation
import PhotonStackProcessing

struct ChannelMaster: Identifiable, Equatable {
    var id = UUID()
    var name: String
    var path = ""
}
enum ChannelPreset: String, CaseIterable {
    case hoo = "HOO", sho = "SHO", rgb = "RGB", lrgb = "LRGB", custom = "Custom"
    var expressions: [String] {
        switch self {
        case .hoo: ["Ha", "OIII", "OIII"]
        case .sho: ["SII", "Ha", "OIII"]
        case .rgb: ["R", "G", "B"]
        case .lrgb: ["L*R/(0.2126*R+0.7152*G+0.0722*B)", "L*G/(0.2126*R+0.7152*G+0.0722*B)", "L*B/(0.2126*R+0.7152*G+0.0722*B)"]
        case .custom: ["Ha", "OIII", "OIII"]
        }
    }
    var symbols: [String] {
        switch self {
        case .hoo, .custom: ["Ha", "OIII"]
        case .sho: ["SII", "Ha", "OIII"]
        case .rgb: ["R", "G", "B"]
        case .lrgb: ["L", "R", "G", "B"]
        }
    }
}
struct ChannelCompositionReport: Decodable {
    struct Source: Decodable { var name, path, sha: String }
    var type, command, directory: String
    var aligned: Bool
    var sources: [Source]
    var expressions, outputs: [String]
    var invalidPixels, workers: Int?
    var wcsPreserved: Bool?
    var isValid: Bool {
        type == "complete" && command == "channel-compose" && directory.hasPrefix("/") && aligned &&
            (1...16).contains(sources.count) && expressions.count == 3 && expressions.allSatisfy { !$0.isEmpty } &&
            Set(sources.map(\.name)).count == sources.count && sources.allSatisfy {
                !$0.name.isEmpty && $0.path.hasPrefix("/") && $0.sha.count == 64 &&
                    $0.sha.allSatisfy { "0123456789abcdef".contains($0) }
            } && outputs == ["RGB.fits", "validity.fits"]
    }
}
@MainActor final class ChannelCompositionSession: ObservableObject {
    @Published var masters = ChannelPreset.hoo.symbols.map { ChannelMaster(name: $0) }
    @Published var expressions = ChannelPreset.hoo.expressions
    @Published var aligned = false
    @Published private(set) var report: ChannelCompositionReport?
    @Published private(set) var lastRun: ChannelCompositionReport?
    var canCompose: Bool {
        aligned && (1...16).contains(masters.count) && expressions.count == 3 && expressions.allSatisfy { !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty } &&
            masters.allSatisfy { !$0.name.isEmpty && $0.path.hasPrefix("/") } && Set(masters.map(\.name)).count == masters.count
    }
    func apply(_ preset: ChannelPreset) {
        guard preset != .custom else { return }
        expressions = preset.expressions
        masters = preset.symbols.map { symbol in masters.first(where: { $0.name == symbol }) ?? ChannelMaster(name: symbol) }
    }
    func arguments(output: URL) throws -> [String] {
        guard canCompose else { throw ProcessingServiceError.invalidArguments("Choose aligned mono masters and three channel expressions.") }
        var args = ["channel-compose", "--aligned", "--output-dir", output.path]
        for input in masters { args += ["--input", input.name + "=" + input.path] }
        for (channel, expression) in zip(["--red", "--green", "--blue"], expressions) { args += [channel, expression] }
        return args
    }
    private func decode(_ text: String, directory: URL) throws -> ChannelCompositionReport {
        let result = try JSONDecoder().decode(ChannelCompositionReport.self, from: Data(text.utf8))
        guard result.isValid, URL(fileURLWithPath: result.directory).standardizedFileURL == directory.standardizedFileURL else {
            throw ProcessingServiceError.invalidReport(command: "channel-compose")
        }
        return result
    }
    private func inspect(_ directory: URL, using service: any ProcessingService) async throws -> ChannelCompositionReport {
        let result = try await service.runCLI(arguments: ["channel-inspect", "--recipe", directory.appendingPathComponent("recipe.txt").path])
        try Task.checkCancellation()
        return try decode(result.standardOutput, directory: directory)
    }
    func open(_ directory: URL, using service: any ProcessingService) async throws -> URL {
        let result = try await inspect(directory, using: service)
        report = result; lastRun = nil
        masters = result.sources.map { ChannelMaster(name: $0.name, path: $0.path) }
        expressions = result.expressions; aligned = result.aligned
        return directory.appendingPathComponent("RGB.fits")
    }
    func compose(output: URL, using service: any ProcessingService) async throws -> URL {
        // An opened recipe remains bound to its recorded sources. Replacing a
        // master intentionally is allowed, but a changed file at the same path is not.
        if let report {
            _ = try await inspect(URL(fileURLWithPath: report.directory), using: service)
        }
        let result = try await service.runCLI(arguments: arguments(output: output))
        try Task.checkCancellation()
        let run = try decode(result.standardOutput, directory: output)
        let image = try await open(output, using: service)
        lastRun = run
        return image
    }
    func verifiedOutput(_ name: String, using service: any ProcessingService) async throws -> URL {
        guard let report, report.outputs.contains(name) else { throw ProcessingServiceError.invalidArguments("Open a channel composition first.") }
        let directory = URL(fileURLWithPath: report.directory)
        _ = try await inspect(directory, using: service)
        return directory.appendingPathComponent(name)
    }
    func newComposition() { report = nil; lastRun = nil; aligned = false }
}
