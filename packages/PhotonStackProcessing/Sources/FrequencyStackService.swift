import Foundation

public enum FrequencyStackError: Error, LocalizedError, Equatable {
    case invalidArguments
    case outputAlreadyExists

    public var errorDescription: String? {
        switch self {
        case .invalidArguments:
            "Frequency stacking requires local plan and FITS output URLs, and 1–64 threads."
        case .outputAlreadyExists:
            "Frequency stacking requires a new output path."
        }
    }
}

public extension ProcessingService {
    /// Coadds an explicit prepared-input plan. Registration, model estimation,
    /// anomaly rejection and saturated-source protection are separate stages.
    func frequencyStack(plan: URL, output: URL, threads: Int = 1) async throws -> ProcessingCommandResult {
        guard plan.isFileURL, output.isFileURL, (1...64).contains(threads),
              ["fit", "fits", "fts"].contains(output.pathExtension.lowercased()) else {
            throw FrequencyStackError.invalidArguments
        }
        // Check the actual requested destination before runCLI substitutes a
        // temporary sibling. attributesOfItem also detects dangling symlinks.
        if (try? FileManager.default.attributesOfItem(atPath: output.path)) != nil {
            throw FrequencyStackError.outputAlreadyExists
        }
        return try await runCLI(arguments: [
            "frequency-stack", "--plan", plan.path, "--output", output.path,
            "--threads", String(threads),
        ])
    }
}
