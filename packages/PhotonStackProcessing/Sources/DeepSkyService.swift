import Foundation
import CryptoKit
#if canImport(Darwin)
import Darwin
#endif
import PhotonStackAppCore

public extension ProcessingService {
    func deepSkySchema() async throws -> DeepSkySchema {
        let result = try await runCLI(arguments: ["deep-sky", "schema"])
        let schema: DeepSkySchema = try decodeDeepSkyOutput(result)
        guard schema.version == 1, !schema.fields.isEmpty,
              Set(schema.fields.map(\.key)).count == schema.fields.count else { throw DeepSkyRecipeError.invalidReport }
        return schema
    }

    func inspectDeepSkyRecipe(_ url: URL) async throws -> DeepSkyInspectedRecipe {
        try decodeDeepSkyOutput(await runCLI(arguments: ["deep-sky", "--inspect-recipe", url.path]))
    }

    func finishDeepSky(recipe: URL, master: URL, outputDirectory: URL, previous: DeepSkyReport) async throws -> DeepSkyReport {
        try previous.validate()
        guard previous.success, !previous.analysisOnly, previous.master == master.path,
              !FileManager.default.fileExists(atPath: outputDirectory.path) else { throw DeepSkyRecipeError.invalidReport }
        let masterDigest = try deepSkySourceDigest(master)
        if let expected = previous.masterSHA256, expected != masterDigest {
            throw DeepSkyRecipeError.masterChanged
        }
        #if canImport(Darwin)
        for frame in previous.frames {
            var info = stat()
            guard stat(frame.input, &info) == 0, info.st_size >= 0,
                  UInt64(info.st_size) == frame.bytes,
                  Int64(info.st_mtimespec.tv_sec) * 1_000_000_000 + Int64(info.st_mtimespec.tv_nsec) == frame.modifiedTicks else {
                throw DeepSkyRecipeError.inputsChanged
            }
        }
        #endif
        let originalRecipe = try String(contentsOf: recipe, encoding: .utf8)
        let patternLine = "calibration.sensor-pattern \"on\""
        let hasPattern = originalRecipe.split(separator: "\n").contains(Substring(patternLine))
        var effectiveRecipe = recipe
        var temporaryRecipe: URL?
        defer { if let temporaryRecipe { try? FileManager.default.removeItem(at: temporaryRecipe) } }
        if hasPattern {
            guard previous.recipe.split(separator: "\n").contains(Substring(patternLine)),
                  let provenance = previous.sensorPatternReport, !provenance.isEmpty,
                  FileManager.default.fileExists(atPath: provenance) else { throw DeepSkyRecipeError.invalidReport }
            for frame in previous.frames {
                guard let expected = frame.sha256, expected.count == 64,
                      try deepSkySourceDigest(URL(fileURLWithPath: frame.input)) == expected else {
                    throw DeepSkyRecipeError.inputsChanged
                }
            }
            // The reused master already contains the correction. The engine's
            // finish-only contract deliberately rejects applying it a second time.
            let url = recipe.deletingLastPathComponent().appendingPathComponent(".finish-\(UUID().uuidString).txt")
            temporaryRecipe = url
            let text = originalRecipe.split(separator: "\n", omittingEmptySubsequences: false).map {
                $0 == Substring(patternLine) ? "calibration.sensor-pattern \"off\"" : String($0)
            }.joined(separator: "\n")
            try text.write(to: url, atomically: true, encoding: .utf8)
            effectiveRecipe = url
        }
        let command = try await runCLI(arguments: ["deep-sky", "--recipe", effectiveRecipe.path, "--from-master", master.path,
                                                    "--output-dir", outputDirectory.path])
        var report: DeepSkyReport = try decodeDeepSkyOutput(command)
        guard report.success, !report.analysisOnly, report.frames.isEmpty,
              URL(fileURLWithPath: report.master).standardizedFileURL == master.standardizedFileURL else {
            throw DeepSkyRecipeError.invalidReport
        }
        for path in [report.backgroundMaster, report.developed] {
            let url = URL(fileURLWithPath: path).standardizedFileURL
            guard url.deletingLastPathComponent() == outputDirectory.standardizedFileURL,
                  FileManager.default.fileExists(atPath: path) else { throw DeepSkyRecipeError.invalidReport }
        }
        // The original frame assessment belongs to the reused master. Keep it
        // separate from the finish-only engine report and retain its provenance.
        report.inputCount = previous.inputCount
        report.selectedCount = previous.selectedCount
        report.referenceIndex = previous.referenceIndex
        report.frames = previous.frames
        report.rejectionLow = previous.rejectionLow
        report.rejectionHigh = previous.rejectionHigh
        report.sensorPatternReport = previous.sensorPatternReport
        // Switching to scene-based finishing does not discard a reusable noise
        // bundle. It remains attached to the unchanged scientific master.
        if report.noiseReferencePaths.isEmpty {
            report.noiseReference = previous.noiseReference
            report.noiseReferenceManifest = previous.noiseReferenceManifest
            report.noiseReferenceInputs = previous.noiseReferenceInputs
        }
        try validateNoiseReferencePaths(report)
        guard try deepSkySourceDigest(master) == masterDigest else { throw DeepSkyRecipeError.masterChanged }
        report.masterSHA256 = masterDigest
        report.developedSHA256 = try deepSkySourceDigest(URL(fileURLWithPath: report.developed))
        if hasPattern { report.recipe = originalRecipe }
        try report.validate()
        try JSONEncoder().encode(report).write(to: outputDirectory.appendingPathComponent("app-report.json"), options: .atomic)
        return report
    }

    func deepSky(recipe: URL, outputDirectory: URL, analyzeOnly: Bool) async throws -> DeepSkyReport {
        guard !FileManager.default.fileExists(atPath: outputDirectory.path) else {
            throw ProcessingServiceError.outputCommitFailed(path: outputDirectory.path, reason: "Deep-sky runs require a new output directory.")
        }
        defer {
            // SIGTERM does not unwind C++ destructors. Only remove this run's
            // explicitly owned intermediates; retain reports and final masters.
            try? FileManager.default.removeItem(at: outputDirectory.appendingPathComponent(".aligned-work"))
        }
        var args = ["deep-sky", "--recipe", recipe.path, "--output-dir", outputDirectory.path]
        if analyzeOnly { args.append("--analyze-only") }
        var report: DeepSkyReport = try decodeDeepSkyOutput(await runCLI(arguments: args))
        try report.validate()
        guard report.success, report.analysisOnly == analyzeOnly else { throw DeepSkyRecipeError.invalidReport }
        if !analyzeOnly {
            try validateNoiseReferencePaths(report)
            for path in [report.master, report.backgroundMaster, report.developed] {
                let url = URL(fileURLWithPath: path).standardizedFileURL
                guard url.deletingLastPathComponent() == outputDirectory.standardizedFileURL,
                      FileManager.default.fileExists(atPath: path) else { throw DeepSkyRecipeError.invalidReport }
            }
            report.masterSHA256 = try deepSkySourceDigest(URL(fileURLWithPath: report.master))
            report.developedSHA256 = try deepSkySourceDigest(URL(fileURLWithPath: report.developed))
            try JSONEncoder().encode(report).write(to: outputDirectory.appendingPathComponent("app-report.json"), options: .atomic)
        }
        return report
    }
}

private func validateNoiseReferencePaths(_ report: DeepSkyReport) throws {
    if report.noiseReferencePaths.isEmpty { return }
    let directory = URL(fileURLWithPath: report.master).standardizedFileURL.deletingLastPathComponent()
    let names = ["stack-noise.fits", "stack-noise-reference.txt", "stack-noise-inputs.txt"]
    guard report.noiseReferencePaths.count == names.count else { throw DeepSkyRecipeError.invalidReport }
    for (path, name) in zip(report.noiseReferencePaths, names) {
        let url = URL(fileURLWithPath: path).standardizedFileURL
        let values = try url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
        guard url == directory.appendingPathComponent(name), values.isRegularFile == true,
              values.isSymbolicLink != true else { throw DeepSkyRecipeError.invalidReport }
    }
}

private func deepSkySourceDigest(_ url: URL) throws -> String {
    let file = try FileHandle(forReadingFrom: url)
    defer { try? file.close() }
    var digest = SHA256()
    while let data = try file.read(upToCount: 1_048_576), !data.isEmpty {
        try Task.checkCancellation()
        digest.update(data: data)
    }
    return digest.finalize().map { String(format: "%02x", $0) }.joined()
}

private func decodeDeepSkyOutput<T: Decodable>(_ result: ProcessingCommandResult) throws -> T {
    guard let line = result.standardOutput.split(separator: "\n").last else { throw DeepSkyRecipeError.invalidReport }
    return try JSONDecoder().decode(T.self, from: Data(line.utf8))
}
