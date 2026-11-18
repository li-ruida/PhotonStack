import Foundation
import PhotonStackProcessing

struct CalibrationMasterConstruction: Codable, Equatable, Sendable {
    static let names: Set<String> = ["recipe.txt", "inputs.txt", "report.json", "accepted.fits"]
    var files: [String: String]
    var isValid: Bool { Set(files.keys) == Self.names && files.values.allSatisfy(SpectralCalibrationReport.digest) }
}

struct CalibrationMasterReport: Codable, Equatable, Sendable {
    struct Source: Codable, Equatable, Sendable {
        var path, sha: String
        var isValid: Bool {
            path.hasPrefix("/") && !path.contains("\0") && !path.contains("\n") && !path.contains("\r") &&
            SpectralCalibrationReport.digest(sha)
        }
    }
    struct Corrections: Codable, Equatable, Sendable { var bias, darkFlat: Source?; var darkFlatBiasState: String? }
    struct Inspection: Codable, Equatable, Sendable {
        var schema: Int
        var sourcesVerified, productsVerified: Bool
        var files: [String: String]
    }
    var schema: Int
    var command, domain, role, biasState, method, sampling, normalization: String
    var width, height, minimumSamples, invalidPixels: Int
    var missingSamples: UInt64
    var workingBytes: UInt64
    var tilePixels, workers: Int
    var temperatureTolerance, temperatureMin, temperatureMax: Double
    var sources: [Source]
    var corrections: Corrections
    var factors: [[Double]]
    var products: [String: String]
    var inspection: Inspection
    var construction: CalibrationMasterConstruction {
        CalibrationMasterConstruction(files: inspection.files.filter { $0.key != "master.fits" })
    }
    var isValid: Bool {
        guard schema == 1, command == "sensor-master-build", domain == "sensor-linear",
              CalibrationLibrary.roles.contains(role), ["included", "removed"].contains(biasState),
              ["median", "mean"].contains(method),
              ["mono", "CFA:RGGB", "CFA:BGGR", "CFA:GBRG", "CFA:GRBG"].contains(sampling),
              width > 0, height > 0, width <= 1_073_741_824 / height,
              (2...256).contains(sources.count), (2...sources.count).contains(minimumSamples),
              invalidPixels >= 0, invalidPixels < width * height,
              missingSamples <= UInt64(width * height) * UInt64(sources.count),
              workingBytes > 0, workingBytes <= 8 * 1024 * 1024 * 1024,
              (1...1_048_576).contains(tilePixels), (1...32).contains(workers),
              temperatureTolerance.isFinite, (0...10).contains(temperatureTolerance),
              temperatureMin.isFinite, temperatureMax.isFinite, temperatureMin >= -273.15,
              temperatureMax <= 200, temperatureMin <= temperatureMax,
              factors.count == sources.count, factors.allSatisfy({ $0.count == 4 && $0.allSatisfy { $0.isFinite && $0 > 0 } }),
              inspection.schema == 1, inspection.productsVerified, construction.isValid,
              Set(inspection.files.keys) == CalibrationMasterConstruction.names.union(["master.fits"]),
              inspection.files.values.allSatisfy(SpectralCalibrationReport.digest),
              Set(products.keys) == ["master.fits", "accepted.fits", "inputs.txt"],
              products.allSatisfy({ inspection.files[$0.key] == $0.value }) else { return false }
        let allSources = sources + [corrections.bias, corrections.darkFlat].compactMap { $0 }
        guard allSources.allSatisfy(\.isValid), Set(allSources.map(\.path)).count == allSources.count,
              Set(allSources.map(\.sha)).count == allSources.count else { return false }
        if role == "bias", corrections.bias != nil || corrections.darkFlat != nil { return false }
        if role != "flat", corrections.darkFlat != nil { return false }
        if role == "flat", corrections.bias == nil && corrections.darkFlat == nil { return false }
        if corrections.darkFlat != nil {
            guard ["included", "removed"].contains(corrections.darkFlatBiasState ?? ""),
                  (corrections.darkFlatBiasState == "removed") == (corrections.bias != nil) else { return false }
        } else if corrections.darkFlatBiasState != nil { return false }
        return biasState == ((corrections.bias != nil || role == "flat") ? "removed" : "included") &&
            normalization == (role == "flat" ? "corrected-phase-positive-mean-to-first-frame" : "none") &&
            (role == "flat" || factors.allSatisfy { $0 == [1, 1, 1, 1] })
    }
    static func inspect(_ directory: URL, service: any ProcessingService, verifySources: Bool) async throws -> Self {
        let response = try await service.runCLI(arguments: ["sensor-master-inspect", "--recipe",
            directory.appendingPathComponent("recipe.txt").path, "--verify-sources", verifySources ? "1" : "0"])
        let report = try JSONDecoder().decode(Self.self, from: Data(response.standardOutput.utf8))
        guard report.isValid, report.inspection.sourcesVerified == verifySources else {
            throw CalibrationAcquisition.failure("Invalid master construction report.")
        }
        try Task.checkCancellation()
        return report
    }
    func matches(_ acquisition: CalibrationAcquisition) -> Bool {
        acquisition.sha == inspection.files["master.fits"] && acquisition.width == width && acquisition.height == height &&
        acquisition.channels == 1 && acquisition.bitpix == -32 && acquisition.text["sampling"] == sampling &&
        acquisition.text["biasState"] == biasState && acquisition.text["domain"] == "linear" &&
        acquisition.text["frameType"]?.lowercased() == "master " + role &&
        acquisition.text["normalization"] == (role == "flat" ? "phase-mean" : "none") &&
        acquisition.numeric["sensorTemperatureMin"] == temperatureMin && acquisition.numeric["sensorTemperatureMax"] == temperatureMax &&
        acquisition.issues.isEmpty
    }
}

struct CalibrationMasterBuildRequest: Equatable, Sendable {
    var frames: [URL] = []
    var role = "dark", method = "median"
    var biasID, darkFlatID: String?
    var temperatureTolerance: Double = 1
    var minimumSamples = 2, memoryMiB = 512, workers = 4
    var isValid: Bool {
        (2...256).contains(frames.count) && Set(frames).count == frames.count && CalibrationLibrary.roles.contains(role) &&
        ["median", "mean"].contains(method) && temperatureTolerance.isFinite && (0...10).contains(temperatureTolerance) &&
        (2...frames.count).contains(minimumSamples) && (4...8192).contains(memoryMiB) && (1...32).contains(workers) &&
        (role != "bias" || (biasID == nil && darkFlatID == nil)) && (darkFlatID == nil || role == "flat") &&
        (role != "flat" || biasID != nil || darkFlatID != nil)
    }
}
