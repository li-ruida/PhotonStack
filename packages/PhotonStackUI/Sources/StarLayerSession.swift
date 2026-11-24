import Foundation
import PhotonStackAppCore
import PhotonStackProcessing

@MainActor
final class StarLayerSession: ObservableObject {
    @Published var directory: URL?
    @Published var rendered: URL?
    @Published var settings = StarLayerRecipe()
    @Published var tool: ExternalImageTool?
    @Published var clippedSamples = 0
    @Published var display = "combined.tiff"
    @Published private(set) var linear = false

    private struct State: Codable {
        var version = 1
        var recipe: StarLayerRecipe
        var render: String?
    }

    func open(_ directory: URL) throws {
        let manifestURL = directory.appendingPathComponent("layers.txt")
        guard (try manifestURL.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? Int.max) < 4096 else {
            throw ProcessingServiceError.invalidArguments("Invalid layer manifest.")
        }
        let manifest = try String(contentsOf: directory.appendingPathComponent("layers.txt"), encoding: .utf8)
        let isLinear = manifest.hasPrefix("PhotonStack-StarLayers-2\nlinear\n")
        guard isLinear || manifest.hasPrefix("PhotonStack-StarLayers-1\n") else {
            throw ProcessingServiceError.invalidArguments("This folder is not a PhotonStack star-layer session.")
        }
        let url = directory.appendingPathComponent("session.json")
        var state = State(recipe: StarLayerRecipe())
        if FileManager.default.fileExists(atPath: url.path) {
            let size = try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? Int.max
            guard size < 65_536 else { throw ProcessingServiceError.invalidArguments("Invalid layer session.") }
            state = try JSONDecoder().decode(State.self, from: Data(contentsOf: url))
        }
        guard state.version == 1, state.recipe.isValid,
              state.render.map({ $0.hasPrefix("render-") && !$0.contains("/") && !$0.contains("\\") }) ?? true else {
            throw ProcessingServiceError.invalidArguments("Invalid layer recipe.")
        }
        self.linear = isLinear
        self.directory = directory
        self.settings = state.recipe
        self.rendered = state.render.map { directory.appendingPathComponent($0) }
        self.display = "combined.tiff"
    }

    func render(using service: any ProcessingService) async throws -> URL {
        guard let directory, settings.isValid, !linear || settings.nebulaGamma == 1 else { throw ProcessingServiceError.invalidArguments("Open a layer session first.") }
        let recipe = settings
        let output = directory.appendingPathComponent("render-\(UUID().uuidString)")
        var arguments = recipe.arguments
        if linear, let index = arguments.firstIndex(of: "--nebula-gamma") { arguments.removeSubrange(index...index + 1) }
        let report = try await service.runCLI(arguments: ["star-layers", linear ? "linear-render" : "render", "--layers", directory.path,
                                                "--output-dir", output.path] + arguments)
        try Task.checkCancellation()
        let state = State(recipe: recipe, render: output.lastPathComponent)
        try JSONEncoder().encode(state).write(to: directory.appendingPathComponent("session.json"), options: .atomic)
        if let bytes = report.standardOutput.data(using: .utf8),
           let fields = try? JSONSerialization.jsonObject(with: bytes) as? [String: Any] {
            clippedSamples = (fields["belowBlackSamples"] as? Int ?? 0) + (fields["aboveWhiteSamples"] as? Int ?? 0)
        }
        rendered = output
        return output.appendingPathComponent(display)
    }

    func separate(input: URL, starless: URL?, output: URL, using service: any ProcessingService) async throws -> URL {
        let work = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStack-StarRemoval-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: work) }
        let isLinear = ["fit", "fits", "fts"].contains(input.pathExtension.lowercased())
        var preparation: URL?
        var externalReport: ProcessingCommandResult?
        var removed = starless
        if removed == nil {
            guard let tool else { throw ProcessingServiceError.invalidArguments("Choose a starless image or configure an external removal tool.") }
            guard isLinear == (tool.linearFITS == true) else {
                throw ProcessingServiceError.invalidArguments("Source and tool domains must match: use a linear FITS tool for linear masters, and a display tool for TIFF images. You can also import a same-grid starless image.")
            }
            let preparedDirectory = work.appendingPathComponent("prepared")
            let prepared = preparedDirectory.appendingPathComponent(isLinear ? "input.fits" : "input.tiff")
            _ = try await service.runCLI(arguments: ["star-layers", isLinear ? "linear-prepare" : "prepare", "--input", input.path, "--output-dir", preparedDirectory.path])
            let dest = work.appendingPathComponent(isLinear ? "starless.fits" : "starless.tiff")
            externalReport = try await tool.run(input: prepared, output: dest, workingDirectory: work)
            if isLinear { preparation = preparedDirectory }
            removed = dest
        }
        guard let removed else { throw ProcessingServiceError.invalidArguments("Missing starless image.") }
        if let preparation {
            _ = try await service.runCLI(arguments: ["star-layers", "linear-restore", "--prepared", preparation.path, "--starless", removed.path, "--output-dir", output.path])
        } else {
            _ = try await service.runCLI(arguments: ["star-layers", isLinear ? "linear-split" : "split", "--input", input.path, "--starless", removed.path, "--output-dir", output.path])
        }
        if let externalReport, let tool {
            try JSONEncoder().encode(tool).write(to: output.appendingPathComponent("external-tool.json"), options: .atomic)
            try (externalReport.standardOutput + "\n" + externalReport.standardError).write(to: output.appendingPathComponent("external-log.txt"), atomically: true, encoding: .utf8)
        }
        try Task.checkCancellation()
        try open(output)
        settings = StarLayerRecipe()
        return try await render(using: service)
    }

    var preview: URL? {
        if display == "combined.tiff" { return rendered?.appendingPathComponent(display) }
        return (rendered ?? directory)?.appendingPathComponent(display)
    }
}
