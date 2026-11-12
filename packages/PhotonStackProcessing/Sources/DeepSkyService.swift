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

    func finishDeepSky(recipe: URL, master: URL, outputDirectory: URL, previous: DeepSkyReport, buildIdentity: String? = nil) async throws -> DeepSkyReport {
        try previous.validate()
        guard previous.success, !previous.analysisOnly, previous.master == master.path,
              !FileManager.default.fileExists(atPath: outputDirectory.path) else { throw DeepSkyRecipeError.invalidReport }
        let previousReportURL = URL(fileURLWithPath: previous.developed).deletingLastPathComponent().appendingPathComponent("app-report.json")
        let sourcesVerified = try await DeepSkyResultReuse.validateSourcesForRefinish(report: previous, reportURL: previousReportURL)
        var reuse: DeepSkyResultReuse.Prepared?
        if let buildIdentity, let sourcesVerified {
            var candidate = try await prepareDeepSkyReuse(recipe: recipe, buildIdentity: buildIdentity)
            candidate?.computeBackend = sourcesVerified.computeBackend
            // A master made by engine/build A and finished by B is valid, but
            // cannot claim to be the result of running the full recipe with B.
            if candidate?.buildIdentity == sourcesVerified.buildIdentity,
               candidate?.engineSHA256 == sourcesVerified.engineSHA256,
               candidate?.preservesStack(from: previous.recipe) == true { reuse = candidate }
        }
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
        try validateDeepSkyProducts([report.backgroundMaster, report.developed], in: outputDirectory)
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
        if let sourcesVerified {
            try await DeepSkyResultReuse.revalidateSourcesForRefinish(sourcesVerified, reportURL: previousReportURL)
        }
        report.masterSHA256 = masterDigest
        report.developedSHA256 = try deepSkySourceDigest(URL(fileURLWithPath: report.developed))
        if hasPattern { report.recipe = originalRecipe }
        try report.validate()
        let reportURL = outputDirectory.appendingPathComponent("app-report.json")
        try JSONEncoder().encode(report).write(to: reportURL, options: .atomic)
        if let reuse { try await DeepSkyResultReuse.publish(reuse, report: report, reportURL: reportURL) }
        return report
    }

    func deepSky(
        recipe: URL,
        outputDirectory: URL,
        analyzeOnly: Bool,
        analysisWorkers: Int = 0,
        stackWorkers: Int = 0,
        gpuAcceleration: Bool? = nil,
        computeBackend: DeepSkyComputeBackend? = nil,
        buildIdentity: String? = nil
    ) async throws -> DeepSkyReport {
        guard !FileManager.default.fileExists(atPath: outputDirectory.path) else {
            throw ProcessingServiceError.outputCommitFailed(path: outputDirectory.path, reason: "Deep-sky runs require a new output directory.")
        }
        guard (0...8).contains(analysisWorkers), (0...16).contains(stackWorkers) else {
            throw ProcessingServiceError.invalidArguments(
                "Deep-sky workers require analysis in 0...8 and stack combine in 0...16; zero selects automatic scheduling."
            )
        }
        defer {
            // SIGTERM does not unwind C++ destructors. Only remove this run's
            // explicitly owned intermediates; retain reports and final masters.
            try? FileManager.default.removeItem(at: outputDirectory.appendingPathComponent(".aligned-work"))
        }
        var reuse = try await prepareDeepSkyReuse(recipe: recipe, buildIdentity: analyzeOnly ? nil : buildIdentity)
        var args = ["deep-sky", "--recipe", recipe.path, "--output-dir", outputDirectory.path]
        if analyzeOnly { args.append("--analyze-only") }
        if analysisWorkers > 0 { args += ["--analysis-workers", String(analysisWorkers)] }
        if stackWorkers > 0 { args += ["--stack-workers", String(stackWorkers)] }
        let backend = computeBackend ?? DeepSkyComputeBackend(gpuAcceleration: gpuAcceleration)
        reuse?.computeBackend = backend
        args += ["--gpu-acceleration", backend == .cpu ? "off" : "on"]
        args += ["--compute-backend", backend.rawValue]
        var report: DeepSkyReport = try decodeDeepSkyOutput(await runCLI(arguments: args))
        try report.validate()
        if let data = try? Data(contentsOf: outputDirectory.appendingPathComponent("performance.json")) {
            report.performance = try? JSONDecoder().decode(DeepSkyRunPerformance.self, from: data)
        }
        guard report.success, report.analysisOnly == analyzeOnly else { throw DeepSkyRecipeError.invalidReport }
        if !analyzeOnly {
            try validateNoiseReferencePaths(report)
            try validateDeepSkyProducts([report.master, report.backgroundMaster, report.developed], in: outputDirectory)
            report.masterSHA256 = try deepSkySourceDigest(URL(fileURLWithPath: report.master))
            report.developedSHA256 = try deepSkySourceDigest(URL(fileURLWithPath: report.developed))
            let reportURL = outputDirectory.appendingPathComponent("app-report.json")
            try JSONEncoder().encode(report).write(to: reportURL, options: .atomic)
            if let reuse { try await DeepSkyResultReuse.publish(reuse, report: report, reportURL: reportURL) }
        }
        return report
    }

    private func prepareDeepSkyReuse(recipe: URL, buildIdentity: String?) async throws -> DeepSkyResultReuse.Prepared? {
        #if os(macOS)
        guard let buildIdentity, let cli = self as? CLIProcessingService else { return nil }
        return try await DeepSkyResultReuse.prepare(inspectDeepSkyRecipe(recipe), buildIdentity: buildIdentity, engine: cli.executableURL)
        #else
        return nil
        #endif
    }

    func reusableDeepSkyResult(reportURL: URL, recipe: String, output: URL, buildIdentity: String, computeBackend: DeepSkyComputeBackend? = nil) async throws -> URL? {
        #if os(macOS)
        guard let cli = self as? CLIProcessingService else { return nil }
        return try await DeepSkyResultReuse.validatedOutput(reportURL: reportURL, recipe: recipe,
            output: output, buildIdentity: buildIdentity, engine: cli.executableURL, computeBackend: computeBackend)
        #else
        return nil
        #endif
    }
}

private func validateDeepSkyProducts(_ paths: [String], in outputDirectory: URL) throws {
    // URL equality includes the directory/trailing-slash hint. Export replay and
    // refinish callers legitimately construct the same directory without it.
    // Compare canonical filesystem paths, while rejecting linked products and
    // any engine report that points outside this run's output directory.
    let directory = outputDirectory.resolvingSymlinksInPath().standardizedFileURL.path
    for path in paths {
        let url = URL(fileURLWithPath: path)
        guard let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey]),
              values.isRegularFile == true, values.isSymbolicLink != true,
              url.resolvingSymlinksInPath().standardizedFileURL.deletingLastPathComponent().path == directory else {
            throw DeepSkyRecipeError.invalidReport
        }
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
