import AppKit
import Foundation
import PhotonStackProcessing

struct SkyAnnotationSettings: Equatable {
    var category = "all"
    var query = ""
    var limitingMagnitude = 18.0
    var includeUnknownMagnitude = true
    var maxObjects = 200
    var previewWidth = 1600
    var showOutlines = true
    var showLabels = true
    var valid: Bool { ["all", "galaxies", "nebulae", "clusters", "other"].contains(category) && query.utf8.count <= 256 && limitingMagnitude.isFinite && (-30...60).contains(limitingMagnitude) && (1...1000).contains(maxObjects) && (320...4096).contains(previewWidth) }
    var arguments: [String] { ["--category", category, "--query", query, "--limiting-magnitude", String(limitingMagnitude), "--include-unknown", includeUnknownMagnitude ? "1" : "0", "--max-objects", String(maxObjects), "--preview-width", String(previewWidth), "--outlines", showOutlines ? "1" : "0", "--labels", showLabels ? "1" : "0"] }
}
struct SkyAnnotationReport: Decodable {
    struct Object: Decodable, Identifiable {
        var id, label, type, category, commonNames, magnitudeBand, outlineKind: String
        var ra, dec, majorArcmin, minorArcmin, x, y, labelX, labelY, labelWidth, anchorX, anchorY: Double
        var magnitude, positionAngle: Double?
        var centerVisible, invalidPositionAngle, labelVisible: Bool
        var paths: [[[Double]]]
        func valid(width: Int, height: Int, previewWidth: Int, previewHeight: Int) -> Bool {
            !id.isEmpty && id.count <= 64 && label.count <= 128 && [ra, dec, majorArcmin, minorArcmin, x, y, labelX, labelY, labelWidth, anchorX, anchorY].allSatisfy(\.isFinite) &&
            (0..<360).contains(ra) && (-90...90).contains(dec) && (0...1800).contains(majorArcmin) && (0...1800).contains(minorArcmin) &&
            (magnitude == nil || (magnitude!.isFinite && (-30...60).contains(magnitude!))) &&
            (positionAngle == nil || (positionAngle!.isFinite && (0..<180).contains(positionAngle!))) &&
            ["none", "catalog-ellipse", "published-contour"].contains(outlineKind) &&
            (!labelVisible || (labelX >= 0 && labelY >= 0 && labelWidth >= 0 && labelX+labelWidth <= Double(previewWidth) && labelY+20 <= Double(previewHeight))) &&
            paths.count <= 500_000 && paths.allSatisfy { $0.count >= 2 && $0.count <= 500_000 && $0.allSatisfy { $0.count == 2 && $0.allSatisfy(\.isFinite) && $0[0] >= -0.500001 && $0[1] >= -0.500001 && $0[0] <= Double(width)-0.499999 && $0[1] <= Double(height)-0.499999 } }
        }
    }
    var type, command, source, sourceSHA, catalogRevision, catalogFrame, projectionFrame, category, query: String
    var width, height, previewWidth, previewHeight, requestedPreviewWidth, matched, maxObjects: Int
    var limitingMagnitude: Double
    var includeUnknownMagnitude, linearDisplayPreview, approximateCatalogGeometry, showOutlines, showLabels: Bool
    var objects: [Object]
    var settings: SkyAnnotationSettings { .init(category: category, query: query, limitingMagnitude: limitingMagnitude, includeUnknownMagnitude: includeUnknownMagnitude, maxObjects: maxObjects, previewWidth: requestedPreviewWidth, showOutlines: showOutlines, showLabels: showLabels) }
    var valid: Bool {
        type == "complete" && command == "sky-annotate" && source.hasPrefix("/") && sourceSHA.count == 64 && sourceSHA.allSatisfy { "0123456789abcdef".contains($0) } &&
        catalogRevision.count == 40 && catalogRevision.allSatisfy { "0123456789abcdef".contains($0) } && catalogFrame == "published-J2000" && projectionFrame == "ICRS" && approximateCatalogGeometry &&
        settings.valid && (16...100_000).contains(width) && (16...100_000).contains(height) && (1...4096).contains(previewWidth) && (1...4096).contains(previewHeight) &&
        matched >= objects.count && objects.count <= maxObjects && Set(objects.map(\.id)).count == objects.count &&
        objects.allSatisfy { $0.valid(width: width, height: height, previewWidth: previewWidth, previewHeight: previewHeight) }
    }
}
@MainActor final class SkyAnnotationSession: ObservableObject {
    @Published var settings = SkyAnnotationSettings()
    @Published private(set) var source: URL
    @Published private(set) var report: SkyAnnotationReport?
    @Published private(set) var output: URL?
    @Published private(set) var image: NSImage?
    @Published private(set) var busy = false
    @Published var error: String?
    private var accepted: SkyAnnotationSettings?
    private let service: any ProcessingService
    private let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStack-SkyAnnotation-\(UUID())")
    private var task: Task<Void, Never>?
    var isCurrent: Bool { report != nil && accepted == settings }
    init(source: URL, service: any ProcessingService = CLIProcessingService.makeDefault()) { self.source = source.resolvingSymlinksInPath(); self.service = service }
    func choose(_ url: URL) { guard !busy else { return }; source = url.resolvingSymlinksInPath(); report = nil; output = nil; image = nil; accepted = nil; error = nil }
    func begin() { guard !busy, task == nil else { return }; task = Task { defer { task = nil }; do { try await annotate(); error = nil } catch is CancellationError { } catch { if !Task.isCancelled { self.error = error.localizedDescription } } } }
    func cancel() { task?.cancel() }
    func close() { let pending = task; pending?.cancel(); let folder = root; Task { await pending?.value; try? FileManager.default.removeItem(at: folder) } }
    private func decode(_ text: String) throws -> SkyAnnotationReport {
        let r = try JSONDecoder().decode(SkyAnnotationReport.self, from: Data(text.utf8))
        guard r.valid else { throw ProcessingServiceError.invalidReport(command: "sky-annotate") }; return r
    }
    private func load(_ folder: URL) throws -> NSImage { guard let image = NSImage(contentsOf: folder.appendingPathComponent("preview.png")) else { throw ProcessingServiceError.invalidReport(command: "sky-annotation-preview") }; return image }
    func annotate() async throws {
        guard !busy, settings.valid else { throw ProcessingServiceError.invalidArguments("Check annotation filters and limits.") }
        busy = true; defer { busy = false }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let folder = root.appendingPathComponent(UUID().uuidString), requested = settings
        do {
            let result = try await service.runCLI(arguments: ["sky-annotate", "--input", source.path, "--output-dir", folder.path]+requested.arguments)
            try Task.checkCancellation(); let r = try decode(result.standardOutput)
            guard URL(fileURLWithPath: r.source).resolvingSymlinksInPath() == source, r.settings == requested else { throw ProcessingServiceError.invalidReport(command: "sky-annotate") }
            let rendered = try load(folder), previous = output
            report = r; output = folder; image = rendered; accepted = requested
            if let previous, previous.deletingLastPathComponent() == root { try? FileManager.default.removeItem(at: previous) }
        } catch { try? FileManager.default.removeItem(at: folder); throw error }
    }
    func open(_ folder: URL) async throws {
        guard !busy else { throw ProcessingServiceError.invalidArguments("Annotation is busy.") }; busy = true; defer { busy = false }
        let result = try await service.runCLI(arguments: ["sky-annotation-inspect", "--recipe", folder.appendingPathComponent("recipe.txt").path])
        try Task.checkCancellation(); let r = try decode(result.standardOutput), rendered = try load(folder)
        source = URL(fileURLWithPath: r.source).resolvingSymlinksInPath(); settings = r.settings; accepted = settings; report = r; output = folder; image = rendered
    }
    func save(to folder: URL) async throws -> URL {
        guard !busy, isCurrent, let output, let report else { throw ProcessingServiceError.invalidArguments("Update and review annotations before exporting.") }; busy = true; defer { busy = false }
        let result = try await service.runCLI(arguments: ["sky-annotation-export", "--recipe", output.appendingPathComponent("recipe.txt").path, "--output-dir", folder.path])
        try Task.checkCancellation(); let r = try decode(result.standardOutput)
        guard r.source == report.source && r.sourceSHA == report.sourceSHA && r.settings == report.settings && r.catalogRevision == report.catalogRevision else { throw ProcessingServiceError.invalidReport(command: "sky-annotation-export") }
        return folder.appendingPathComponent("annotated.svg")
    }
}
