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
    for command in ["spcc", "spcc-replay", "spcc-export", "spcc-narrowband-profile", "sensor-master-build", "sensor-master-replay"] {
        let file = command == "spcc-export"
        let profile = command == "spcc-narrowband-profile"
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
            \(profile ? "mkdir \"$3\"; printf 'PhotonStack-SpectralProfile-2\\n' > \"$3/profile.txt\"; printf 'PhotonStack-SpectralProfile-1\\n' > \"$3/base-profile.txt\"" : file ? "cp fixture.fits \"$3\"" : "mkdir \"$3\"; cp fixture.fits \"$3/image.fits\"")
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
            if mode == "new" {
                if profile { #expect(try String(contentsOf: destination.appendingPathComponent("profile.txt"), encoding: .utf8) == "PhotonStack-SpectralProfile-2\n") }
                else { #expect(try Data(contentsOf: file ? destination : destination.appendingPathComponent("image.fits")) == fixture) }
            }
            else { #expect(try String(contentsOf: marker, encoding: .utf8) == mode) }
            #expect(try FileManager.default.contentsOfDirectory(atPath: root.path).allSatisfy { !$0.contains("photonstack-output-") })
            try FileManager.default.removeItem(at: destination)
        }
    }
}

@Test func spectralProfilePublicationRejectsMissingEmptyLinkedAndImageOnlyOutputs() async throws {
    let fm = FileManager.default, root = fm.temporaryDirectory.appendingPathComponent("ProfilePublication-\(UUID())")
    try fm.createDirectory(at: root, withIntermediateDirectories: false)
    defer { try? fm.removeItem(at: root) }
    let executable = root.appendingPathComponent("tool")
    let service = CLIProcessingService(executableURL: executable, workingDirectoryURL: root)
    for bad in ["rm \"$3/base-profile.txt\"", "printf '' > \"$3/profile.txt\"",
                "mv \"$3/profile.txt\" \"$3/real.txt\"; ln -s real.txt \"$3/profile.txt\"",
                "rm \"$3/profile.txt\"; printf image > \"$3/image.fits\""] {
        let script = """
        #!/bin/sh
        set -eu
        mkdir "$3"
        printf 'PhotonStack-SpectralProfile-2\\n' > "$3/profile.txt"
        printf 'PhotonStack-SpectralProfile-1\\n' > "$3/base-profile.txt"
        \(bad)
        """
        try script.write(to: executable, atomically: true, encoding: .utf8)
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
        let destination = root.appendingPathComponent("result")
        do {
            _ = try await service.runCLI(arguments: ["spcc-narrowband-profile", "--output-dir", destination.path])
            Issue.record("Incomplete profile published")
        } catch let error as ProcessingServiceError {
            guard case .outputInvalid = error else { throw error }
        }
        #expect(!fm.fileExists(atPath: destination.path))
        #expect(try fm.contentsOfDirectory(atPath: root.path).allSatisfy { !$0.contains("photonstack-output-") })
    }
}
