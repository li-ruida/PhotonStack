import CryptoKit
import Foundation
import PhotonStackAppCore

/// A completed full-resolution result is reusable only with the same engine/app
/// build, canonical recipe, source contents, report, and developed image bytes.
/// Older runs without this receipt deliberately rebuild on export.
public enum DeepSkyResultReuse {
    public static let receiptFileName = "app-result-reuse.json"

    struct Source: Codable, Equatable, Sendable {
        /// Keep the recipe's logical location: a symlink or one of its parent
        /// directories can be retargeted while the old resolved file survives.
        let path: String
        let resolvedPath: String
        let sha256: String
    }

    struct Prepared: Sendable {
        let buildIdentity: String
        var computeBackend: DeepSkyComputeBackend? = nil
        let engineSHA256: String
        var recipe: String
        let sources: [Source]

        func preservesStack(from previousRecipe: String) -> Bool {
            func stackLines(_ text: String) -> [Substring] {
                text.split(separator: "\n").filter { line in
                    !["background.", "crop.", "denoise.", "develop.", "output.save-denoise-stages "]
                        .contains(where: { line.hasPrefix($0) })
                }
            }
            return stackLines(recipe) == stackLines(previousRecipe)
        }
    }

    struct VerifiedSources: Sendable {
        var computeBackend: DeepSkyComputeBackend? = nil
        let buildIdentity: String
        let engineSHA256: String
        let sources: [Source]
        let reportSHA256: String
    }

    private struct Receipt: Codable {
        var version = 2
        let buildIdentity: String
        var computeBackend: DeepSkyComputeBackend? = nil
        let engineSHA256: String
        let recipe: String
        let sources: [Source]
        let reportSHA256: String
        let outputPath: String
        let outputSHA256: String
    }

    static func prepare(_ inspected: DeepSkyInspectedRecipe, buildIdentity: String, engine: URL) async throws -> Prepared {
        var paths = inspected.inputs
        for key in ["calibration.darks", "calibration.biases", "calibration.flats"] {
            paths += try quotedPaths(inspected.values[key] ?? "")
        }
        return try await prepare(recipe: inspected.recipe, paths: paths, buildIdentity: buildIdentity, engine: engine)
    }

    static func prepare(recipe: String, paths: [String], buildIdentity: String, engine: URL,
                        verified: VerifiedSources? = nil) async throws -> Prepared {
        let logicalPaths = Set(paths.map { URL(fileURLWithPath: $0).standardizedFileURL.path }).sorted()
        guard !logicalPaths.isEmpty, logicalPaths.count <= 8_000 else { throw DeepSkyRecipeError.invalidReport }
        let fingerprints: [Source]
        if let verified, verified.sources.map(\.path) == logicalPaths {
            fingerprints = verified.sources
        } else {
            fingerprints = try await sources(logicalPaths)
        }
        return Prepared(buildIdentity: buildIdentity, engineSHA256: try digest(engine.resolvingSymlinksInPath()),
                        recipe: recipe, sources: fingerprints)
    }

    static func publish(_ prepared: Prepared, report: DeepSkyReport, reportURL: URL,
                        verifiedAfterRun: VerifiedSources? = nil) async throws {
        // Non-canonical historical finish recipes may remain usable, but must
        // not receive a receipt claiming an exact canonical recipe match.
        guard report.recipe == prepared.recipe else { return }
        guard report.success, !report.analysisOnly else { throw DeepSkyRecipeError.invalidReport }
        if verifiedAfterRun?.sources != prepared.sources,
           try await sources(prepared.sources.map(\.path)) != prepared.sources {
            throw DeepSkyRecipeError.inputsChanged
        }
        let output = URL(fileURLWithPath: report.developed).standardizedFileURL
        let receipt = Receipt(buildIdentity: prepared.buildIdentity, computeBackend: prepared.computeBackend, engineSHA256: prepared.engineSHA256, recipe: report.recipe,
            sources: prepared.sources, reportSHA256: try digest(reportURL),
            outputPath: output.path, outputSHA256: try digest(output))
        try JSONEncoder().encode(receipt).write(to: reportURL.deletingLastPathComponent().appendingPathComponent(receiptFileName), options: .atomic)
    }

    public static func validatedOutput(reportURL: URL, recipe: String, output: URL, buildIdentity: String, engine: URL, computeBackend: DeepSkyComputeBackend? = nil) async throws -> URL? {
        do {
            let receiptURL = reportURL.deletingLastPathComponent().appendingPathComponent(receiptFileName)
            let receiptData = try boundedReceipt(receiptURL)
            let receipt = try JSONDecoder().decode(Receipt.self, from: receiptData)
            guard computeBackend == nil || computeBackend == (receipt.computeBackend ?? .automatic),
                  receipt.version == 2, receipt.buildIdentity == buildIdentity, receipt.recipe == recipe,
                  try digest(engine.resolvingSymlinksInPath()) == receipt.engineSHA256,
                  receipt.outputPath == output.standardizedFileURL.path,
                  !receipt.sources.isEmpty, receipt.sources.count <= 8_000,
                  Set(receipt.sources.map(\.path)).count == receipt.sources.count,
                  try digest(reportURL) == receipt.reportSHA256,
                  try await sources(receipt.sources.map(\.path)) == receipt.sources,
                  try digest(output) == receipt.outputSHA256 else { return nil }
            return output
        } catch is CancellationError {
            throw CancellationError()
        } catch {
            // Missing, stale, corrupt or historical receipts are cache misses.
            return nil
        }
    }

    /// Finishing uses the scientific master, not the old developed image or old
    /// engine. Keep source provenance enforceable across build/finishing changes.
    /// Nil denotes a legacy run without the v2 content/target evidence.
    static func validateSourcesForRefinish(report: DeepSkyReport, reportURL: URL) async throws -> VerifiedSources? {
        let receiptURL = reportURL.deletingLastPathComponent().appendingPathComponent(receiptFileName)
        guard FileManager.default.fileExists(atPath: receiptURL.path) else { return nil }
        let data = try boundedReceipt(receiptURL)
        struct Version: Decodable { let version: Int }
        let version = try JSONDecoder().decode(Version.self, from: data).version
        if version == 1 { return nil }
        guard version == 2 else { throw DeepSkyRecipeError.invalidReport }
        let receipt = try JSONDecoder().decode(Receipt.self, from: data)
        guard receipt.recipe == report.recipe, receipt.outputPath == URL(fileURLWithPath: report.developed).standardizedFileURL.path,
              !receipt.sources.isEmpty, receipt.sources.count <= 8_000,
              Set(receipt.sources.map(\.path)).count == receipt.sources.count,
              try digest(reportURL) == receipt.reportSHA256,
              try JSONDecoder().decode(DeepSkyReport.self, from: boundedReceipt(reportURL, limit: 32 * 1_024 * 1_024)) == report else {
            throw DeepSkyRecipeError.invalidReport
        }
        let verified = VerifiedSources(computeBackend: receipt.computeBackend, buildIdentity: receipt.buildIdentity, engineSHA256: receipt.engineSHA256,
            sources: receipt.sources, reportSHA256: receipt.reportSHA256)
        try await revalidateSourcesForRefinish(verified, reportURL: reportURL)
        return verified
    }

    /// Hold the preflight evidence through the run. Removing, replacing or
    /// downgrading its receipt cannot silently weaken the postflight check.
    static func revalidateSourcesForRefinish(_ verified: VerifiedSources, reportURL: URL) async throws {
        guard try digest(reportURL) == verified.reportSHA256 else { throw DeepSkyRecipeError.invalidReport }
        do {
            guard try await sources(verified.sources.map(\.path)) == verified.sources else { throw DeepSkyRecipeError.inputsChanged }
        } catch is CancellationError { throw CancellationError() }
        catch { throw DeepSkyRecipeError.inputsChanged }
    }

    /// Canonical recipes quote each input and quote the calibration path list a
    /// second time. Reuse this parser for protecting export destinations as well.
    public static func sourcePaths(inRecipe recipe: String, requiringCanonicalLines: Bool = false) throws -> [String] {
        guard recipe.hasPrefix("PHOTONSTACK_DEEP_SKY_RECIPE 1\n") else { throw DeepSkyRecipeError.invalidText }
        var result: [String] = []
        for line in recipe.split(separator: "\n").dropFirst() {
            guard let separator = line.firstIndex(where: { $0.isWhitespace }), separator != line.startIndex else {
                if requiringCanonicalLines { throw DeepSkyRecipeError.invalidText }
                continue
            }
            let key = line[..<separator]
            let source = key == "input" || ["calibration.darks", "calibration.biases", "calibration.flats"].contains(String(key))
            guard source || requiringCanonicalLines else { continue }
            let outer = try quotedPaths(String(line[line.index(after: separator)...]), allowEmpty: true)
            guard outer.count == 1 else { throw DeepSkyRecipeError.invalidText }
            guard source else { continue }
            if key == "input" {
                guard !outer[0].isEmpty else { throw DeepSkyRecipeError.invalidText }
                result.append(outer[0])
            } else { result += try quotedPaths(outer[0]) }
        }
        return result
    }

    public static func calibrationSourcePaths(values: [String: String]) throws -> [String] {
        try ["calibration.darks", "calibration.biases", "calibration.flats"].flatMap { try quotedPaths(values[$0] ?? "") }
    }

    private static func boundedReceipt(_ url: URL, limit: Int = 16 * 1_024 * 1_024) throws -> Data {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        let data = try handle.read(upToCount: limit + 1) ?? Data()
        guard data.count <= limit else { throw DeepSkyRecipeError.invalidReport }
        return data
    }

    private static func sources(_ paths: [String]) async throws -> [Source] {
        let workers = min(4, paths.count)
        let groups = try await withThrowingTaskGroup(of: [(Int, Source)].self) { group in
            for worker in 0..<workers {
                group.addTask {
                    var results: [(Int, Source)] = []
                    for i in stride(from: worker, to: paths.count, by: workers) {
                        let path = paths[i]
                        let logical = URL(fileURLWithPath: path)
                        let resolved = logical.resolvingSymlinksInPath().standardizedFileURL
                        let hash = try digest(resolved)
                        guard logical.resolvingSymlinksInPath().standardizedFileURL == resolved else {
                            throw DeepSkyRecipeError.inputsChanged
                        }
                        results.append((i, Source(path: path, resolvedPath: resolved.path, sha256: hash)))
                    }
                    return results
                }
            }
            var results: [(Int, Source)] = []
            for try await batch in group { results += batch }
            return results
        }
        return groups.sorted { $0.0 < $1.0 }.map(\.1)
    }

    private static func digest(_ url: URL) throws -> String {
        try Task.checkCancellation()
        let values = try url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
        guard values.isRegularFile == true, values.isSymbolicLink != true else { throw DeepSkyRecipeError.invalidReport }
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        var hash = SHA256()
        while let data = try file.read(upToCount: 4 * 1024 * 1024), !data.isEmpty {
            try Task.checkCancellation()
            hash.update(data: data)
        }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }

    static func quotedPaths(_ text: String, allowEmpty: Bool = false) throws -> [String] {
        var index = text.startIndex
        var result: [String] = []
        while index < text.endIndex {
            while index < text.endIndex, text[index].isWhitespace { text.formIndex(after: &index) }
            if index == text.endIndex { break }
            guard text[index] == "\"" else { throw DeepSkyRecipeError.invalidReport }
            text.formIndex(after: &index)
            var path = "", closed = false
            while index < text.endIndex {
                let character = text[index]; text.formIndex(after: &index)
                if character == "\"" { closed = true; break }
                if character == "\\" {
                    guard index < text.endIndex else { throw DeepSkyRecipeError.invalidReport }
                    path.append(text[index]); text.formIndex(after: &index)
                } else { path.append(character) }
            }
            guard closed, allowEmpty || !path.isEmpty else { throw DeepSkyRecipeError.invalidReport }
            result.append(path)
        }
        return result
    }
}
