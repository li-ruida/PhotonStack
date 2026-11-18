import AppKit
import PhotonStackProcessing

@MainActor final class SpectralCalibrationSession: ObservableObject {
    @Published var settings = SpectralCalibrationSettings()
    @Published private(set) var source: URL
    @Published private(set) var profile: URL?
    @Published private(set) var profileName: String?
    @Published private(set) var catalog: URL?
    @Published private(set) var report: SpectralCalibrationReport?
    @Published private(set) var output: URL?
    @Published private(set) var beforeImage: NSImage?
    @Published private(set) var afterImage: NSImage?
    @Published private(set) var busy = false
    @Published var error: String?
    private var acceptedProfile: URL?, acceptedCatalog: URL?
    private let service: any ProcessingService
    private let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStack-SPCC-\(UUID())")
    private var task: Task<Void, Never>?
    var canReview: Bool { settings.isValid && profile != nil && catalog != nil }
    var canExclude: Bool { report != nil && catalog == acceptedCatalog }
    var isCurrent: Bool { report != nil && settings == report?.options && profile == acceptedProfile && catalog == acceptedCatalog }
    init(source: URL, service: any ProcessingService = CLIProcessingService.makeDefault()) {
        self.source = source.resolvingSymlinksInPath(); self.service = service
    }
    func chooseSource(_ url: URL) {
        guard !busy else { return }
        source = url.resolvingSymlinksInPath(); report = nil; output = nil; beforeImage = nil; afterImage = nil
        settings.excluded = []; error = nil
    }
    func chooseProfile(_ url: URL, name: String? = nil) { guard !busy else { return }; profile = url.resolvingSymlinksInPath(); profileName = name; error = nil }
    func chooseCatalog(_ url: URL) { guard !busy else { return }; catalog = url.resolvingSymlinksInPath(); settings.excluded = []; error = nil }
    func exclude(_ id: String, _ excluded: Bool) {
        guard !busy, canExclude, report?.stars.contains(where: { $0.id == id }) == true else { return }
        var ids = Set(settings.excluded); if excluded { ids.insert(id) } else { ids.remove(id) }; settings.excluded = ids.sorted()
    }
    func begin() {
        guard !busy, task == nil else { return }
        task = Task { defer { task = nil }; do { try await review(); error = nil } catch is CancellationError { } catch { if !Task.isCancelled { self.error = error.localizedDescription } } }
    }
    func cancel() { task?.cancel() }
    func close() {
        let pending = task; pending?.cancel(); let folder = root
        Task { await pending?.value; try? FileManager.default.removeItem(at: folder) }
    }
    private func decode(_ text: String, directory: URL) async throws -> SpectralCalibrationReport {
        struct Response: Decodable { var type, command, directory: String; var report: SpectralCalibrationReport }
        let r = try await Task.detached(priority: .userInitiated) {
            let value = try JSONDecoder().decode(Response.self, from: Data(text.utf8))
            guard value.type == "complete", value.command == "spcc", value.report.isValid,
                  URL(fileURLWithPath: value.directory).resolvingSymlinksInPath().path == directory.resolvingSymlinksInPath().path else {
                throw ProcessingServiceError.invalidReport(command: "spcc")
            }
            return value.report
        }.value
        try Task.checkCancellation(); return r
    }
    private func images(_ folder: URL, report: SpectralCalibrationReport) throws -> (NSImage, NSImage?) {
        guard let before = NSImage(contentsOf: folder.appendingPathComponent("before.tiff")) else { throw ProcessingServiceError.invalidReport(command: "spcc-preview") }
        let after = report.fit.valid ? NSImage(contentsOf: folder.appendingPathComponent("after.tiff")) : nil
        guard !report.fit.valid || after != nil else { throw ProcessingServiceError.invalidReport(command: "spcc-preview") }
        return (before, after)
    }
    func review() async throws {
        guard !busy, canReview, let profile, let catalog else { throw ProcessingServiceError.invalidArguments("Choose a response profile and catalog, and check the aperture settings.") }
        busy = true; defer { busy = false }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let folder = root.appendingPathComponent(UUID().uuidString), requested = settings
        var args = ["spcc", "--input", source.path, "--profile", profile.path, "--catalog", catalog.path, "--output-dir", folder.path]+requested.arguments
        if let report { args += ["--expected-sha", report.sourceSHA] }
        do {
            let response = try await service.runCLI(arguments: args), value = try await decode(response.standardOutput, directory: folder)
            guard value.options == requested, URL(fileURLWithPath: value.source).resolvingSymlinksInPath() == source,
                  report == nil || report?.sourceSHA == value.sourceSHA else { throw ProcessingServiceError.invalidReport(command: "spcc") }
            let rendered = try images(folder, report: value), previous = output
            report = value; output = folder; beforeImage = rendered.0; afterImage = rendered.1
            acceptedProfile = profile; acceptedCatalog = catalog
            profileName = value.profile.name
            if let previous, previous.deletingLastPathComponent() == root { try? FileManager.default.removeItem(at: previous) }
        } catch { try? FileManager.default.removeItem(at: folder); throw error }
    }
    func open(_ folder: URL) async throws {
        guard !busy else { throw ProcessingServiceError.invalidArguments("Spectral calibration is busy.") }; busy = true; defer { busy = false }
        let response = try await service.runCLI(arguments: ["spcc-inspect", "--recipe", folder.appendingPathComponent("recipe.txt").path])
        let value = try await decode(response.standardOutput, directory: folder), rendered = try images(folder, report: value)
        source = URL(fileURLWithPath: value.source).resolvingSymlinksInPath(); settings = value.options; report = value; output = folder
        profile = folder.appendingPathComponent("profile.txt").resolvingSymlinksInPath(); catalog = folder.appendingPathComponent("catalog.txt").resolvingSymlinksInPath()
        acceptedProfile = profile; acceptedCatalog = catalog; beforeImage = rendered.0; afterImage = rendered.1
        profileName = value.profile.name
    }
    /// Save by replaying the reviewed snapshots. CLI verifies source, options,
    /// every dependency and every product, including rejected-review reports.
    func save(to folder: URL) async throws -> URL? {
        guard !busy, isCurrent, let output, let report, !FileManager.default.fileExists(atPath: folder.path) else { throw ProcessingServiceError.invalidArguments("Update the spectral review and choose a new output directory.") }
        busy = true; defer { busy = false }
        let response = try await service.runCLI(arguments: ["spcc-replay", "--recipe", output.appendingPathComponent("recipe.txt").path,
                                                           "--expected-sha", report.sourceSHA, "--output-dir", folder.path])
        let value = try await decode(response.standardOutput, directory: folder)
        guard value.source == report.source, value.sourceSHA == report.sourceSHA, value.options == report.options,
              value.profile.sha == report.profile.sha, value.catalog.sha == report.catalog.sha, value.fit.valid == report.fit.valid else {
            throw ProcessingServiceError.invalidReport(command: "spcc-replay")
        }
        return value.fit.valid ? folder.appendingPathComponent("calibrated.fits") : nil
    }
    func export(to file: URL) async throws {
        guard !busy, isCurrent, let output, let report, report.fit.valid, !FileManager.default.fileExists(atPath: file.path) else { throw ProcessingServiceError.invalidArguments("A validated current spectral review and new output file are required for export.") }
        busy = true; defer { busy = false }
        _ = try await service.runCLI(arguments: ["spcc-export", "--recipe", output.appendingPathComponent("recipe.txt").path,
                                               "--expected-sha", report.sourceSHA, "--output", file.path])
        try Task.checkCancellation()
    }
}
