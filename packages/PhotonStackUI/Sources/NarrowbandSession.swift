import Foundation
import PhotonStackProcessing

struct NarrowbandCalibrationReport: Decodable, Equatable {
    var bias = "", dark = "", flat = ""
    var darkBiasState = "unknown", flatBiasState = "unknown"
    var flatNormalization = "bayer-phase-mean"
    var isUsable: Bool {
        let paths = [bias, dark, flat].filter { !$0.isEmpty }
        return !paths.isEmpty && paths.allSatisfy { $0.hasPrefix("/") } && Set(paths).count == paths.count &&
            flatNormalization == "bayer-phase-mean" &&
            (dark.isEmpty ? darkBiasState == "unknown" : ["included", "removed"].contains(darkBiasState)) &&
            (flat.isEmpty ? flatBiasState == "unknown" : ["included", "removed"].contains(flatBiasState)) &&
            ((darkBiasState != "removed" && flatBiasState != "included") || !bias.isEmpty)
    }
    var arguments: [String] {
        var args: [String] = []
        for (name, path) in [("bias", bias), ("dark", dark), ("flat", flat)] where !path.isEmpty { args += ["--" + name, path] }
        if !dark.isEmpty { args += ["--dark-bias", darkBiasState] }
        if !flat.isEmpty { args += ["--flat-bias", flatBiasState] }
        return args
    }
}

enum NarrowbandSampling: String, CaseIterable {
    case redGrid = "red-grid", cellAverage = "cell-average"
    func title(chinese: Bool) -> String {
        switch self {
        case .redGrid: chinese ? "同位置采样" : "Common red-site grid"
        case .cellAverage: chinese ? "像素格均值（旧版）" : "Cell average (legacy)"
        }
    }
}

struct NarrowbandReport: Decodable, Equatable {
    var type, command, operation: String
    var directory: String
    var greenHydrogen: Double
    var frames: Int
    var outputs: [String]
    var calibration: NarrowbandCalibrationReport?
    var sampling: String?
    var effectiveSampling: String { sampling ?? (operation == "compose" ? "provided" : "cell-average") }
    var extractionSampling: NarrowbandSampling? { NarrowbandSampling(rawValue: effectiveSampling) }
    var isValid: Bool {
        type == "complete" && command == "narrowband-" + operation && ["extract", "stack", "compose"].contains(operation) &&
        directory.hasPrefix("/") && greenHydrogen.isFinite && (0...1).contains(greenHydrogen) && frames > 0 &&
        (operation == "compose" ? effectiveSampling == "provided" : extractionSampling != nil) &&
        (calibration == nil || (operation != "compose" && calibration?.isUsable == true)) &&
        !outputs.isEmpty && Set(outputs).count == outputs.count && outputs.allSatisfy {
            ["Ha.fits", "OIII.fits", "HOO.fits", "Ha-valid.fits", "OIII-valid.fits", "Ha-weight.fits",
             "OIII-weight.fits", "geometric-coverage.fits", "validity.fits"].contains($0)
        }
    }
}
@MainActor final class NarrowbandSession: ObservableObject {
    @Published private(set) var report: NarrowbandReport?
    @Published var greenHydrogen: Double = 0
    @Published var sampling: NarrowbandSampling = .redGrid
    @Published var useCalibration = false
    @Published var calibration = NarrowbandCalibrationReport()
    var canCalibrate: Bool { !useCalibration || calibration.isUsable }
    var extractionArguments: [String] { ["--sampling", sampling.rawValue] + calibrationArguments }
    var calibrationArguments: [String] { useCalibration ? calibration.arguments : [] }
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
        calibration = decoded.calibration ?? NarrowbandCalibrationReport()
        useCalibration = decoded.calibration != nil
        sampling = decoded.extractionSampling ?? .redGrid
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
        else { args += ["--sampling", sampling.rawValue] }
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
        let pending = calibration, enabled = useCalibration, pendingSampling = sampling
        defer { greenHydrogen = fraction; calibration = pending; useCalibration = enabled; sampling = pendingSampling }
        _ = try await open(directory, using: service)
        return directory.appendingPathComponent(name)
    }
}
