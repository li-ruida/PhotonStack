import Foundation
import PhotonStackAppCore

public extension ProcessingService {
    func removeGreenCast(input: URL, output: URL, settings: GreenCastSettings) async throws -> ProcessingCommandResult {
        try await runCLI(arguments: [
            "color", "remove-green", "--input", input.path, "--output", output.path,
            "--green-method", settings.method.rawValue,
            "--amount", String(settings.amount),
            "--background-limit", String(settings.backgroundLimit),
            "--green-threshold", String(settings.greenThreshold),
            "--preserve-lightness", settings.preserveLightness ? "on" : "off",
        ])
    }
}
