import Foundation
import Testing
import PhotonStackAppCore
@testable import PhotonStackProcessing
#if os(macOS)
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
#endif

private final class ProgressEventRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var events: [ProcessingProgressEvent] = []

    func append(_ event: ProcessingProgressEvent) {
        lock.lock()
        events.append(event)
        lock.unlock()
    }

    func snapshot() -> [ProcessingProgressEvent] {
        lock.lock()
        let result = events
        lock.unlock()
        return result
    }
}

private let testPNGBase64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="

private func shellWriteTestPNG(to shellPath: String) -> String {
    "printf '%s' '\(testPNGBase64)' | /usr/bin/base64 -D > \(shellPath)"
}

#if os(macOS)
private func writeSolidTestPNG(to url: URL, red: CGFloat, green: CGFloat, blue: CGFloat) throws {
    let colorSpace = try #require(CGColorSpace(name: CGColorSpace.sRGB))
    let context = try #require(
        CGContext(
            data: nil,
            width: 1,
            height: 1,
            bitsPerComponent: 8,
            bytesPerRow: 4,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
        )
    )
    context.setFillColor(red: red, green: green, blue: blue, alpha: 1)
    context.fill(CGRect(x: 0, y: 0, width: 1, height: 1))
    let image = try #require(context.makeImage())
    let destination = try #require(
        CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)
    )
    CGImageDestinationAddImage(destination, image, nil)
    #expect(CGImageDestinationFinalize(destination))
}
#endif

@Test func unavailableProcessingServiceReportsExecutableError() async {
    let service = UnavailableProcessingService()
    let asset = PhotonStackAsset(originalURL: URL(fileURLWithPath: "/tmp/missing.fit"), kind: .fits)

    do {
        _ = try await service.inspect(asset)
        Issue.record("Expected unavailable service to throw")
    } catch let error as ProcessingServiceError {
        #expect(error == .executableNotFound)
    } catch {
        Issue.record("Unexpected error: \(error)")
    }
}

#if os(macOS)
@Test func cliProcessingServiceFindsBuiltDebugBinaryWhenPresent() {
    let candidate = CLIProcessingService.defaultExecutableURL()
    if let candidate {
        #expect(candidate.lastPathComponent == "photonstack")
    }
}

@Test func cliProcessingServiceFindsBundledExecutable() throws {
    let bundleRoot = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackBundleTest-\(UUID().uuidString).app")
    let macOSDirectory = bundleRoot.appendingPathComponent("Contents/MacOS", isDirectory: true)
    let executable = macOSDirectory.appendingPathComponent("photonstack")
    defer {
        try? FileManager.default.removeItem(at: bundleRoot)
    }

    try FileManager.default.createDirectory(at: macOSDirectory, withIntermediateDirectories: true)
    try "#!/usr/bin/env bash\nexit 0\n".write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let bundle = try #require(Bundle(url: bundleRoot))
    let candidate = CLIProcessingService.bundledExecutableURL(bundle: bundle)
    #expect(candidate == executable)
}

@Test func cliProcessingServiceRunsFromExecutableDirectory() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackCLICWD-\(UUID().uuidString)", isDirectory: true)
    let executable = directory.appendingPathComponent("photonstack")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try "#!/usr/bin/env bash\npwd\n".write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    let result = try await service.runCLI(arguments: ["version"])
    let reportedDirectory = URL(fileURLWithPath: result.standardOutput.trimmingCharacters(in: .whitespacesAndNewlines))
    #expect(reportedDirectory.resolvingSymlinksInPath().path == directory.resolvingSymlinksInPath().path)
}

@Test func cliProcessingServiceRejectsMixedColorEncodingBeforeLaunch() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLIColorPreflight-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let launchMarker = directory.appendingPathComponent("launched")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try "#!/usr/bin/env bash\ntouch '\(launchMarker.path)'\nexit 0\n"
        .write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    let srgb = directory.appendingPathComponent("display.png")
    let linear = directory.appendingPathComponent("scientific.fits")
    let output = directory.appendingPathComponent("output.fits")

    func expectRejected(
        command: String,
        operation: () async throws -> Void
    ) async {
        do {
            try await operation()
            Issue.record("Expected \(command) color preflight to reject mixed inputs")
        } catch let error as ProcessingServiceError {
            #expect(error == .inputColorEncodingMismatch(
                command: command,
                inputs: [srgb.path, linear.path]
            ))
            #expect(error.localizedDescription.contains("display.png"))
            #expect(error.localizedDescription.contains("scientific.fits"))
        } catch {
            Issue.record("Unexpected error: \(error)")
        }
    }

    await expectRejected(command: "master") {
        _ = try await service.createMaster(
            output: output,
            method: .average,
            inputs: [srgb, linear],
            rawOptions: .init()
        )
    }
    await expectRejected(command: "calibrate") {
        _ = try await service.calibrate(
            light: srgb,
            output: output,
            dark: linear,
            bias: nil,
            flat: nil,
            darkBiasState: .included,
            flatBiasState: .included,
            rawOptions: .init()
        )
    }
    await expectRejected(command: "stack") {
        _ = try await service.stack(inputs: [srgb, linear], output: output, method: .average, alignment: .none)
    }
    await expectRejected(command: "mosaic") {
        _ = try await service.mosaic(inputs: [srgb, linear], output: output)
    }
    await expectRejected(command: "meteors restore") {
        _ = try await service.restoreMeteors(base: srgb, source: linear, output: output)
    }
    await expectRejected(command: "drizzle") {
        _ = try await service.drizzle(inputs: [srgb, linear], output: output)
    }

    #expect(FileManager.default.fileExists(atPath: launchMarker.path) == false)
    #expect(FileManager.default.fileExists(atPath: output.path) == false)

    let rasterBase = directory.appendingPathComponent("stack.tiff")
    let rawSource = directory.appendingPathComponent("source.nef")
    let rasterOutput = directory.appendingPathComponent("restored.tiff")
    do {
        _ = try await service.restoreMeteors(base: rasterBase, source: rawSource, output: rasterOutput)
    } catch {
        // The stub does not create an image; reaching it is the behavior under test.
    }
    #expect(FileManager.default.fileExists(atPath: launchMarker.path))
}

@Test func cliProcessingServiceMapsRawOptionsForMasterAndCalibration() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLICalibrationRawOptions-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let argumentsFile = directory.appendingPathComponent("arguments.txt")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    : > '\(argumentsFile.path)'
    output=''
    previous=''
    for argument in "$@"; do
      printf '%s\n' "$argument" >> '\(argumentsFile.path)'
      if [ "$previous" = '--output' ]; then output="$argument"; fi
      previous="$argument"
    done
    \(shellWriteTestPNG(to: "\"$output\""))
    printf '%s\n' '{"type":"complete"}'
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    let options = RawProcessingOptions(
        whiteBalanceMode: .daylight,
        manualWhiteBalanceTemperature: 7200,
        manualWhiteBalanceTint: -18,
        exposureBias: 1.5,
        blackLevelMode: .auto,
        manualBlackLevel: 0.0125,
        demosaicQuality: .high,
        linearOutput: true
    )
    let light = directory.appendingPathComponent("light.nef")
    let dark = directory.appendingPathComponent("dark.nef")
    let bias = directory.appendingPathComponent("bias.nef")
    let flat = directory.appendingPathComponent("flat.nef")
    let output = directory.appendingPathComponent("output.png")

    func arguments() throws -> [String] {
        try String(contentsOf: argumentsFile, encoding: .utf8)
            .split(whereSeparator: \.isNewline)
            .map(String.init)
    }

    func containsPair(_ arguments: [String], _ name: String, _ value: String) -> Bool {
        arguments.indices.dropLast().contains { index in
            arguments[index] == name && arguments[index + 1] == value
        }
    }

    _ = try await service.createMaster(
        output: output,
        method: .median,
        inputs: [dark],
        rawOptions: options
    )
    var mapped = try arguments()
    #expect(containsPair(mapped, "--raw-white-balance", "daylight"))
    #expect(containsPair(mapped, "--raw-temperature", "7200.0"))
    #expect(containsPair(mapped, "--raw-tint", "-18.0"))
    #expect(containsPair(mapped, "--raw-exposure-bias", "1.5"))
    #expect(containsPair(mapped, "--raw-black-level", "auto"))
    #expect(containsPair(mapped, "--raw-black-value", "0.0125"))
    #expect(containsPair(mapped, "--raw-demosaic", "high"))
    #expect(containsPair(mapped, "--raw-linear", "on"))

    _ = try await service.calibrate(
        light: light,
        output: output,
        dark: dark,
        bias: bias,
        flat: flat,
        darkBiasState: .included,
        flatBiasState: .removed,
        rawOptions: options
    )
    mapped = try arguments()
    #expect(containsPair(mapped, "--raw-white-balance", "daylight"))
    #expect(containsPair(mapped, "--raw-temperature", "7200.0"))
    #expect(containsPair(mapped, "--raw-tint", "-18.0"))
    #expect(containsPair(mapped, "--raw-exposure-bias", "1.5"))
    #expect(containsPair(mapped, "--raw-black-level", "auto"))
    #expect(containsPair(mapped, "--raw-black-value", "0.0125"))
    #expect(containsPair(mapped, "--raw-demosaic", "high"))
    #expect(containsPair(mapped, "--raw-linear", "on"))
    #expect(containsPair(mapped, "--dark-bias", "included"))
    #expect(containsPair(mapped, "--flat-bias", "removed"))

    _ = try await service.background(
        input: light,
        output: output,
        model: "grid",
        mode: "divide",
        strength: 0.6,
        preserveBrightness: false,
        protectBrightTargets: false
    )
    mapped = try arguments()
    #expect(containsPair(mapped, "--mode", "divide"))
    #expect(containsPair(mapped, "--strength", "0.6"))
    #expect(containsPair(mapped, "--preserve-brightness", "off"))
    #expect(containsPair(mapped, "--protect-bright-targets", "off"))
}

@Test func cliProcessingServicePublishesEveryProgressLineForCurrentJob() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLIProgress-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    printf '%s\n' '{ "type": "progress", "task": "drizzle", "command": "drizzle", "stage": "align", "progress": 0.42, "step": 2, "total": 3 }'
    printf '%s\n' '{"type":"progress","task":"drizzle","command":"drizzle","stage":"write","progress":0.91}'
    printf '%s\n' '{"type":"complete","command":"drizzle"}'
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let recorder = ProgressEventRecorder()
    let observer = NotificationCenter.default.addObserver(
        forName: .photonStackProcessingProgress,
        object: nil,
        queue: nil
    ) { notification in
        if let event = notification.object as? ProcessingProgressEvent {
            recorder.append(event)
        }
    }
    defer {
        NotificationCenter.default.removeObserver(observer)
    }

    let jobID = UUID()
    let service = CLIProcessingService(executableURL: executable)
    let progressScope = ProcessingProgressScope(lowerBound: 0.2, upperBound: 0.4)
    let result = try await ProcessingProgressContext.$jobID.withValue(jobID) {
        try await ProcessingProgressContext.$progressScope.withValue(progressScope) {
            try await service.runCLI(arguments: ["drizzle"])
        }
    }
    let events = recorder.snapshot().filter { $0.jobID == jobID }

    #expect(events.map(\.stage) == ["align", "write"])
    #expect(abs((events.first?.progress ?? 0) - 0.284) < 0.000_001)
    #expect(abs((events.last?.progress ?? 0) - 0.382) < 0.000_001)
    #expect(events.first?.step == 2)
    #expect(events.first?.total == 3)
    #expect(result.standardOutput.contains("\"type\":\"progress\"") == false)
    #expect(result.standardOutput.contains("\"type\": \"progress\"") == false)
    #expect(result.standardOutput.contains("\"type\":\"complete\"") == true)
}

@Test func cliProcessingServiceMapsCombinedCurvePreviewArguments() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLICurvePreview-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let argumentsFile = directory.appendingPathComponent("arguments.txt")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    : > '\(argumentsFile.path)'
    output=''
    previous=''
    for argument in "$@"; do
      printf '%s\n' "$argument" >> '\(argumentsFile.path)'
      if [ "$previous" = '--output' ]; then output="$argument"; fi
      previous="$argument"
    done
    \(shellWriteTestPNG(to: "\"$output\""))
    printf '%s\n' '{"type":"complete","command":"preview","curveApplied":true}'
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    let options = RawProcessingOptions(
        whiteBalanceMode: .manual,
        manualWhiteBalanceTemperature: 6800,
        manualWhiteBalanceTint: 12,
        exposureBias: 0.75,
        blackLevelMode: .manual,
        manualBlackLevel: 0.02,
        demosaicQuality: .fast,
        linearOutput: false
    )
    let input = directory.appendingPathComponent("input.nef")
    let output = directory.appendingPathComponent("output.png")

    _ = try await service.makeCurvePreview(
        input: input,
        output: output,
        width: 960,
        rawOptions: options,
        points: [(0, 0), (0.45, 0.38), (1, 1)],
        channel: .luminance
    )

    let mapped = try String(contentsOf: argumentsFile, encoding: .utf8)
        .split(whereSeparator: \.isNewline)
        .map(String.init)

    func containsPair(_ arguments: [String], _ name: String, _ value: String) -> Bool {
        arguments.indices.dropLast().contains { index in
            arguments[index] == name && arguments[index + 1] == value
        }
    }

    #expect(mapped.first == "preview")
    #expect(containsPair(mapped, "--width", "960"))
    #expect(containsPair(mapped, "--curve-points", "0.0:0.0,0.45:0.38,1.0:1.0"))
    #expect(containsPair(mapped, "--curve-channel", "luminance"))
    #expect(containsPair(mapped, "--raw-white-balance", "manual"))
    #expect(containsPair(mapped, "--raw-temperature", "6800.0"))
    #expect(containsPair(mapped, "--raw-tint", "12.0"))
    #expect(containsPair(mapped, "--raw-exposure-bias", "0.75"))
    #expect(containsPair(mapped, "--raw-black-level", "manual"))
    #expect(containsPair(mapped, "--raw-black-value", "0.02"))
    #expect(containsPair(mapped, "--raw-demosaic", "fast"))
    #expect(containsPair(mapped, "--raw-linear", "off"))
}

@Test func engineCurvePreviewBridgeAppliesCurveWithoutLaunchingCLI() throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackEngineCurvePreview-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let input = directory.appendingPathComponent("input.png")
    let output = directory.appendingPathComponent("output.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try "#!/usr/bin/env bash\nexit 99\n".write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    try writeSolidTestPNG(to: input, red: 0.5, green: 0.4, blue: 0.3)

    let service = CLIProcessingService(executableURL: executable)
    let bridgedResult = try EngineCurvePreview.runIfAvailable(
        using: service,
        input: input,
        output: output,
        width: 1,
        rawOptions: RawProcessingOptions(),
        points: [(0, 0), (0.5, 0.25), (1, 1)],
        channel: .rgb
    )
    let result = try #require(bridgedResult)

    #expect(FileManager.default.fileExists(atPath: output.path))
    #expect(result.command == ["preview"])
    #expect(result.standardOutput.contains("\"curveApplied\": true"))
    #expect(try Data(contentsOf: input) != Data(contentsOf: output))
}

@Test func processingProgressScopeRejectsNonFiniteBoundsAndProgress() {
    let scope = ProcessingProgressScope(lowerBound: .nan, upperBound: .infinity)

    #expect(scope.map(0.5) == 0.5)
    #expect(scope.map(.nan) == 0)

    scope.update(lowerBound: .nan, upperBound: -.infinity)
    #expect(scope.map(0.5) == 0.5)

    scope.update(lowerBound: 0.8, upperBound: 0.2)
    #expect(scope.map(0) == 0.8)
    #expect(scope.map(1) == 0.8)
}

@Test func cliProcessingServicePreservesUTF8SplitAcrossPipeReads() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLIUTF8-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = #"""
    #!/usr/bin/env bash
    printf '%s' '{"type":"complete","command":"inspect","message":"'
    printf '\351'
    sleep 0.05
    printf '\223\266河"}\n'
    printf '\351' >&2
    sleep 0.05
    printf '\224\231误\n' >&2
    """#
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    let result = try await service.runCLI(arguments: ["inspect"])

    #expect(result.standardOutput.contains(#""message":"银河""#))
    #expect(result.standardError.contains("错误"))
    #expect(result.standardOutput.contains("�") == false)
    #expect(result.standardError.contains("�") == false)
}

@Test func cliProcessingServiceCancellationStopsProgressAndThrowsCancellation() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLICancellation-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    trap 'exit 0' TERM
    progress=0
    while true; do
      printf '{"type":"progress","task":"drizzle","command":"drizzle","stage":"accumulate","progress":0.%02d}\n' "$progress"
      progress=$((progress + 1))
      sleep 0.05
    done
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let jobID = UUID()
    let recorder = ProgressEventRecorder()
    let observer = NotificationCenter.default.addObserver(
        forName: .photonStackProcessingProgress,
        object: nil,
        queue: nil
    ) { notification in
        guard let event = notification.object as? ProcessingProgressEvent,
              event.jobID == jobID
        else {
            return
        }
        recorder.append(event)
    }
    defer {
        NotificationCenter.default.removeObserver(observer)
    }

    let service = CLIProcessingService(executableURL: executable)
    let task = Task {
        try await ProcessingProgressContext.$jobID.withValue(jobID) {
            try await service.runCLI(arguments: ["drizzle"])
        }
    }
    let deadline = ContinuousClock.now + .seconds(15)
    while recorder.snapshot().isEmpty, ContinuousClock.now < deadline {
        try await Task.sleep(for: .milliseconds(10))
    }
    #expect(recorder.snapshot().isEmpty == false)

    task.cancel()
    do {
        _ = try await task.value
        Issue.record("Expected the cancelled CLI task to throw CancellationError")
    } catch is CancellationError {
    } catch {
        Issue.record("Unexpected cancellation error: \(error)")
    }

    let countAfterCancellation = recorder.snapshot().count
    try await Task.sleep(for: .milliseconds(150))
    #expect(recorder.snapshot().count == countAfterCancellation)
}

@Test func cliProcessingServiceRejectsMissingExpectedOutput() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackCLIMissingOutput-\(UUID().uuidString)", isDirectory: true)
    let executable = directory.appendingPathComponent("photonstack")
    let output = directory.appendingPathComponent("missing.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try "#!/usr/bin/env bash\nexit 0\n".write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    do {
        _ = try await service.autoStretch(
            input: directory.appendingPathComponent("input.tiff"),
            output: output,
            targetBackground: 0.25
        )
        Issue.record("Expected missing output to fail")
    } catch let error as ProcessingServiceError {
        #expect(error == .outputMissing(path: output.path))
    }
}

@Test func cliProcessingServiceAcceptsCreatedExpectedOutput() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackCLIValidOutput-\(UUID().uuidString)", isDirectory: true)
    let executable = directory.appendingPathComponent("photonstack")
    let output = directory.appendingPathComponent("created.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output" ]; then
        shift
        \(shellWriteTestPNG(to: "\"$1\""))
      fi
      shift
    done
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    let result = try await service.autoStretch(
        input: directory.appendingPathComponent("input.tiff"),
        output: output,
        targetBackground: 0.25
    )

    #expect(result.exitCode == 0)
    #expect(FileManager.default.fileExists(atPath: output.path))
}

@Test func cliProcessingServiceRejectsCorruptExpectedImageOutput() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLICorruptOutput-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let output = directory.appendingPathComponent("existing.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output" ]; then
        shift
        printf 'not an image' > "$1"
        exit 0
      fi
      shift
    done
    exit 2
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    try Data("original output".utf8).write(to: output)

    let service = CLIProcessingService(executableURL: executable)
    do {
        _ = try await service.autoStretch(
            input: directory.appendingPathComponent("input.tiff"),
            output: output,
            targetBackground: 0.25
        )
        Issue.record("Expected a corrupt image output to fail")
    } catch let error as ProcessingServiceError {
        #expect(error == .outputInvalid(path: output.path))
    }

    #expect(try String(contentsOf: output, encoding: .utf8) == "original output")
    let leftovers = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        .filter { $0.contains("photonstack-output-") }
    #expect(leftovers.isEmpty)
}

@Test func cliProcessingServicePreservesExistingOutputWhenCommandFails() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLIAtomicFailure-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let output = directory.appendingPathComponent("existing.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output" ]; then
        shift
        printf 'partial replacement' > "$1"
        exit 17
      fi
      shift
    done
    exit 17
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    try Data("original output".utf8).write(to: output)

    let service = CLIProcessingService(executableURL: executable)
    do {
        _ = try await service.autoStretch(
            input: directory.appendingPathComponent("input.tiff"),
            output: output,
            targetBackground: 0.25
        )
        Issue.record("Expected the command to fail")
    } catch let error as ProcessingServiceError {
        guard case .commandFailed(exitCode: 17, _) = error else {
            Issue.record("Unexpected processing error: \(error)")
            return
        }
    }

    #expect(try String(contentsOf: output, encoding: .utf8) == "original output")
    let leftovers = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        .filter { $0.contains("photonstack-output-") }
    #expect(leftovers.isEmpty)
}

@Test func cliProcessingServiceAtomicallyReplacesExistingOutputAfterSuccess() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLIAtomicSuccess-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let output = directory.appendingPathComponent("existing.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output" ]; then
        shift
        \(shellWriteTestPNG(to: "\"$1\""))
        printf '{"output":"%s"}\n' "$1"
        exit 0
      fi
      shift
    done
    exit 2
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    try Data("original output".utf8).write(to: output)

    let service = CLIProcessingService(executableURL: executable)
    let result = try await service.autoStretch(
        input: directory.appendingPathComponent("input.tiff"),
        output: output,
        targetBackground: 0.25
    )

    #expect(try Data(contentsOf: output) == Data(base64Encoded: testPNGBase64))
    #expect(result.command.contains(output.path))
    #expect(result.standardOutput.contains(output.path))
    #expect(result.standardOutput.contains("photonstack-output-") == false)
    let leftovers = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        .filter { $0.contains("photonstack-output-") }
    #expect(leftovers.isEmpty)
}

@Test func cliProcessingServicePreservesExistingOutputWhenCancelledAfterPartialWrite() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLIAtomicCancellation-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let output = directory.appendingPathComponent("existing.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    output=''
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output" ]; then
        shift
        output="$1"
      fi
      shift
    done
    printf 'partial replacement' > "$output"
    trap 'exit 0' TERM
    while true; do
      printf '%s\n' '{"type":"progress","task":"stretch","command":"stretch","stage":"write","progress":0.5}'
      sleep 0.05
    done
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    try Data("original output".utf8).write(to: output)

    let service = CLIProcessingService(executableURL: executable)
    let task = Task {
        try await service.autoStretch(
            input: directory.appendingPathComponent("input.tiff"),
            output: output,
            targetBackground: 0.25
        )
    }
    let deadline = ContinuousClock.now + .seconds(15)
    var partialOutputExists = false
    while ContinuousClock.now < deadline {
        let entries = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        if entries.contains(where: { $0.contains("photonstack-output-") }) {
            partialOutputExists = true
            break
        }
        try await Task.sleep(for: .milliseconds(10))
    }
    #expect(partialOutputExists)

    task.cancel()
    do {
        _ = try await task.value
        Issue.record("Expected cancellation")
    } catch is CancellationError {
    } catch {
        Issue.record("Unexpected cancellation error: \(error)")
    }

    #expect(try String(contentsOf: output, encoding: .utf8) == "original output")
    let leftovers = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        .filter { $0.contains("photonstack-output-") }
    #expect(leftovers.isEmpty)
}

@Test func cliProcessingServiceForceTerminatesProcessThatIgnoresCancellation() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLIForcedCancellation-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let output = directory.appendingPathComponent("existing.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    output=''
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output" ]; then
        shift
        output="$1"
      fi
      shift
    done
    printf 'partial replacement' > "$output"
    trap '' TERM
    deadline=$((SECONDS + 5))
    while [ "$SECONDS" -lt "$deadline" ]; do
      :
    done
    exit 0
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    try Data("original output".utf8).write(to: output)

    let service = CLIProcessingService(executableURL: executable)
    let task = Task {
        try await service.autoStretch(
            input: directory.appendingPathComponent("input.tiff"),
            output: output,
            targetBackground: 0.25
        )
    }
    let deadline = ContinuousClock.now + .seconds(15)
    var partialOutputExists = false
    while ContinuousClock.now < deadline {
        let entries = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        if entries.contains(where: { $0.contains("photonstack-output-") }) {
            partialOutputExists = true
            break
        }
        try await Task.sleep(for: .milliseconds(10))
    }
    #expect(partialOutputExists)

    let cancelledAt = ContinuousClock.now
    task.cancel()
    do {
        _ = try await task.value
        Issue.record("Expected cancellation")
    } catch is CancellationError {
    } catch {
        Issue.record("Unexpected cancellation error: \(error)")
    }
    let cancellationDuration = cancelledAt.duration(to: .now)

    #expect(cancellationDuration < .seconds(3))
    #expect(try String(contentsOf: output, encoding: .utf8) == "original output")
    let leftovers = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        .filter { $0.contains("photonstack-output-") }
    #expect(leftovers.isEmpty)
}

@Test func cliProcessingServiceRejectsEmptyExpectedOutputDirectory() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLIEmptyOutputDirectory-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let outputDirectory = directory.appendingPathComponent("cleaned", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output-dir" ]; then
        shift
        mkdir -p "$1"
      fi
      shift
    done
    exit 0
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    do {
        _ = try await service.cleanArtifactSequence(
            inputs: [directory.appendingPathComponent("input.tiff")],
            outputDirectory: outputDirectory
        )
        Issue.record("Expected an empty output directory to fail")
    } catch let error as ProcessingServiceError {
        #expect(error == .outputMissing(path: outputDirectory.path))
    }

    #expect(FileManager.default.fileExists(atPath: outputDirectory.path) == false)
    let leftovers = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        .filter { $0.contains("photonstack-output-") }
    #expect(leftovers.isEmpty)
}

@Test func cliProcessingServiceRejectsOutputDirectoryWithoutImages() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLINonImageOutputDirectory-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let outputDirectory = directory.appendingPathComponent("cleaned", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output-dir" ]; then
        shift
        mkdir -p "$1"
        printf '{"frames":0}\n' > "$1/report.json"
      fi
      shift
    done
    exit 0
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    do {
        _ = try await service.cleanArtifactSequence(
            inputs: [directory.appendingPathComponent("input.tiff")],
            outputDirectory: outputDirectory
        )
        Issue.record("Expected a report-only output directory to fail")
    } catch let error as ProcessingServiceError {
        #expect(error == .outputInvalid(path: outputDirectory.path))
    }

    #expect(FileManager.default.fileExists(atPath: outputDirectory.path) == false)
}

@Test func cliProcessingServicePreservesExistingOutputDirectoryWhenCommandFails() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLIAtomicDirectoryFailure-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let outputDirectory = directory.appendingPathComponent("cleaned", isDirectory: true)
    let original = outputDirectory.appendingPathComponent("original.txt")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)
    try Data("original output".utf8).write(to: original)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output-dir" ]; then
        shift
        mkdir -p "$1"
        printf 'partial output' > "$1/partial.txt"
        exit 17
      fi
      shift
    done
    exit 17
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    do {
        _ = try await service.cleanArtifactSequence(
            inputs: [directory.appendingPathComponent("input.tiff")],
            outputDirectory: outputDirectory
        )
        Issue.record("Expected the directory-producing command to fail")
    } catch let error as ProcessingServiceError {
        guard case .commandFailed(exitCode: 17, _) = error else {
            Issue.record("Unexpected processing error: \(error)")
            return
        }
    }

    #expect(try String(contentsOf: original, encoding: .utf8) == "original output")
    #expect(FileManager.default.fileExists(atPath: outputDirectory.appendingPathComponent("partial.txt").path) == false)
    let leftovers = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        .filter { $0.contains("photonstack-output-") }
    #expect(leftovers.isEmpty)
}

@Test func cliProcessingServiceRejectsPartiallyCorruptExpectedOutputDirectory() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLIPartiallyCorruptOutputDirectory-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let outputDirectory = directory.appendingPathComponent("cleaned", isDirectory: true)
    let original = outputDirectory.appendingPathComponent("original.txt")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)
    try Data("original output".utf8).write(to: original)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output-dir" ]; then
        shift
        mkdir -p "$1"
        \(shellWriteTestPNG(to: "\"$1/valid.png\""))
        printf 'corrupt image' > "$1/corrupt.png"
        exit 0
      fi
      shift
    done
    exit 2
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    do {
        _ = try await service.cleanArtifactSequence(
            inputs: [directory.appendingPathComponent("input.tiff")],
            outputDirectory: outputDirectory
        )
        Issue.record("Expected a partially corrupt output directory to fail")
    } catch let error as ProcessingServiceError {
        #expect(error == .outputInvalid(path: outputDirectory.path))
    }

    #expect(try String(contentsOf: original, encoding: .utf8) == "original output")
    #expect(FileManager.default.fileExists(atPath: outputDirectory.appendingPathComponent("valid.png").path) == false)
    #expect(FileManager.default.fileExists(atPath: outputDirectory.appendingPathComponent("corrupt.png").path) == false)
    let leftovers = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        .filter { $0.contains("photonstack-output-") }
    #expect(leftovers.isEmpty)
}

@Test func cliProcessingServiceAtomicallyReplacesExistingOutputDirectoryAfterSuccess() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLIAtomicDirectorySuccess-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let outputDirectory = directory.appendingPathComponent("cleaned", isDirectory: true)
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)
    try Data("old output".utf8).write(to: outputDirectory.appendingPathComponent("old.txt"))
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output-dir" ]; then
        shift
        mkdir -p "$1"
        \(shellWriteTestPNG(to: "\"$1/new.png\""))
        printf '{"frames":1}\n' > "$1/report.json"
        printf '{"outputDirectory":"%s"}\n' "$1"
        exit 0
      fi
      shift
    done
    exit 2
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    let result = try await service.cleanArtifactSequence(
        inputs: [directory.appendingPathComponent("input.tiff")],
        outputDirectory: outputDirectory
    )

    #expect(try Data(contentsOf: outputDirectory.appendingPathComponent("new.png")) == Data(base64Encoded: testPNGBase64))
    #expect(FileManager.default.fileExists(atPath: outputDirectory.appendingPathComponent("report.json").path))
    #expect(FileManager.default.fileExists(atPath: outputDirectory.appendingPathComponent("old.txt").path) == false)
    #expect(result.command.contains(outputDirectory.path))
    #expect(result.standardOutput.contains(outputDirectory.path))
    #expect(result.standardOutput.contains("photonstack-output-") == false)
    let leftovers = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        .filter { $0.contains("photonstack-output-") }
    #expect(leftovers.isEmpty)
}

@Test func cliProcessingServiceMapsSatelliteArtifactRemoval() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
        "PhotonStackCLISatelliteRemoval-\(UUID().uuidString)",
        isDirectory: true
    )
    let executable = directory.appendingPathComponent("photonstack")
    let output = directory.appendingPathComponent("cleaned.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output" ]; then
        shift
        \(shellWriteTestPNG(to: "\"$1\""))
      fi
      shift
    done
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    let result = try await service.removeArtifacts(
        input: directory.appendingPathComponent("input.tiff"),
        output: output,
        selectedIndices: [1],
        removeAirplanes: true,
        removeDrones: true,
        removeSatellites: true,
        removeMeteors: false
    )

    let kindsIndex = try #require(result.command.firstIndex(of: "--remove-kinds"))
    #expect(result.command[kindsIndex + 1] == "airplane,drone,satellite")
    #expect(result.command.contains("--selected-indices"))
    #expect(FileManager.default.fileExists(atPath: output.path))

    let encodedTrails = "satellite|0.9|10|20|200|22|190|2|0.1|0.8|0.5|0|10:20,200:22"
    let hintedResult = try await service.removeArtifacts(
        input: directory.appendingPathComponent("input.tiff"),
        output: output,
        selectedIndices: [0],
        removeAirplanes: false,
        removeDrones: false,
        removeSatellites: true,
        removeMeteors: false,
        detectedTrailsV1: encodedTrails
    )
    let hintsIndex = try #require(hintedResult.command.firstIndex(of: "--detected-trails-v1"))
    #expect(hintedResult.command[hintsIndex + 1] == encodedTrails)
}

@Test func cliProcessingServiceMapsStarDetectionAndMaskOptions() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackCLIStars-\(UUID().uuidString)", isDirectory: true)
    let executable = directory.appendingPathComponent("photonstack")
    let input = directory.appendingPathComponent("input.tiff")
    let output = directory.appendingPathComponent("mask.png")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output" ]; then
        shift
        \(shellWriteTestPNG(to: "\"$1\""))
      fi
      shift
    done
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    let detection = try await service.detectStars(input: input, sigmaThreshold: 3.4, minPeak: 0.07, maxStars: 640)
    #expect(Array(detection.command.dropFirst()) == [
        "stars", "detect", "--input", input.path,
        "--sigma-threshold", "3.4", "--min-peak", "0.07", "--max-stars", "640", "--summary-only",
    ])

    let mask = try await service.createStarMask(
        input: input,
        output: output,
        radius: 3,
        largeRadius: 9,
        layered: true,
        sigmaThreshold: 3.2,
        minPeak: 0.08,
        maxStars: 5_000
    )
    #expect(Array(mask.command.dropFirst()) == [
        "stars", "mask", "--input", input.path, "--output", output.path,
        "--radius", "3", "--large-radius", "9",
        "--sigma-threshold", "3.2", "--min-peak", "0.08", "--max-stars", "5000", "--layered",
    ])
    #expect(FileManager.default.fileExists(atPath: output.path))
}

@Test func cliProcessingServiceMapsDrizzlePixfrac() async throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackCLIDrizzle-\(UUID().uuidString)", isDirectory: true)
    let executable = directory.appendingPathComponent("photonstack")
    let output = directory.appendingPathComponent("drizzle.tiff")
    defer {
        try? FileManager.default.removeItem(at: directory)
    }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output" ]; then
        shift
        \(shellWriteTestPNG(to: "\"$1\""))
      fi
      shift
    done
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let first = directory.appendingPathComponent("first.tiff")
    let second = directory.appendingPathComponent("second.tiff")
    let service = CLIProcessingService(executableURL: executable)
    let result = try await service.drizzle(
        inputs: [first, second],
        output: output,
        scale: 3,
        pixfrac: 0.7,
        alignment: .none
    )

    #expect(Array(result.command.dropFirst()) == [
        "drizzle", "--output", output.path, "--scale", "3", "--pixfrac", "0.7", "--align", "none",
        first.path, second.path,
    ])
    #expect(FileManager.default.fileExists(atPath: output.path))
}

@Test func cliProcessingServiceMapsFITSValueModeForConversion() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCLIFITSValues-\(UUID().uuidString)", isDirectory: true)
    let executable = directory.appendingPathComponent("photonstack")
    let input = directory.appendingPathComponent("input.fits")
    let output = directory.appendingPathComponent("output.fits")
    defer { try? FileManager.default.removeItem(at: directory) }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output" ]; then
        shift
        printf 'SIMPLE  =                    T' > "$1"
        dd if=/dev/zero bs=2880 count=1 >> "$1" 2>/dev/null
      fi
      shift
    done
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    try Data("fits".utf8).write(to: input)

    let service = CLIProcessingService(executableURL: executable)
    let result = try await service.convert(
        input: input,
        output: output,
        options: ExportOptions(fitsValueMode: .scientific),
        rawOptions: RawProcessingOptions()
    )

    #expect(Array(result.command.dropFirst()) == [
        "convert", "--input", input.path, "--output", output.path,
        "--bit-depth", "auto", "--color-space", "srgb", "--fits-values", "scientific", "--quality", "0.92",
        "--raw-white-balance", "camera", "--raw-temperature", "6500.0", "--raw-tint", "0.0",
        "--raw-exposure-bias", "0.0", "--raw-black-level", "camera", "--raw-black-value", "0.0",
        "--raw-demosaic", "high", "--raw-linear", "on",
    ])
    #expect(FileManager.default.fileExists(atPath: output.path))
}

@Test func cliProcessingServiceMapsAndCommitsFITSBatchRegistration() async throws {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("PhotonStackCLIBatchFITS-\(UUID().uuidString)", isDirectory: true)
    let executable = directory.appendingPathComponent("photonstack")
    let reference = directory.appendingPathComponent("reference.fits")
    let moving = directory.appendingPathComponent("moving.fits")
    let outputDirectory = directory.appendingPathComponent("registered", isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }

    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let script = """
    #!/usr/bin/env bash
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--output-dir" ]; then
        shift
        mkdir -p "$1"
        printf 'SIMPLE  =                    T' > "$1/aligned.fits"
        dd if=/dev/zero bs=2880 count=1 >> "$1/aligned.fits" 2>/dev/null
      fi
      shift
    done
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)

    let service = CLIProcessingService(executableURL: executable)
    let result = try await service.registerBatch(
        reference: reference,
        inputs: [moving],
        outputDirectory: outputDirectory,
        alignment: .distortion,
        outputFormat: "fits"
    )

    #expect(Array(result.command.dropFirst()) == [
        "register-batch", "--reference", reference.path,
        "--output-dir", outputDirectory.path,
        "--mode", "distortion", "--output-format", "fits",
        "--include-reference", "on", moving.path,
    ])
    #expect(FileManager.default.fileExists(atPath: outputDirectory.appendingPathComponent("aligned.fits").path))
}
#endif
