import AppKit
import Foundation
import PhotonStackProcessing

struct PlateSolveSettings: Equatable, Sendable {
    var solver = "", indices: [String] = []
    var icrsConfirmed = false, useScaleHint = false
    var scaleLow = 1.0, scaleHigh = 5.0, timeout = 120
    var isValid: Bool {
        solver.hasPrefix("/") && !indices.isEmpty && indices.count <= 128 &&
        Set(indices).count == indices.count && indices.allSatisfy { $0.hasPrefix("/") } && icrsConfirmed &&
        (1...600).contains(timeout) && (!useScaleHint || (scaleLow.isFinite && scaleHigh.isFinite && scaleLow > 0 && scaleLow < scaleHigh && scaleHigh <= 3600))
    }
    var arguments: [String] {
        var args = ["--solver", solver, "--index-frame", "ICRS", "--timeout", String(timeout)]
        for path in indices { args += ["--index", path] }
        if useScaleHint { args += ["--scale-low", String(scaleLow), "--scale-high", String(scaleHigh)] }
        return args
    }
}
struct PlateSolveReport: Decodable {
    struct Summary: Decodable {
        var count, regions: Int
        var medianPixels, p95Pixels, maximumPixels, spanX, spanY: Double
        var valid: Bool {
            count >= 6 && (3...16).contains(regions) && [medianPixels, p95Pixels, maximumPixels, spanX, spanY].allSatisfy(\.isFinite) &&
            medianPixels >= 0 && medianPixels <= p95Pixels && p95Pixels <= maximumPixels && p95Pixels <= 2 &&
            (0.3...1).contains(spanX) && (0.3...1).contains(spanY)
        }
    }
    struct Star: Decodable { var id: Int; var x, y: Double; var heldOut: Bool }
    struct Residual: Decodable { var source: Int; var predictedX, predictedY, distance: Double }
    var type, command, source, sourceSHA, backend, referenceFrame, solver: String
    var indices: [String]
    var timeout, width, height, detectedAccepted, submitted, heldOut: Int
    var scaleLow, scaleHigh, centerRA, centerDec, scaleArcsecPerPixel, minimumFitMatchWeight, elapsedSeconds: Double
    var indexFrameUserDeclared, blindPosition, originalWcsIgnored, pixelsPreserved: Bool
    var fitted, validation: Summary
    var stars: [Star]; var residuals: [Residual]
    var settings: PlateSolveSettings {
        .init(solver: solver, indices: indices, icrsConfirmed: true, useScaleHint: scaleLow > 0,
              scaleLow: scaleLow > 0 ? scaleLow : 1, scaleHigh: scaleHigh > 0 ? scaleHigh : 5, timeout: timeout)
    }
    var valid: Bool {
        type == "complete" && command == "plate-solve" && backend == "local-astrometry-net" && referenceFrame == "ICRS" &&
        source.hasPrefix("/") && sourceSHA.count == 64 && sourceSHA.allSatisfy { "0123456789abcdef".contains($0) } &&
        settings.isValid && width >= 16 && height >= 16 && width <= 100_000 && height <= 100_000 &&
        indexFrameUserDeclared && blindPosition && originalWcsIgnored && pixelsPreserved &&
        centerRA.isFinite && (0..<360).contains(centerRA) && centerDec.isFinite && (-90...90).contains(centerDec) &&
        scaleArcsecPerPixel.isFinite && scaleArcsecPerPixel > 0 && elapsedSeconds.isFinite && elapsedSeconds >= 0 && minimumFitMatchWeight == 0.9 &&
        fitted.valid && fitted.count >= 12 && fitted.regions >= 4 && fitted.maximumPixels <= 5 && validation.valid &&
        stars.count == detectedAccepted && stars.count <= 2000 && submitted+heldOut == stars.count &&
        stars.filter(\.heldOut).count == heldOut && stars.enumerated().allSatisfy { i, s in s.id == i && s.x.isFinite && s.y.isFinite && s.x >= 0 && s.x < Double(width) && s.y >= 0 && s.y < Double(height) } &&
        residuals.count == fitted.count+validation.count && Set(residuals.map(\.source)).count == residuals.count &&
        residuals.allSatisfy { stars.indices.contains($0.source) && $0.predictedX.isFinite && $0.predictedY.isFinite && $0.distance.isFinite && $0.distance >= 0 }
    }
}

@MainActor final class PlateSolveSession: ObservableObject {
    @Published var settings = PlateSolveSettings()
    @Published private(set) var source: URL
    @Published private(set) var report: PlateSolveReport?
    @Published private(set) var output: URL?
    @Published private(set) var image: NSImage?
    @Published private(set) var busy = false
    @Published var error: String?
    private var acceptedSettings: PlateSolveSettings?
    private let service: any ProcessingService
    private let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStack-PlateSolve-\(UUID())")
    private var task: Task<Void, Never>?
    var isCurrent: Bool { report != nil && acceptedSettings == settings }
    init(source: URL, service: any ProcessingService = CLIProcessingService.makeDefault()) {
        self.source = source.resolvingSymlinksInPath(); self.service = service
    }
    func choose(_ url: URL) { guard !busy else { return }; source = url.resolvingSymlinksInPath(); report = nil; output = nil; image = nil; acceptedSettings = nil; error = nil }
    func begin() {
        guard !busy, task == nil else { return }
        task = Task { defer { task = nil }; do { try await solve(); error = nil } catch is CancellationError { } catch { if !Task.isCancelled { self.error = error.localizedDescription } } }
    }
    func cancel() { task?.cancel() }
    func close() { let pending = task; pending?.cancel(); let folder = root; Task { await pending?.value; try? FileManager.default.removeItem(at: folder) } }
    private func decode(_ text: String) throws -> PlateSolveReport {
        let r = try JSONDecoder().decode(PlateSolveReport.self, from: Data(text.utf8))
        guard r.valid else { throw ProcessingServiceError.invalidReport(command: "plate-solve") }; return r
    }
    private func preview(_ folder: URL) async throws -> NSImage {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let file = root.appendingPathComponent(UUID().uuidString+".tiff")
        defer { try? FileManager.default.removeItem(at: file) }
        _ = try await service.runCLI(arguments: ["review-preview", "--input", folder.appendingPathComponent("solved.fits").path, "--output", file.path])
        try Task.checkCancellation()
        guard let image = NSImage(contentsOf: file) else { throw ProcessingServiceError.invalidReport(command: "review-preview") }; return image
    }
    func solve() async throws {
        guard !busy, settings.isValid else { throw ProcessingServiceError.invalidArguments("Select solve-field and ICRS indices, and check the scale and time limit.") }
        busy = true; defer { busy = false }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let folder = root.appendingPathComponent(UUID().uuidString), requested = settings
        do {
            let result = try await service.runCLI(arguments: ["plate-solve", "--input", source.path, "--output-dir", folder.path]+requested.arguments)
            try Task.checkCancellation(); let r = try decode(result.standardOutput)
            guard URL(fileURLWithPath: r.source).resolvingSymlinksInPath() == source,
                  URL(fileURLWithPath: r.solver).resolvingSymlinksInPath() == URL(fileURLWithPath: requested.solver).resolvingSymlinksInPath(),
                  r.indices.map({ URL(fileURLWithPath: $0).resolvingSymlinksInPath() }) == requested.indices.map({ URL(fileURLWithPath: $0).resolvingSymlinksInPath() }),
                  r.timeout == requested.timeout, r.scaleLow == (requested.useScaleHint ? requested.scaleLow : 0), r.scaleHigh == (requested.useScaleHint ? requested.scaleHigh : 0) else {
                throw ProcessingServiceError.invalidReport(command: "plate-solve")
            }
            let rendered = try await preview(folder)
            let previous = output; report = r; output = folder; image = rendered; acceptedSettings = requested
            if let previous, previous.deletingLastPathComponent() == root { try? FileManager.default.removeItem(at: previous) }
        } catch { try? FileManager.default.removeItem(at: folder); throw error }
    }
    func open(_ folder: URL) async throws {
        guard !busy else { throw ProcessingServiceError.invalidArguments("Plate solving is busy.") }
        busy = true; defer { busy = false }
        let result = try await service.runCLI(arguments: ["plate-solve-inspect", "--recipe", folder.appendingPathComponent("recipe.txt").path])
        try Task.checkCancellation(); let r = try decode(result.standardOutput), rendered = try await preview(folder)
        source = URL(fileURLWithPath: r.source).resolvingSymlinksInPath(); settings = r.settings; acceptedSettings = settings; report = r; output = folder; image = rendered
    }
    func save(to folder: URL) async throws -> URL {
        guard !busy, isCurrent, let output, let report else { throw ProcessingServiceError.invalidArguments("Solve and review the current settings before saving.") }
        busy = true; defer { busy = false }
        let response = try await service.runCLI(arguments: ["plate-solve-export", "--recipe", output.appendingPathComponent("recipe.txt").path, "--output-dir", folder.path])
        try Task.checkCancellation(); let saved = try decode(response.standardOutput)
        guard saved.sourceSHA == report.sourceSHA && saved.source == report.source else { throw ProcessingServiceError.invalidReport(command: "plate-solve-export") }
        return folder.appendingPathComponent("solved.fits")
    }
}
