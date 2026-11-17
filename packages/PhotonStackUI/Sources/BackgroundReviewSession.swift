import AppKit
import Foundation
import Darwin
import PhotonStackProcessing

struct BackgroundReviewPoint: Codable, Equatable, Sendable, Identifiable {
    var id = UUID()
    var x: Double
    var y: Double
    var radius: Int = 4
    var enabled = true
    init(x: Double, y: Double, radius: Int = 4, enabled: Bool = true) {
        self.x = x; self.y = y; self.radius = radius; self.enabled = enabled
    }
    enum CodingKeys: String, CodingKey { case id, x, y, radius, enabled }
    init(from decoder: Decoder) throws {
        let box = try decoder.container(keyedBy: CodingKeys.self)
        id = try box.decodeIfPresent(UUID.self, forKey: .id) ?? UUID()
        x = try box.decode(Double.self, forKey: .x); y = try box.decode(Double.self, forKey: .y)
        radius = try box.decode(Int.self, forKey: .radius); enabled = try box.decode(Bool.self, forKey: .enabled)
    }
}
struct BackgroundReviewRecipe: Codable, Equatable, Sendable {
    var version = 2
    var model = "polynomial"
    var rbfSmoothing = 0.001
    var rbfRejectOutliers = true
    var source: URL
    var sourceSHA: String
    var width: Int
    var height: Int
    var degree = 2
    var strength = 0.35
    var preserveBrightness = true
    var samples: [BackgroundReviewPoint]
    var isValid: Bool {
        version == 2 && ["polynomial", "rbf"].contains(model) &&
        rbfSmoothing.isFinite && (0...1).contains(rbfSmoothing) && source.isFileURL && sourceSHA.count == 64 &&
        sourceSHA.allSatisfy(\.isHexDigit) && width >= 16 && height >= 16 &&
        (1...4).contains(degree) && strength.isFinite && (0...1).contains(strength) && samples.count <= 4096 && Set(samples.map(\.id)).count == samples.count &&
        samples.allSatisfy {
            $0.x.isFinite && $0.y.isFinite && $0.x >= 0 && $0.y >= 0 &&
            $0.x < Double(width) && $0.y < Double(height) && (1...64).contains($0.radius)
        }
    }
}
extension BackgroundReviewRecipe {
    enum CodingKeys: String, CodingKey {
        case version, model, rbfSmoothing, rbfRejectOutliers, source, sourceSHA, width, height
        case degree, strength, preserveBrightness, samples
    }
    init(from decoder: Decoder) throws {
        let box = try decoder.container(keyedBy: CodingKeys.self)
        let oldVersion = try box.decode(Int.self, forKey: .version)
        guard oldVersion == 1 || oldVersion == 2 else {
            throw DecodingError.dataCorruptedError(forKey: .version, in: box, debugDescription: "Unsupported background recipe version.")
        }
        if oldVersion == 2 {
            model = try box.decode(String.self, forKey: .model)
            rbfSmoothing = try box.decode(Double.self, forKey: .rbfSmoothing)
            rbfRejectOutliers = try box.decode(Bool.self, forKey: .rbfRejectOutliers)
        } else if box.contains(.model) || box.contains(.rbfSmoothing) || box.contains(.rbfRejectOutliers) {
            throw DecodingError.dataCorruptedError(forKey: .version, in: box, debugDescription: "RBF requires recipe version 2.")
        }
        version = 2
        source = try box.decode(URL.self, forKey: .source)
        sourceSHA = try box.decode(String.self, forKey: .sourceSHA)
        width = try box.decode(Int.self, forKey: .width); height = try box.decode(Int.self, forKey: .height)
        degree = try box.decode(Int.self, forKey: .degree)
        strength = try box.decode(Double.self, forKey: .strength)
        preserveBrightness = try box.decode(Bool.self, forKey: .preserveBrightness)
        samples = try box.decode([BackgroundReviewPoint].self, forKey: .samples)
    }
}
struct BackgroundReviewReport: Decodable {
    struct Measurement: Decodable {
        var x: Double, y: Double
        var radius: Int
        var enabled: Bool
        var status: String
        var measured: [Double], fitted: [Double], used: [Bool]
        var validationResidual: [Double]?
        var point: BackgroundReviewPoint { .init(x: x, y: y, radius: radius, enabled: enabled) }
    }
    var width: Int, height: Int
    var sourceSHA: String
    var residualRMS: [Double]
    var validationRMS: [Double]?
    var model: String?
    var wcsPreserved: Bool
    var samples: [Measurement]
}

@MainActor
final class BackgroundReviewSession: ObservableObject {
    @Published var recipe: BackgroundReviewRecipe?
    @Published private(set) var report: BackgroundReviewReport?
    @Published private(set) var output: URL?
    @Published private(set) var renderedRecipe: BackgroundReviewRecipe?
    @Published private(set) var image: NSImage?
    @Published private(set) var busy = false
    @Published var error: String?
    @Published var display = "source" { didSet { loadPreview() } }
    @Published var selected: Int?
    @Published var editMode = "add"
    @Published var showSamples = true
    private(set) var source: URL
    private let service: any ProcessingService
    private let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStack-Background-\(UUID())")
    private var task: Task<Void, Never>?

    init(source: URL, service: any ProcessingService = CLIProcessingService.makeDefault()) {
        self.source = source
        self.service = service
    }
    var isCurrent: Bool { renderedRecipe != nil && renderedRecipe == recipe }
    func begin(inspect: Bool = false) {
        guard !busy else { return }
        busy = true
        task = Task {
            defer { busy = false; task = nil }
            do { try await evaluate(inspect: inspect); error = nil }
            catch is CancellationError { }
            catch { if !Task.isCancelled { self.error = error.localizedDescription } }
        }
    }
    func cancel() { task?.cancel() }
    func close() {
        let pending = task
        pending?.cancel()
        let folder = root
        Task {
            await pending?.value
            try? FileManager.default.removeItem(at: folder)
        }
    }
    func evaluate(inspect: Bool) async throws {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let directory = root.appendingPathComponent(UUID().uuidString)
        let requested = recipe
        renderedRecipe = nil
        var sampleFile: URL?
        defer { if let sampleFile { try? FileManager.default.removeItem(at: sampleFile) } }
        var args = ["background-review", "--input", source.path, "--output-dir", directory.path]
        if let requested {
            guard requested.isValid else { throw ProcessingServiceError.invalidArguments("Invalid background recipe.") }
            let rows = "PhotonStack-BackgroundSamples-1\n" + requested.samples.map {
                "\($0.x) \($0.y) \($0.radius) \($0.enabled ? 1 : 0)\n"
            }.joined()
            let file = root.appendingPathComponent("samples-\(UUID()).txt")
            sampleFile = file
            try rows.write(to: file, atomically: true, encoding: .utf8)
            args += ["--model", requested.model, "--samples", file.path, "--degree", String(requested.degree), "--strength", String(requested.strength),
                     "--preserve-brightness", requested.preserveBrightness ? "on" : "off", "--expected-sha", requested.sourceSHA]
        }
        if let requested, requested.model == "rbf" {
            args += ["--rbf-smoothing", String(requested.rbfSmoothing),
                     "--rbf-reject", requested.rbfRejectOutliers ? "on" : "off"]
        }
        if inspect { args += ["--inspect-only", "on"] }
        _ = try await service.runCLI(arguments: args)
        try Task.checkCancellation()
        let report = try JSONDecoder().decode(BackgroundReviewReport.self, from: Data(contentsOf: directory.appendingPathComponent("report.json")))
        if let requested, (report.model ?? "polynomial") != requested.model {
            throw ProcessingServiceError.invalidArguments("The processing tool did not use the requested background model.")
        }
        if let requested, (requested.width != report.width || requested.height != report.height) {
            throw ProcessingServiceError.invalidArguments("The recipe dimensions differ from the source image.")
        }
        let validated = requested ?? BackgroundReviewRecipe(source: source, sourceSHA: report.sourceSHA,
            width: report.width, height: report.height, samples: report.samples.map(\.point))
        if recipe == requested { recipe = validated }
        let previous = output
        self.report = report
        output = directory
        renderedRecipe = inspect ? nil : validated
        loadPreview()
        if let previous, previous != directory {
            await Task.detached { try? FileManager.default.removeItem(at: previous) }.value
        }
    }
    func loadRecipe(_ file: URL) throws {
        guard !busy, (try file.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? Int.max) <= 1_048_576 else {
            throw ProcessingServiceError.invalidArguments("Invalid background recipe file.")
        }
        let saved = try JSONDecoder().decode(BackgroundReviewRecipe.self, from: Data(contentsOf: file))
        guard saved.isValid else { throw ProcessingServiceError.invalidArguments("Invalid background recipe.") }
        let previous = output
        if let previous { Task.detached { try? FileManager.default.removeItem(at: previous) } }
        source = saved.source
        recipe = saved
        renderedRecipe = nil
        report = nil; output = nil; image = nil; selected = nil; display = "source"
    }
    func saveRecipe(to file: URL) throws {
        guard let recipe, recipe.isValid else { throw ProcessingServiceError.invalidArguments("Load an image first.") }
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(recipe).write(to: file, options: .atomic)
    }
    /// Copies a reviewed result to a newly claimed, durable directory. Never edits the input.
    func saveResult(to destination: URL) async throws -> URL {
        guard isCurrent, let output, let recipe else {
            throw ProcessingServiceError.invalidArguments("Update the background preview before saving.")
        }
        let recipeBytes = try JSONEncoder().encode(recipe)
        try await Task.detached {
            guard mkdir(destination.path, 0o755) == 0 else {
                throw CocoaError(.fileWriteFileExists, userInfo: [NSFilePathErrorKey: destination.path])
            }
            do {
                for name in ["corrected.fits", "model.fits", "difference.fits", "report.json"] {
                    try Task.checkCancellation()
                    try FileManager.default.copyItem(at: output.appendingPathComponent(name), to: destination.appendingPathComponent(name))
                }
                try recipeBytes.write(to: destination.appendingPathComponent("recipe.json"), options: .atomic)
            } catch {
                try? FileManager.default.removeItem(at: destination)
                throw error
            }
        }.value
        return destination.appendingPathComponent("corrected.fits")
    }
    func point(id: UUID) -> BackgroundReviewPoint? { recipe?.samples.first { $0.id == id } }
    func updatePoint<Value>(id: UUID, key: WritableKeyPath<BackgroundReviewPoint, Value>, value: Value) {
        guard var recipe, let i = recipe.samples.firstIndex(where: { $0.id == id }) else { return }
        recipe.samples[i][keyPath: key] = value
        self.recipe = recipe
    }
    func removePoint(id: UUID) {
        guard var recipe else { return }
        selected = nil
        recipe.samples.removeAll { $0.id == id }
        self.recipe = recipe
    }
    func resetGrid() {
        guard var value = recipe else { return }
        value.samples = (0..<8).flatMap { y in (0..<12).map { x in
            BackgroundReviewPoint(x: floor((Double(x)+0.5)*Double(value.width)/12),
                y: floor((Double(y)+0.5)*Double(value.height)/8))
        }}
        recipe = value; selected = nil
    }
    func tap(x: Double, y: Double, tolerance: Double) {
        guard !busy, var value = recipe, x >= 0, y >= 0, x < Double(value.width), y < Double(value.height) else { return }
        let near = value.samples.indices.min {
            hypot(value.samples[$0].x-x, value.samples[$0].y-y) < hypot(value.samples[$1].x-x, value.samples[$1].y-y)
        }
        if let near, hypot(value.samples[near].x-x, value.samples[near].y-y) < tolerance {
            if editMode == "remove" { value.samples.remove(at: near); selected = nil }
            else { selected = near }
        } else if editMode == "add", value.samples.count < 4096 {
            value.samples.append(.init(x: x.rounded().clamped(to: 0...Double(value.width-1)),
                                      y: y.rounded().clamped(to: 0...Double(value.height-1))))
            selected = value.samples.count-1
        }
        recipe = value
    }
    private func loadPreview() {
        guard let output else { image = nil; return }
        let url = output.appendingPathComponent(display + ".tiff")
        image = NSImage(contentsOf: url)
    }
}
private extension Double {
    func clamped(to range: ClosedRange<Double>) -> Double { min(range.upperBound, max(range.lowerBound, self)) }
}
