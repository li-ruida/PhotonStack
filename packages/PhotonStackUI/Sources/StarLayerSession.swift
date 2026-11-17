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
        guard manifest.hasPrefix("PhotonStack-StarLayers-1\n") else {
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
        self.directory = directory
        self.settings = state.recipe
        self.rendered = state.render.map { directory.appendingPathComponent($0) }
        self.display = "combined.tiff"
    }

    func render(using service: any ProcessingService) async throws -> URL {
        guard let directory, settings.isValid else { throw ProcessingServiceError.invalidArguments("Open a layer session first.") }
        let recipe = settings
        let output = directory.appendingPathComponent("render-\(UUID().uuidString)")
        let report = try await service.runCLI(arguments: ["star-layers", "render", "--layers", directory.path,
                                                "--output-dir", output.path] + recipe.arguments)
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
        var removed = starless
        if removed == nil {
            guard let tool else { throw ProcessingServiceError.invalidArguments("Choose a starless image or configure an external removal tool.") }
            // Use a canonical TIFF with explicit sRGB and orientation before the
            // external tool. Never let its optional masks overwrite the source folder.
            let preparation = work.appendingPathComponent("prepared")
            let prepared = preparation.appendingPathComponent("input.tiff")
            _ = try await service.runCLI(arguments: ["star-layers", "prepare", "--input", input.path, "--output-dir", preparation.path])
            let dest = work.appendingPathComponent("starless.tiff")
            _ = try await tool.run(input: prepared, output: dest, workingDirectory: work)
            removed = dest
        }
        guard let removed else { throw ProcessingServiceError.invalidArguments("Missing starless image.") }
        _ = try await service.runCLI(arguments: ["star-layers", "split", "--input", input.path, "--starless", removed.path,
                                                "--output-dir", output.path])
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
