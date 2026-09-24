import Foundation
import Testing
@testable import PhotonStackProcessing

@Test func reconstructionProtectionServiceRejectsInputAliasesBeforeStaging() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackProtectionAlias-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let source = directory.appendingPathComponent("source.fits")
    try Data("unchanged scientific input".utf8).write(to: source)
    let symbolic = directory.appendingPathComponent("symbolic.fits")
    let hard = directory.appendingPathComponent("hard.fits")
    try FileManager.default.createSymbolicLink(at: symbolic, withDestinationURL: source)
    try FileManager.default.linkItem(at: source, to: hard)
    let service = CLIProcessingService(executableURL: directory.appendingPathComponent("absent-executable"))
    for destination in [source, symbolic, hard] {
        for sourcePosition in 0..<3 {
            var inputs = (0..<3).map { directory.appendingPathComponent("input-\($0).fits") }
            inputs[sourcePosition] = source
            do {
                _ = try await service.protectReconstruction(
                    input: inputs[0], reference: inputs[1], mask: inputs[2], output: destination
                )
                Issue.record("Input alias must fail before launching the CLI")
            } catch let error as ReconstructionProtectionError {
                #expect(error == .outputConflictsWithInput)
            }
        }
    }
    #expect(try Data(contentsOf: source) == Data("unchanged scientific input".utf8))
}

@Test func reconstructionProtectionServiceRequiresCreatedOutput() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackProtection-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let executable = directory.appendingPathComponent("photonstack")
    let arguments = directory.appendingPathComponent("arguments.txt")
    try "#!/bin/sh\nprintf '%s\\n' \"$@\" > '\(arguments.path)'\nexit 0\n".write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    let output = directory.appendingPathComponent("result.fits")
    let input = directory.appendingPathComponent("reconstructed.fits")
    let reference = directory.appendingPathComponent("direct reference.fits")
    let mask = directory.appendingPathComponent("weights.fits")
    let service = CLIProcessingService(executableURL: executable)
    do {
        _ = try await service.protectReconstruction(
            input: input, reference: reference, mask: mask, output: output,
            backgroundSigma: 6, amount: 0.75
        )
        Issue.record("Missing protection output must fail")
    } catch let error as ProcessingServiceError {
        #expect(error == .outputMissing(path: output.path))
    }
    let args = try String(contentsOf: arguments, encoding: .utf8).split(separator: "\n").map(String.init)
    // The service redirects output to a sibling staging file before publishing.
    let outputIndex = try #require(args.firstIndex(of: "--output")) + 1
    let stagedOutput = URL(fileURLWithPath: args[outputIndex])
    #expect(stagedOutput.deletingLastPathComponent().path == output.deletingLastPathComponent().path)
    #expect(stagedOutput.lastPathComponent.hasPrefix(".result.photonstack-output-"))
    #expect(stagedOutput.pathExtension == "fits")
    #expect(stagedOutput != output)
    #expect(args == [
        "protect-reconstruction", "--input", input.path,
        "--reference", reference.path, "--mask", mask.path,
        "--output", stagedOutput.path, "--background-sigma", "6.0", "--amount", "0.75",
    ])
}
