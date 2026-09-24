import Foundation
import PhotonStackAppCore

public enum DisplayGridError: Error, LocalizedError, Equatable {
    case invalidSettings
    case displayImageRequired
    case outputAlreadyExists
    public var errorDescription: String? {
        switch self {
        case .invalidSettings: "Grid reduction amount must be finite and between 0 and 1."
        case .displayImageRequired: "Grid reduction requires a developed TIFF or PNG input and output."
        case .outputAlreadyExists: "Grid reduction requires a new output path."
        }
    }
}

public extension ProcessingService {
    func suppressDisplayGrid(input: URL, output: URL, settings: DisplayGridSettings) async throws -> ProcessingCommandResult {
        guard settings.isValid else { throw DisplayGridError.invalidSettings }
        // Validate before output staging can hide a requested FITS suffix.
        guard input.isFileURL, output.isFileURL,
              DisplayGridSettings.supports(input), DisplayGridSettings.supports(output) else {
            throw DisplayGridError.displayImageRequired
        }
        if (try? FileManager.default.attributesOfItem(atPath: output.path)) != nil {
            throw DisplayGridError.outputAlreadyExists
        }
        return try await runCLI(arguments: ["suppress-grid", "--input", input.path,
            "--output", output.path, "--amount", String(settings.amount)])
    }
}
