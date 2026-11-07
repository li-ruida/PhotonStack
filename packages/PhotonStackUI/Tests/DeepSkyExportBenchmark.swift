import CryptoKit
import Foundation
import PhotonStackAppCore
import PhotonStackProcessing
import Testing
@testable import PhotonStackUI

/// Opt-in real-data acceptance and benchmark. Inputs are read only; all outputs
/// go to a new explicitly supplied directory, retained for inspection.
@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_EXPORT_BENCHMARK_RECIPE"] != nil)) @MainActor
func deepSkyVerifiedExportRealDataBenchmark() async throws {
    let environment = ProcessInfo.processInfo.environment
    let cli = URL(fileURLWithPath: try #require(environment["PHOTONSTACK_TEST_CLI"]))
    let recipe = URL(fileURLWithPath: try #require(environment["PHOTONSTACK_EXPORT_BENCHMARK_RECIPE"]))
    let root = URL(fileURLWithPath: try #require(environment["PHOTONSTACK_EXPORT_BENCHMARK_ROOT"]))
    try #require(!FileManager.default.fileExists(atPath: root.path))
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let service = CLIProcessingService(executableURL: cli)
    let inspected = try await service.inspectDeepSkyRecipe(recipe)
    func digest(_ url: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var hash = SHA256()
        while let data = try handle.read(upToCount: 1_048_576), !data.isEmpty { hash.update(data: data) }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }
    let inputURLs = inspected.inputs.map { URL(fileURLWithPath: $0) }
    let originals = try inputURLs.map(digest)
    let previews = root.appendingPathComponent("previews")
    let model = PhotonStackWorkspaceModel(project: PhotonStackProject(name: "Verified export benchmark"), autosaveEnabled: false,
        processingService: service, previewDirectory: previews, cacheBuildIdentity: "export-reuse-benchmark-v1")
    await model.importDeepSkyRecipe(from: recipe)
    try #require(model.errorMessage == nil)
    await model.runDeepSkyWorkflow(analyzeOnly: false)
    try #require(model.errorMessage == nil, "\(model.errorMessage ?? "")")
    let report = try #require(model.deepSkySettings.lastReport)
    let receipt = URL(fileURLWithPath: report.developed).deletingLastPathComponent().appendingPathComponent(DeepSkyResultReuse.receiptFileName)
    let heldReceipt = receipt.appendingPathExtension("benchmark-held")
    try #require(FileManager.default.fileExists(atPath: receipt.path))
    let receiptObject = try #require(JSONSerialization.jsonObject(with: Data(contentsOf: receipt)) as? [String: Any])
    let receiptVersion = try #require(receiptObject["version"] as? Int)
    #expect(receiptVersion == 2)
    defer {
        if FileManager.default.fileExists(atPath: heldReceipt.path) { try? FileManager.default.moveItem(at: heldReceipt, to: receipt) }
    }
    func directoryBytes(_ directory: URL) -> Int64 {
        var bytes: Int64 = 0
        if let files = FileManager.default.enumerator(at: directory, includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey]) {
            for case let file as URL in files {
                if let values = try? file.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey]), values.isRegularFile == true {
                    bytes += Int64(values.fileSize ?? 0)
                }
            }
        }
        return bytes
    }
    func export(_ name: String) async throws -> (seconds: Double, peakReplayBytes: Int64, observedReplay: Bool, url: URL) {
        var peak: Int64 = 0, observed = false
        let monitor = Task { @MainActor in
            while !Task.isCancelled {
                let directories = (try? FileManager.default.contentsOfDirectory(at: previews, includingPropertiesForKeys: nil)) ?? []
                var bytes: Int64 = 0
                for directory in directories where directory.lastPathComponent.hasPrefix("replay-deep-sky-") {
                    observed = true
                    bytes += directoryBytes(directory)
                }
                peak = max(peak, bytes)
                try? await Task.sleep(for: .milliseconds(20))
            }
        }
        let output = root.appendingPathComponent(name + ".tiff")
        let started = ContinuousClock.now
        await model.exportCurrentImage(to: output)
        let duration = started.duration(to: .now).components
        monitor.cancel(); await monitor.value
        try #require(model.errorMessage == nil, "\(model.errorMessage ?? "")")
        return (Double(duration.seconds) + Double(duration.attoseconds) / 1e18, peak, observed, output)
    }
    try FileManager.default.moveItem(at: receipt, to: heldReceipt)
    let baseline = try await export("baseline-rebuild")
    try FileManager.default.moveItem(at: heldReceipt, to: receipt)
    let reused = try await export("verified-reuse")
    let sourcesUnchanged = try inputURLs.map(digest) == originals
    let identical = try digest(baseline.url) == digest(reused.url)
    #expect(sourcesUnchanged)
    #expect(identical)
    #expect(baseline.observedReplay && !reused.observedReplay)
    #expect(baseline.peakReplayBytes > 0 && reused.peakReplayBytes == 0)
    #expect(model.deepSkySettings.lastReport == report)
    let summary: [String: Any] = [
        "inputCount": inputURLs.count, "sourcesUnchanged": sourcesUnchanged, "exportBytesIdentical": identical,
        "baselineSeconds": baseline.seconds, "verifiedReuseSeconds": reused.seconds,
        "speedup": baseline.seconds / max(reused.seconds, 0.000001),
        "baselinePeakReplayBytes": baseline.peakReplayBytes, "verifiedReusePeakReplayBytes": reused.peakReplayBytes,
        "baselineRestacked": baseline.observedReplay, "verifiedReuseRestacked": reused.observedReplay,
        "engineSHA256": try digest(cli),
        "receiptVersion": receiptVersion,
    ]
    try JSONSerialization.data(withJSONObject: summary, options: [.prettyPrinted, .sortedKeys])
        .write(to: root.appendingPathComponent("benchmark.json"), options: .atomic)
    print("Verified export benchmark: \(baseline.seconds)s → \(reused.seconds)s; replay bytes \(baseline.peakReplayBytes) → \(reused.peakReplayBytes)")
}
