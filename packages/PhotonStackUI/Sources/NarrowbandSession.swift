import Foundation
import PhotonStackProcessing

struct NarrowbandReport: Decodable, Equatable {
    var type, command, operation: String
    var directory: String
    var greenHydrogen: Double
    var frames: Int
    var outputs: [String]
    var isValid: Bool {
        type == "complete" && command == "narrowband-" + operation && ["extract", "stack", "compose"].contains(operation) &&
        directory.hasPrefix("/") && greenHydrogen.isFinite && (0...1).contains(greenHydrogen) && frames > 0 &&
        !outputs.isEmpty && Set(outputs).count == outputs.count && outputs.allSatisfy {
            ["Ha.fits", "OIII.fits", "HOO.fits", "Ha-valid.fits", "OIII-valid.fits", "Ha-weight.fits",
             "OIII-weight.fits", "geometric-coverage.fits", "validity.fits"].contains($0)
        }
    }
}
@MainActor final class NarrowbandSession: ObservableObject {
    @Published private(set) var report: NarrowbandReport?
    @Published var greenHydrogen: Double = 0
    var directory: URL? { report.map { URL(fileURLWithPath: $0.directory, isDirectory: true) } }
    var recipe: URL? { directory?.appendingPathComponent("recipe.txt") }
    var primary: URL? {
        guard let report, let directory else { return nil }
        return directory.appendingPathComponent(report.outputs.contains("HOO.fits") ? "HOO.fits" : "Ha.fits")
    }
    func open(_ directory: URL, using service: any ProcessingService) async throws -> URL {
        let result = try await service.runCLI(arguments: ["narrowband-inspect", "--recipe", directory.appendingPathComponent("recipe.txt").path])
        try Task.checkCancellation()
        let decoded = try JSONDecoder().decode(NarrowbandReport.self, from: Data(result.standardOutput.utf8))
        guard decoded.isValid, URL(fileURLWithPath: decoded.directory).standardizedFileURL == directory.standardizedFileURL else {
            throw ProcessingServiceError.invalidReport(command: "narrowband-inspect")
        }
        report = decoded; greenHydrogen = decoded.greenHydrogen
        return directory.appendingPathComponent(decoded.outputs.contains("HOO.fits") ? "HOO.fits" : "Ha.fits")
    }
    func run(_ arguments: [String], output: URL, using service: any ProcessingService) async throws -> URL {
        _ = try await service.runCLI(arguments: arguments)
        try Task.checkCancellation()
        return try await open(output, using: service)
    }
    func replay(output: URL, using service: any ProcessingService) async throws -> URL {
        guard let recipe, let report else { throw ProcessingServiceError.invalidArguments("Open a narrowband result first.") }
        var args = ["narrowband-replay", "--recipe", recipe.path, "--output-dir", output.path]
        if report.operation == "compose" { args += ["--green-ha", String(greenHydrogen)] }
        return try await run(args, output: output, using: service)
    }
    func remix(output: URL, using service: any ProcessingService) async throws -> URL {
        guard let directory, let report, report.outputs.contains("Ha.fits"), report.outputs.contains("OIII.fits") else {
            throw ProcessingServiceError.invalidArguments("Open extracted or stacked line masters first.")
        }
        let fraction = greenHydrogen
        _ = try await open(directory, using: service)
        greenHydrogen = fraction
        return try await run(["narrowband-compose", "--ha", directory.appendingPathComponent("Ha.fits").path,
            "--oiii", directory.appendingPathComponent("OIII.fits").path, "--green-ha", String(fraction),
            "--output-dir", output.path], output: output, using: service)
    }
    func verifiedOutput(_ name: String, using service: any ProcessingService) async throws -> URL {
        guard let directory, report?.outputs.contains(name) == true else {
            throw ProcessingServiceError.invalidArguments("Unknown narrowband product.")
        }
        let fraction = greenHydrogen
        defer { greenHydrogen = fraction }
        _ = try await open(directory, using: service)
        return directory.appendingPathComponent(name)
    }
}
