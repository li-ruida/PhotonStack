import Foundation
import Testing
@testable import PhotonStackProcessing

private func frequencyTestDirectory() throws -> URL {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackFrequency-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
}

@Test func frequencyServiceRejectsExistingOutputsAndInvalidArguments() async throws {
    let directory = try frequencyTestDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }
    let source = directory.appendingPathComponent("source.fits")
    try Data("preserved".utf8).write(to: source)
    let symbolic = directory.appendingPathComponent("symbolic.fits")
    let hard = directory.appendingPathComponent("hard.fits")
    let dangling = directory.appendingPathComponent("dangling.fits")
    try FileManager.default.createSymbolicLink(at: symbolic, withDestinationURL: source)
    try FileManager.default.linkItem(at: source, to: hard)
    try FileManager.default.createSymbolicLink(at: dangling, withDestinationURL: directory.appendingPathComponent("absent.fits"))
    let service = CLIProcessingService(executableURL: directory.appendingPathComponent("absent-executable"))
    for output in [source, symbolic, hard, dangling] {
        do {
            _ = try await service.frequencyStack(plan: directory.appendingPathComponent("plan.psfreq"), output: output)
            Issue.record("Existing destinations must fail before launch")
        } catch let error as FrequencyStackError {
            #expect(error == .outputAlreadyExists)
        }
    }
    for threads in [0, 65] {
        do {
            _ = try await service.frequencyStack(plan: source, output: directory.appendingPathComponent("new.fits"), threads: threads)
            Issue.record("Invalid thread count must fail before launch")
        } catch let error as FrequencyStackError {
            #expect(error == .invalidArguments)
        }
    }
    #expect(try Data(contentsOf: source) == Data("preserved".utf8))
}

@Test func frequencyServicePublishesNewOutputAndPreservesConcurrentDestination() async throws {
    let directory = try frequencyTestDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }
    let fixture = directory.appendingPathComponent("fixture.fits")
    let cards = ["SIMPLE  =                    T", "BITPIX  =                  -32", "NAXIS   =                    2",
                 "NAXIS1  =                    1", "NAXIS2  =                    1", "END"]
    var data = Data(cards.map { $0.padding(toLength: 80, withPad: " ", startingAt: 0) }.joined().utf8)
    data.append(Data(repeating: 32, count: 2880 - data.count))
    data.append(Data(repeating: 0, count: 2880))
    try data.write(to: fixture)
    let output = directory.appendingPathComponent("result.fits")
    let executable = directory.appendingPathComponent("photonstack")
    let arguments = directory.appendingPathComponent("arguments.txt")
    let service = CLIProcessingService(executableURL: executable)
    for concurrentWriter in [false, true] {
        let script = """
        #!/bin/sh
        printf '%s\\n' "$@" > '\(arguments.path)'
        while [ "$1" != "--output" ]; do shift; done
        shift
        cp '\(fixture.path)' "$1"
        \(concurrentWriter ? "printf 'late destination' > '\(output.path)'" : "")
        exit 0
        """
        try script.write(to: executable, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
        if concurrentWriter {
            do {
                _ = try await service.frequencyStack(plan: directory.appendingPathComponent("plan.psfreq"), output: output, threads: 2)
                Issue.record("Publication must not replace a concurrent destination")
            } catch let error as ProcessingServiceError {
                guard case .outputCommitFailed(let path, _) = error else { throw error }
                #expect(path == output.path)
            }
            #expect(try String(contentsOf: output, encoding: .utf8) == "late destination")
        } else {
            _ = try await service.frequencyStack(plan: directory.appendingPathComponent("plan.psfreq"), output: output, threads: 2)
            #expect(try Data(contentsOf: output) == data)
            try FileManager.default.removeItem(at: output)
        }
        let args = try String(contentsOf: arguments, encoding: .utf8).split(separator: "\n").map(String.init)
        #expect(args[0...2] == ["frequency-stack", "--plan", directory.appendingPathComponent("plan.psfreq").path])
        #expect(args.suffix(2) == ["--threads", "2"])
        let staged = URL(fileURLWithPath: args[4])
        #expect(staged != output)
        #expect(!FileManager.default.fileExists(atPath: staged.path))
    }
}
