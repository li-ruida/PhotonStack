import AppKit
import Foundation
import PhotonStackProcessing

struct GeneralizedStretchSettings: Equatable, Sendable {
    var logStretch = 3.0, localIntensity = 1.0
    var symmetry = 0.0, shadowProtection = 0.0, highlightProtection = 1.0
    var black = 0.0, white = 1.0
    var luminance = true, automaticRange = true
    var isValid: Bool {
        [logStretch, localIntensity, symmetry, shadowProtection, highlightProtection, black, white].allSatisfy(\.isFinite) &&
        (0...10).contains(logStretch) && (-5...15).contains(localIntensity) && shadowProtection >= 0 &&
        shadowProtection <= symmetry && symmetry <= highlightProtection && highlightProtection <= 1 &&
        (automaticRange || (black < white && (white-black).isFinite))
    }
    var arguments: [String] {
        var a = ["--log-stretch", String(logStretch), "--local-intensity", String(localIntensity),
                 "--symmetry", String(symmetry), "--shadow-protection", String(shadowProtection),
                 "--highlight-protection", String(highlightProtection), "--mode", luminance ? "luminance" : "independent"]
        if !automaticRange { a += ["--black", String(black), "--white", String(white)] }
        return a
    }
}
struct GeneralizedStretchReport: Decodable {
    var type, command, source, sourceSHA, sourceDomain, outputDomain: String
    var width, height, previewWidth, coveredPixels, belowBlack, aboveWhite, gamutBlendedPixels: Int
    var logStretch, localIntensity, symmetry, shadowProtection, highlightProtection, black, white: Double
    var luminance, wcsPreserved: Bool
    var beforeHistogram, afterHistogram: [Int]
    var curve: [Double]
    var settings: GeneralizedStretchSettings {
        .init(logStretch: logStretch, localIntensity: localIntensity, symmetry: symmetry,
              shadowProtection: shadowProtection, highlightProtection: highlightProtection,
              black: black, white: white, luminance: luminance, automaticRange: false)
    }
    var isValid: Bool {
        type == "complete" && command == "ghs-stretch" && source.hasPrefix("/") &&
        sourceSHA.count == 64 && sourceSHA.allSatisfy({ "0123456789abcdef".contains($0) }) &&
        ["linear", "display-srgb"].contains(sourceDomain) && outputDomain == "display-srgb" &&
        width > 0 && height > 0 && width <= 100_000 && height <= 100_000 &&
        (0...2048).contains(previewWidth) && coveredPixels > 0 && coveredPixels <= width*height &&
        [belowBlack, aboveWhite, gamutBlendedPixels].allSatisfy({ (0...coveredPixels).contains($0) }) && settings.isValid &&
        [beforeHistogram, afterHistogram].allSatisfy({ $0.count == 256 && $0.allSatisfy { (0...coveredPixels).contains($0) } && $0.reduce(0,+) == coveredPixels }) &&
        curve.count == 1025 && curve.first == 0 && curve.last == 1 &&
        curve.allSatisfy({ $0.isFinite && (0...1).contains($0) }) && zip(curve, curve.dropFirst()).allSatisfy({ $0 <= $1+1e-13 })
    }
}

@MainActor final class GeneralizedStretchSession: ObservableObject {
    @Published var settings = GeneralizedStretchSettings()
    @Published private(set) var source: URL
    @Published private(set) var report: GeneralizedStretchReport?
    @Published private(set) var output: URL?
    @Published private(set) var image: NSImage?
    @Published private(set) var busy = false
    @Published var error: String?
    @Published var before = false { didSet { loadImage() } }
    private let service: any ProcessingService
    private let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStack-GHS-\(UUID())")
    private var task: Task<Void, Never>?
    var isCurrent: Bool { report != nil && settings == report?.settings }
    init(source: URL, service: any ProcessingService = CLIProcessingService.makeDefault()) {
        self.source = source.resolvingSymlinksInPath(); self.service = service
    }
    func choose(_ file: URL) {
        guard !busy else { return }
        source = file.resolvingSymlinksInPath(); settings = .init(); report = nil; output = nil; image = nil; error = nil
    }
    func begin() {
        guard !busy, task == nil else { return }
        task = Task {
            defer { task = nil }
            do { try await updatePreview(); error = nil }
            catch is CancellationError { }
            catch { if !Task.isCancelled { self.error = error.localizedDescription } }
        }
    }
    func cancel() { task?.cancel() }
    func close() {
        let pending = task; pending?.cancel(); let directory = root
        Task { await pending?.value; try? FileManager.default.removeItem(at: directory) }
    }
    private func decode(_ text: String) throws -> GeneralizedStretchReport {
        let value = try JSONDecoder().decode(GeneralizedStretchReport.self, from: Data(text.utf8))
        guard value.isValid else { throw ProcessingServiceError.invalidReport(command: "ghs-stretch") }
        return value
    }
    private func render(_ requested: GeneralizedStretchSettings, to folder: URL, preview: Bool) async throws -> GeneralizedStretchReport {
        guard requested.isValid else { throw ProcessingServiceError.invalidArguments("GHS requires 0 ≤ LP ≤ SP ≤ HP ≤ 1 and black < white.") }
        var args = ["ghs-stretch", "--input", source.path, "--output-dir", folder.path] + requested.arguments
        if preview { args += ["--preview-width", "1200"] }
        if let report { args += ["--expected-sha", report.sourceSHA] }
        let response = try await service.runCLI(arguments: args)
        try Task.checkCancellation()
        let value = try decode(response.standardOutput)
        var resolved = requested
        if requested.automaticRange { resolved.black = value.black; resolved.white = value.white; resolved.automaticRange = false }
        guard value.settings == resolved, URL(fileURLWithPath: value.source).resolvingSymlinksInPath() == source,
              value.previewWidth == (preview ? 1200 : 0), report == nil || value.sourceSHA == report?.sourceSHA else {
            throw ProcessingServiceError.invalidReport(command: "ghs-stretch")
        }
        return value
    }
    func updatePreview() async throws {
        guard !busy else { throw ProcessingServiceError.invalidArguments("GHS is busy.") }
        busy = true; defer { busy = false }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let folder = root.appendingPathComponent(UUID().uuidString), requested = settings
        do {
            let value = try await render(requested, to: folder, preview: true)
            let previous = output
            if settings == requested { settings = value.settings }
            report = value; output = folder; loadImage()
            if let previous, previous.deletingLastPathComponent() == root { try? FileManager.default.removeItem(at: previous) }
        } catch { try? FileManager.default.removeItem(at: folder); throw error }
    }
    func open(_ folder: URL) async throws {
        guard !busy else { throw ProcessingServiceError.invalidArguments("GHS is busy.") }
        busy = true; defer { busy = false }
        let response = try await service.runCLI(arguments: ["ghs-inspect", "--recipe", folder.appendingPathComponent("recipe.txt").path])
        try Task.checkCancellation()
        let value = try decode(response.standardOutput)
        source = URL(fileURLWithPath: value.source).resolvingSymlinksInPath()
        settings = value.settings; report = value; output = folder; loadImage()
    }
    func saveResult(to folder: URL) async throws -> URL {
        guard !busy, isCurrent else { throw ProcessingServiceError.invalidArguments("Update the GHS preview before saving.") }
        busy = true; defer { busy = false }
        _ = try await render(settings, to: folder, preview: false)
        return folder.appendingPathComponent("developed.tiff")
    }
    private func loadImage() {
        image = output.flatMap { NSImage(contentsOf: $0.appendingPathComponent(before ? "before.tiff" : "after.tiff")) }
    }
}
