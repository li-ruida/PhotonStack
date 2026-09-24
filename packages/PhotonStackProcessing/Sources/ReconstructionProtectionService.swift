import Foundation

public enum ReconstructionProtectionError: Error, LocalizedError, Equatable {
    case outputConflictsWithInput

    public var errorDescription: String? {
        "Reconstruction protection output must not overwrite any input."
    }
}

public extension ProcessingService {
    /// Inputs must share a linear photometric scale and pixel grid. The mask is
    /// supplied by the caller; this command does not identify clipped sources.
    func protectReconstruction(
        input: URL, reference: URL, mask: URL, output: URL,
        backgroundSigma: Double = 8, amount: Double = 1
    ) async throws -> ProcessingCommandResult {
        // runCLI stages its output, so check the requested destination before
        // that rewrite hides an input/output alias from the CLI's own check.
        let destination = output.standardizedFileURL.resolvingSymlinksInPath()
        let destinationAttributes = try? FileManager.default.attributesOfItem(atPath: destination.path)
        for source in [input, reference, mask] {
            let resolved = source.standardizedFileURL.resolvingSymlinksInPath()
            let attributes = try? FileManager.default.attributesOfItem(atPath: resolved.path)
            let sameInode = attributes?[.systemFileNumber] as? NSNumber
            let sameDevice = attributes?[.systemNumber] as? NSNumber
            let aliasesExistingFile = sameInode != nil && sameDevice != nil
                && sameInode == destinationAttributes?[.systemFileNumber] as? NSNumber
                && sameDevice == destinationAttributes?[.systemNumber] as? NSNumber
            if resolved == destination || aliasesExistingFile {
                throw ReconstructionProtectionError.outputConflictsWithInput
            }
        }
        return try await runCLI(arguments: [
            "protect-reconstruction", "--input", input.path,
            "--reference", reference.path, "--mask", mask.path,
            "--output", output.path, "--background-sigma", String(backgroundSigma),
            "--amount", String(amount),
        ])
    }
}
