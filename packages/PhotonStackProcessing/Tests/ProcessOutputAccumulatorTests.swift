import Foundation
import Testing
@testable import PhotonStackProcessing

/// Waits for an actual child progress record, with a separate startup watchdog.
/// Cancelling the losing stream iterator/sleeper keeps failed tests bounded.
func waitForEngineTestSignal(_ stream: AsyncStream<Void>) async -> Bool {
    await withTaskGroup(of: Bool.self) { group in
        group.addTask {
            for await _ in stream { return true }
            return false
        }
        group.addTask {
            try? await Task.sleep(for: .seconds(30))
            return false
        }
        let signalled = await group.next() ?? false
        group.cancelAll()
        return signalled
    }
}

@Test func processOutputRejectsOversizedUnterminatedRecord() {
    let output = ProcessOutputAccumulator(jobID: nil, progressScope: nil, stdoutLimit: 8)
    #expect(output.appendStdout(Data("1234".utf8)))
    #expect(output.appendStdout(Data("5678".utf8)))
    #expect(!output.appendStdout(Data("9".utf8)))
    #expect(!output.appendStdout(Data("\n{\"success\":true}\n".utf8)))
    output.finishStdout()
    #expect(output.snapshot().stdoutExceededLimit)
    #expect(output.snapshot().stdout.isEmpty)
}

@Test func processOutputLimitsTotalRecordsAndPreservesExactBoundary() {
    let output = ProcessOutputAccumulator(jobID: nil, progressScope: nil, stdoutLimit: 8)
    #expect(output.appendStdout(Data("abc\ndef\n".utf8)))
    output.finishStdout()
    #expect(output.snapshot().stdout == "abc\ndef\n")
    #expect(!output.appendStdout(Data("x\n".utf8)))
    #expect(output.snapshot().stdoutExceededLimit)
}

@Test func processOutputAcceptsProgressAfterReportBudgetIsFull() {
    let output = ProcessOutputAccumulator(jobID: nil, progressScope: nil, stdoutLimit: 256)
    let report = String(repeating: "x", count: 255) + "\n"
    #expect(output.appendStdout(Data(report.utf8)))
    let progress = Data(#"{"type":"progress","task":"stack","command":"stack","stage":"accumulate","progress":1}"#.utf8) + Data([0x0A])
    #expect(output.appendStdout(progress))
    output.finishStdout()
    #expect(output.snapshot().stdout == report)
    #expect(!output.snapshot().stdoutExceededLimit)
}

@Test func processOutputStreamsProgressWithoutConsumingReportBudget() {
    let jobID = UUID()
    let output = ProcessOutputAccumulator(jobID: jobID, progressScope: nil, stdoutLimit: 256)
    let progress = Data(#"{"type":"progress","task":"stack","command":"stack","stage":"accumulate","progress":0.5}"#.utf8) + Data([0x0A])
    // A synchronous observer may inspect its job. Notification must not hold the
    // accumulator lock, otherwise this actual producer/observer contract deadlocks.
    let observer = NotificationCenter.default.addObserver(forName: .photonStackProcessingProgress, object: nil, queue: nil) { notification in
        if (notification.object as? ProcessingProgressEvent)?.jobID == jobID {
            #expect(!output.snapshot().stdoutExceededLimit)
        }
    }
    defer { NotificationCenter.default.removeObserver(observer) }
    for _ in 0..<1_000 { #expect(output.appendStdout(progress)) }
    #expect(output.appendStdout(Data("{\"success\":true}".utf8)))
    output.finishStdout()
    #expect(output.snapshot().stdout == "{\"success\":true}")
    #expect(!output.snapshot().stdoutExceededLimit)
}

@Test func processOutputRetainsBoundedDiagnosticTailAcrossUTF8Chunks() {
    let output = ProcessOutputAccumulator(jobID: nil, progressScope: nil, stderrLimit: 8)
    let diagnostic = Data("第一段错误!".utf8)
    for byte in diagnostic { output.appendStderr(Data([byte])) }
    let result = output.snapshot()
    #expect(result.stderr.contains("Earlier diagnostic output omitted"))
    #expect(result.stderr.hasSuffix("错误!"))
    #expect(!result.stderr.contains("�"))
    #expect(!result.stdoutExceededLimit)
}

#if os(macOS)
@Test func cliProcessingServiceStopsOversizedReportAndPreservesExistingOutput() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackCLIOutputLimit-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let executable = directory.appendingPathComponent("photonstack")
    let output = directory.appendingPathComponent("existing.png")
    try Data("original output".utf8).write(to: output)
    let script = """
    #!/bin/bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output" ]; then
        shift
        printf 'partial replacement' > "$1"
      fi
      shift
    done
    trap '' TERM
    printf '%s\\n' '{"type":"progress","task":"stretch","command":"stretch","stage":"ready","progress":0}'
    deadline=$((SECONDS + 30))
    while [ ! -e "$PWD/release" ] && [ "$SECONDS" -lt "$deadline" ]; do /bin/sleep 0.01; done
    /usr/bin/head -c \(ProcessOutputAccumulator.standardOutputLimit + 1) /dev/zero
    deadline=$((SECONDS + 10))
    while [ "$SECONDS" -lt "$deadline" ]; do :; done
    printf 'not terminated' > "$PWD/natural-exit"
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    let jobID = UUID()
    let ready = AsyncStream<Void>.makeStream()
    let observer = NotificationCenter.default.addObserver(forName: .photonStackProcessingProgress, object: nil, queue: nil) { notification in
        if let event = notification.object as? ProcessingProgressEvent,
           event.jobID == jobID, event.stage == "ready" {
            ready.continuation.yield(())
        }
    }
    defer {
        ready.continuation.finish()
        NotificationCenter.default.removeObserver(observer)
    }
    let task = Task {
        try await ProcessingProgressContext.$jobID.withValue(jobID) {
            try await CLIProcessingService(executableURL: executable).runCLI(arguments: ["stretch", "--output", output.path])
        }
    }
    defer { task.cancel() }
    guard await waitForEngineTestSignal(ready.stream) else {
        task.cancel()
        _ = try? await task.value
        Issue.record("Engine never reached the ready handshake")
        return
    }
    // Release output only after the child and its stream reader are running.
    try Data().write(to: directory.appendingPathComponent("release"))
    do {
        _ = try await task.value
        Issue.record("An oversized structured report must fail before publishing its output")
    } catch let error as ProcessingServiceError {
        #expect(error == .commandOutputLimitExceeded(maximumBytes: ProcessOutputAccumulator.standardOutputLimit))
    }
    // This proves the TERM-ignoring child was forcibly stopped, independently of
    // scheduler/launch latency. The script's natural exit also bounds regressions.
    #expect(!FileManager.default.fileExists(atPath: directory.appendingPathComponent("natural-exit").path))
    #expect(try String(contentsOf: output, encoding: .utf8) == "original output")
    #expect(try !FileManager.default.contentsOfDirectory(atPath: directory.path).contains { $0.contains("photonstack-output-") })
}
#endif
