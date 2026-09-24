import Foundation
import PhotonStackAppCore

public enum ContinuumToneError: Error, LocalizedError, Equatable {
    case invalidSettings
    case displayImageRequired
    public var errorDescription: String? {
        switch self {
        case .invalidSettings: "Continuum tone requires a monotone curve from 0:0 to 1:1 and fine radius smaller than the outer radius."
        case .displayImageRequired: "Develop the linear FITS before adjusting diffuse tone; output must also be a display image."
        }
    }
}

public extension ProcessingService {
    func continuumTone(input: URL, output: URL, settings: ContinuumToneSettings) async throws -> ProcessingCommandResult {
        guard settings.isValid else { throw ContinuumToneError.invalidSettings }
        // Check the requested output before runCLI rewrites it to a staging URL.
        guard ![input, output].contains(where: { AssetKind.detect(from: $0) == .fits }) else {
            throw ContinuumToneError.displayImageRequired
        }
        return try await runCLI(arguments: [
            "local-contrast", "--input", input.path, "--output", output.path,
            "--protect-structure", "on", "--amount", String(settings.amount),
            "--fine-radius", String(settings.fineRadius), "--radius", String(settings.radius),
            "--star-chroma", String(settings.starChroma), "--continuum-curve", settings.curvePoints,
        ])
    }
}
