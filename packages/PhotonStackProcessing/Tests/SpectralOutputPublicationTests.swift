import Foundation
import Testing
@testable import PhotonStackProcessing

@Test func spectralPublicationPreservesExistingAndConcurrentDestinations() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("SpectralPublication-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let fixture = Data("SIMPLE  =                    T".padding(toLength: 2880, withPad: " ", startingAt: 0).utf8)
    try fixture.write(to: root.appendingPathComponent("fixture.fits"))
    let executable = root.appendingPathComponent("tool")
    let service = CLIProcessingService(executableURL: executable, workingDirectoryURL: root)
    for command in ["spcc", "spcc-replay", "spcc-export"] {
        let file = command == "spcc-export"
        for mode in ["new", "existing", "concurrent"] {
            let destination = root.appendingPathComponent(file ? "result.fits" : "result")
            let marker = file ? destination : destination.appendingPathComponent("keep")
            if mode == "existing" {
                if !file { try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: false) }
                try Data("existing".utf8).write(to: marker)
            }
            let script = """
            #!/bin/sh
            set -eu
            \(file ? "cp fixture.fits \"$3\"" : "mkdir \"$3\"; cp fixture.fits \"$3/image.fits\"")
            \(mode == "concurrent" ? (file ? "printf concurrent > result.fits" : "mkdir result; printf concurrent > result/keep") : "")
            """
            try script.write(to: executable, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
            do {
                _ = try await service.runCLI(arguments: [command, file ? "--output" : "--output-dir", destination.path])
                #expect(mode == "new", "Existing destination replaced by \(command)")
            } catch let error as ProcessingServiceError {
                guard case .outputCommitFailed = error else { throw error }; #expect(mode != "new")
            }
            if mode == "new" { #expect(try Data(contentsOf: file ? destination : destination.appendingPathComponent("image.fits")) == fixture) }
            else { #expect(try String(contentsOf: marker, encoding: .utf8) == mode) }
            #expect(try FileManager.default.contentsOfDirectory(atPath: root.path).allSatisfy { !$0.contains("photonstack-output-") })
            try FileManager.default.removeItem(at: destination)
        }
    }
}
