import Foundation
import Testing
@testable import PhotonStackProcessing

private func calibrationJobServiceFixture(exitCode: Int, waiting: Bool = false) throws -> (URL, CLIProcessingService) {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("CalibrationJobService-" + UUID().uuidString)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
    let executable = root.appendingPathComponent("photonstack")
    let script = """
    #!/bin/bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output-dir" ]; then shift; output="$1"; fi
      shift
    done
    mkdir -p "$output/frame-000001"
    printf 'committed' > "$output/frame-000001/receipt.txt"
    printf 'ready' > "\(root.path)/ready"
    \(waiting ? "trap 'exit 143' TERM; while true; do /bin/sleep 0.1; done" : "printf '{\"status\":\"partial\"}\\n'")
    exit \(exitCode)
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    return (root, CLIProcessingService(executableURL: executable))
}

@Test func calibrationJobServicePreservesPublishedFramesOnFailureAndSuccess() async throws {
    for exitCode in [0, 1] {
        let (root, service) = try calibrationJobServiceFixture(exitCode: exitCode)
        defer { try? FileManager.default.removeItem(at: root) }
        let output = root.appendingPathComponent("job")
        do {
            _ = try await service.runCLI(arguments: ["calibration-run", "--manifest", root.appendingPathComponent("manifest").path, "--output-dir", output.path])
            #expect(exitCode == 0)
        } catch { #expect(exitCode != 0) }
        #expect(try String(contentsOf: output.appendingPathComponent("frame-000001/receipt.txt"), encoding: .utf8) == "committed")
        #expect(try FileManager.default.contentsOfDirectory(atPath: root.path).allSatisfy { !$0.contains("photonstack-output") })
    }
}

@Test func calibrationJobServiceCancellationRetainsTheResumableDirectory() async throws {
    let (root, service) = try calibrationJobServiceFixture(exitCode: 0, waiting: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let output = root.appendingPathComponent("job")
    let task = Task { try await service.runCLI(arguments: ["calibration-run", "--manifest", root.appendingPathComponent("manifest").path, "--output-dir", output.path]) }
    let deadline = ContinuousClock.now + .seconds(30)
    while !FileManager.default.fileExists(atPath: root.appendingPathComponent("ready").path) && ContinuousClock.now < deadline {
        try await Task.sleep(for: .milliseconds(10))
    }
    task.cancel()
    do { _ = try await task.value; Issue.record("Expected cancellation") } catch is CancellationError { }
    #expect(try String(contentsOf: output.appendingPathComponent("frame-000001/receipt.txt"), encoding: .utf8) == "committed")
    #expect(try FileManager.default.contentsOfDirectory(atPath: root.path).allSatisfy { !$0.contains("photonstack-output") })
}
