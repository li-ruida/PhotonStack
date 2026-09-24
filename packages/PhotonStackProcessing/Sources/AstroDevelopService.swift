import Foundation
import PhotonStackAppCore

public extension ProcessingService {
    func develop(input: URL, output: URL, settings: AstroDevelopSettings) async throws -> ProcessingCommandResult {
        try await runCLI(arguments: [
            "develop", "--input", input.path, "--output", output.path,
            "--stellar-balance", settings.stellarBalance ? "on" : "off",
            "--brightness", String(settings.brightness),
            "--tone-curve", settings.toneCurve,
            "--tone-scale", String(settings.toneScale),
            "--white-point", String(settings.whitePoint),
            "--background", String(settings.background),
            "--star-exposure", String(settings.starExposure),
            "--star-peak-threshold", String(settings.starPeakThreshold),
            "--saturation", String(settings.saturation),
            "--shadow-neutralization", String(settings.shadowNeutralization),
            "--red-gain", String(settings.redGain),
            "--blue-gain", String(settings.blueGain),
        ])
    }
}
