import Foundation
import ImageIO
import PhotonStackAppCore

#if os(macOS)
import Darwin

private struct AtomicOutputTransaction: Sendable {
    let destinationURL: URL
    let temporaryURL: URL
    let processArguments: [String]
    let isDirectory: Bool
    let requiresNewDestination: Bool

    init?(arguments: [String], workingDirectory: URL) {
        // The shared workflow owns a new directory and stores absolute paths in
        // its durable report. Moving a staging directory would invalidate that
        // report and discard the reviewable failure diagnostics.
        if arguments.first == "deep-sky" { return nil }
        let outputIndex: Int
        let isDirectory: Bool
        if let index = arguments.firstIndex(of: "--output"),
           arguments.indices.contains(index + 1) {
            outputIndex = index
            isDirectory = false
        } else if let index = arguments.firstIndex(of: "--output-dir"),
                  arguments.indices.contains(index + 1) {
            outputIndex = index
            isDirectory = true
        } else {
            return nil
        }

        let destinationURL = URL(
            fileURLWithPath: arguments[outputIndex + 1],
            relativeTo: workingDirectory
        ).standardizedFileURL
        let extensionSuffix = destinationURL.pathExtension.isEmpty
            ? ""
            : ".\(destinationURL.pathExtension)"
        let baseName = destinationURL.deletingPathExtension().lastPathComponent
        let temporaryURL = destinationURL.deletingLastPathComponent().appendingPathComponent(
            ".\(baseName).photonstack-output-\(UUID().uuidString)\(extensionSuffix)"
        )
        var processArguments = arguments
        processArguments[outputIndex + 1] = temporaryURL.path

        self.destinationURL = destinationURL
        self.temporaryURL = temporaryURL
        self.processArguments = processArguments
        self.isDirectory = isDirectory
        self.requiresNewDestination = arguments.first == "frequency-stack" || arguments.first == "suppress-grid"
    }

    func commit() throws {
        if isDirectory,
           FileManager.default.fileExists(atPath: destinationURL.path) {
            let result = temporaryURL.withUnsafeFileSystemRepresentation { temporaryPath in
                destinationURL.withUnsafeFileSystemRepresentation { destinationPath in
                    guard let temporaryPath, let destinationPath else {
                        errno = EINVAL
                        return Int32(-1)
                    }
                    return Darwin.renamex_np(temporaryPath, destinationPath, UInt32(RENAME_SWAP))
                }
            }
            guard result == 0 else {
                let error = String(cString: strerror(errno))
                throw ProcessingServiceError.outputCommitFailed(path: destinationURL.path, reason: error)
            }
            try? FileManager.default.removeItem(at: temporaryURL)
            return
        }

        let result = temporaryURL.withUnsafeFileSystemRepresentation { temporaryPath in
            destinationURL.withUnsafeFileSystemRepresentation { destinationPath in
                guard let temporaryPath, let destinationPath else {
                    errno = EINVAL
                    return Int32(-1)
                }
                // Preserve commands' explicit new-output contract after
                // staging, including files created while the CLI was running.
                if requiresNewDestination {
                    return Darwin.renamex_np(temporaryPath, destinationPath, UInt32(RENAME_EXCL))
                }
                return Darwin.rename(temporaryPath, destinationPath)
            }
        }
        guard result == 0 else {
            let error = String(cString: strerror(errno))
            throw ProcessingServiceError.outputCommitFailed(path: destinationURL.path, reason: error)
        }
    }

    func cleanUp() {
        try? FileManager.default.removeItem(at: temporaryURL)
    }

    func restoreDestinationPath(in text: String) -> String {
        text.replacingOccurrences(of: temporaryURL.path, with: destinationURL.path)
    }
}

private enum ExpectedOutputValidation {
    case valid
    case missing
    case invalid
}

private final class ProcessBox: @unchecked Sendable {
    private static let forceTerminationDelay: DispatchTimeInterval = .seconds(1)

    private let lock = NSLock()
    private var process: Process?
    private var cancelled = false
    private var forceTerminationWorkItem: DispatchWorkItem?

    func set(_ process: Process) {
        lock.lock()
        self.process = process
        let shouldTerminate = cancelled
        lock.unlock()

        if shouldTerminate {
            terminate()
        }
    }

    func clear() {
        lock.lock()
        process = nil
        let workItem = forceTerminationWorkItem
        forceTerminationWorkItem = nil
        lock.unlock()

        workItem?.cancel()
    }

    func terminate() {
        lock.lock()
        cancelled = true
        let process = process
        let workItem: DispatchWorkItem?
        if let process, forceTerminationWorkItem == nil {
            let newWorkItem = DispatchWorkItem { [weak self, weak process] in
                guard let self, let process else {
                    return
                }
                self.forceTerminateIfNeeded(process)
            }
            forceTerminationWorkItem = newWorkItem
            workItem = newWorkItem
        } else {
            workItem = nil
        }
        lock.unlock()

        if process?.isRunning == true {
            process?.terminate()
        }
        if let workItem {
            DispatchQueue.global(qos: .utility).asyncAfter(
                deadline: .now() + Self.forceTerminationDelay,
                execute: workItem
            )
        }
    }

    func isCancelled() -> Bool {
        lock.lock()
        let result = cancelled
        lock.unlock()
        return result
    }

    private func forceTerminateIfNeeded(_ expectedProcess: Process) {
        lock.lock()
        let shouldTerminate = cancelled && process === expectedProcess
        lock.unlock()

        if shouldTerminate, expectedProcess.isRunning {
            Darwin.kill(expectedProcess.processIdentifier, SIGKILL)
        }
    }
}

private final class ProcessOutputAccumulator: @unchecked Sendable {
    private let lock = NSLock()
    private let jobID: UUID?
    private let progressScope: ProcessingProgressScope?
    private var stdout = ""
    private var stderr = Data()
    private var stdoutRemainder = Data()
    private var pendingProgress: ProcessingProgressEvent?
    private var lastProgressEmission: ContinuousClock.Instant?

    init(jobID: UUID?, progressScope: ProcessingProgressScope?) {
        self.jobID = jobID
        self.progressScope = progressScope
    }

    func appendStdout(_ data: Data) {
        guard data.isEmpty == false else {
            return
        }

        lock.lock()
        stdoutRemainder.append(data)
        while let newlineIndex = stdoutRemainder.firstIndex(of: 0x0A) {
            let lineData = stdoutRemainder[..<newlineIndex]
            appendStdoutLine(String(decoding: lineData, as: UTF8.self))
            stdoutRemainder.removeSubrange(stdoutRemainder.startIndex...newlineIndex)
        }
        lock.unlock()
    }

    func finishStdout() {
        lock.lock()
        if stdoutRemainder.isEmpty == false {
            appendStdoutLine(String(decoding: stdoutRemainder, as: UTF8.self))
            stdoutRemainder.removeAll(keepingCapacity: false)
        }
        flushProgress()
        lock.unlock()
    }

    func appendStderr(_ data: Data) {
        guard data.isEmpty == false else {
            return
        }

        lock.lock()
        stderr.append(data)
        lock.unlock()
    }

    func snapshot() -> (stdout: String, stderr: String) {
        lock.lock()
        let result = (stdout, String(decoding: stderr, as: UTF8.self))
        lock.unlock()
        return result
    }

    private func appendStdoutLine(_ line: String) {
        if let event = Self.progressEvent(from: line, jobID: jobID, progressScope: progressScope) {
            // Large stacks emit row-level progress much faster than the UI can
            // redraw. Keep the newest event, with a guaranteed final flush at EOF.
            pendingProgress = event
            let now = ContinuousClock.now
            if lastProgressEmission.map({ $0.duration(to: now) >= .milliseconds(200) }) ?? true {
                flushProgress()
            }
        } else {
            stdout += line
            stdout += "\n"
        }
    }

    private func flushProgress() {
        guard let event = pendingProgress else { return }
        pendingProgress = nil
        lastProgressEmission = .now
        NotificationCenter.default.post(name: .photonStackProcessingProgress, object: event)
    }

    private static func progressEvent(
        from line: String,
        jobID: UUID?,
        progressScope: ProcessingProgressScope?
    ) -> ProcessingProgressEvent? {
        guard line.first(where: { $0.isWhitespace == false }) == "{",
              let data = line.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              object["type"] as? String == "progress",
              let task = object["task"] as? String,
              let command = object["command"] as? String,
              let stage = object["stage"] as? String,
              let progressNumber = object["progress"] as? NSNumber,
              CFGetTypeID(progressNumber) != CFBooleanGetTypeID(),
              progressNumber.doubleValue.isFinite
        else {
            return nil
        }

        let progress = progressNumber.doubleValue

        return ProcessingProgressEvent(
            jobID: jobID,
            task: task,
            command: command,
            stage: stage,
            progress: progressScope?.map(progress) ?? progress,
            step: (object["step"] as? NSNumber)?.intValue,
            total: (object["total"] as? NSNumber)?.intValue
        )
    }
}

public final class CLIProcessingService: ProcessingService {
    public let executableURL: URL

    public init(executableURL: URL) {
        self.executableURL = executableURL
    }

    public static func makeDefault(fileManager: FileManager = .default) -> any ProcessingService {
        guard let executableURL = defaultExecutableURL(fileManager: fileManager) else {
            return UnavailableProcessingService()
        }
        return CLIProcessingService(executableURL: executableURL)
    }

    public static func defaultExecutableURL(fileManager: FileManager = .default) -> URL? {
        if let bundledURL = bundledExecutableURL(fileManager: fileManager) {
            return bundledURL
        }

        if let override = ProcessInfo.processInfo.environment["PHOTONSTACK_CLI"], override.isEmpty == false {
            let url = URL(fileURLWithPath: override)
            return fileManager.isExecutableFile(atPath: url.path) ? url : nil
        }

        let currentDirectory = URL(fileURLWithPath: fileManager.currentDirectoryPath, isDirectory: true)
        let candidates = [
            currentDirectory.appendingPathComponent("build/debug/apps/PhotonStackCLI/photonstack"),
            currentDirectory.appendingPathComponent("build/release/apps/PhotonStackCLI/photonstack"),
            currentDirectory.appendingPathComponent("build/apps/PhotonStackCLI/photonstack"),
        ]

        return candidates.first { fileManager.isExecutableFile(atPath: $0.path) }
    }

    public static func bundledExecutableURL(bundle: Bundle = .main, fileManager: FileManager = .default) -> URL? {
        let bundleURL = bundle.bundleURL
        let candidates = [
            bundleURL.appendingPathComponent("Contents/MacOS/photonstack"),
            bundleURL.appendingPathComponent("Contents/Resources/photonstack"),
            bundleURL.deletingLastPathComponent().appendingPathComponent("photonstack"),
        ]

        return candidates.first { fileManager.isExecutableFile(atPath: $0.path) }
    }

    public func inspect(_ asset: PhotonStackAsset) async throws -> ProcessingCommandResult {
        try await run(["inspect", asset.originalURL.path])
    }

    public func convert(input: URL, output: URL, options: ExportOptions, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        try await run([
            "convert",
            "--input", input.path,
            "--output", output.path,
            "--bit-depth", options.bitDepth.cliValue,
            "--color-space", options.colorSpace.cliValue,
            "--fits-values", options.fitsValueMode.cliValue,
            "--quality", String(options.jpegQuality),
        ] + Self.rawArguments(for: rawOptions))
    }

    public func createMaster(output: URL, method: StackMethod, inputs: [URL], rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        try Self.validateCompatibleDecodedColorEncodings(inputs, command: "master")
        return try await run([
            "master",
            "--output", output.path,
            "--method", method.rawValue,
        ] + Self.rawArguments(for: rawOptions) + inputs.map(\.path))
    }

    public func calibrate(
        light: URL,
        output: URL,
        dark: URL?,
        bias: URL?,
        flat: URL?,
        darkBiasState: CalibrationBiasState,
        flatBiasState: CalibrationBiasState,
        rawOptions: RawProcessingOptions
    ) async throws -> ProcessingCommandResult {
        try Self.validateCompatibleDecodedColorEncodings(
            [light, dark, bias, flat].compactMap { $0 },
            command: "calibrate"
        )
        var arguments = [
            "calibrate",
            "--light", light.path,
            "--output", output.path,
        ]

        if let dark {
            arguments.append(contentsOf: ["--dark", dark.path])
        }
        if let bias {
            arguments.append(contentsOf: ["--bias", bias.path])
        }
        if let flat {
            arguments.append(contentsOf: ["--flat", flat.path])
        }
        if dark != nil {
            arguments.append(contentsOf: ["--dark-bias", darkBiasState.rawValue])
        }
        if flat != nil {
            arguments.append(contentsOf: ["--flat-bias", flatBiasState.rawValue])
        }

        return try await run(arguments + Self.rawArguments(for: rawOptions))
    }

    public func quality(input: URL) async throws -> ProcessingCommandResult {
        try await run([
            "quality",
            "--input", input.path,
        ])
    }

    public func register(reference: URL, moving: URL, output: URL, alignment: AlignmentMethod) async throws -> ProcessingCommandResult {
        let mode = alignment == .none ? AlignmentMethod.translation : alignment
        return try await run([
            "register",
            "--reference", reference.path,
            "--moving", moving.path,
            "--output", output.path,
            "--mode", mode.rawValue,
        ])
    }

    public func registerBatch(
        reference: URL,
        inputs: [URL],
        outputDirectory: URL,
        alignment: AlignmentMethod,
        outputFormat: String
    ) async throws -> ProcessingCommandResult {
        let mode = alignment == .none ? AlignmentMethod.distortion : alignment
        var arguments = [
            "register-batch",
            "--reference", reference.path,
            "--output-dir", outputDirectory.path,
            "--mode", mode.rawValue,
            "--output-format", outputFormat,
            "--include-reference", "on",
        ]
        arguments.append(contentsOf: inputs.map(\.path))
        return try await run(arguments)
    }

    public func stack(inputs: [URL], output: URL, method: StackMethod, alignment: AlignmentMethod) async throws -> ProcessingCommandResult {
        try Self.validateCompatibleDecodedColorEncodings(inputs, command: "stack")
        var arguments = [
            "stack",
            "--output", output.path,
            "--method", method.rawValue,
        ]

        if alignment != .none {
            arguments.append(contentsOf: ["--align", alignment.rawValue])
        }

        arguments.append(contentsOf: inputs.map(\.path))
        return try await run(arguments)
    }

    public func mosaic(
        inputs: [URL],
        output: URL,
        overlapPixels: Int = 0,
        projection: MosaicProjection = .planar,
        layout: MosaicLayoutMode = .horizontal,
        alignment: MosaicAlignmentMode = .manual,
        blendMode: MosaicBlendMode = .feather,
        exposureMatching: Bool = true,
        columns: Int = 1,
        previewWidth: Int? = nil
    ) async throws -> ProcessingCommandResult {
        try Self.validateCompatibleDecodedColorEncodings(inputs, command: "mosaic")
        var arguments = [
            "mosaic",
            "--output", output.path,
            "--overlap-pixels", String(overlapPixels),
            "--projection", projection.rawValue,
            "--layout", layout.rawValue,
            "--columns", String(columns),
            "--alignment", alignment.rawValue,
            "--blend", blendMode.rawValue,
            "--exposure-match", exposureMatching ? "on" : "off",
        ]
        if let previewWidth {
            arguments.append(contentsOf: ["--preview-width", String(previewWidth)])
        }
        arguments.append(contentsOf: inputs.map(\.path))
        return try await run(arguments)
    }

    public func makePreview(input: URL, output: URL, width: Int = 1600, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        try await run([
            "preview",
            "--input", input.path,
            "--output", output.path,
            "--width", String(width),
        ] + Self.rawArguments(for: rawOptions))
    }

    public func makeCurvePreview(
        input: URL,
        output: URL,
        width: Int = 1600,
        rawOptions: RawProcessingOptions,
        points: [(Double, Double)],
        channel: CurveChannel
    ) async throws -> ProcessingCommandResult {
        let encodedPoints = points
            .map { "\($0.0):\($0.1)" }
            .joined(separator: ",")
        return try await run([
            "preview",
            "--input", input.path,
            "--output", output.path,
            "--width", String(width),
            "--curve-points", encodedPoints,
            "--curve-channel", channel.rawValue,
        ] + Self.rawArguments(for: rawOptions))
    }

    public func adjustRawPreview(input: URL, output: URL, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        try await run([
            "raw",
            "adjust",
            "--input", input.path,
            "--output", output.path,
        ] + Self.rawArguments(for: rawOptions))
    }

    public func histogram(input: URL, bins: Int = 64) async throws -> ProcessingCommandResult {
        try await run([
            "histogram",
            "--input", input.path,
            "--bins", String(bins),
        ])
    }

    public func background(
        input: URL,
        output: URL,
        model: String = "grid",
        mode: String = "subtract",
        strength: Double = 0.35,
        preserveBrightness: Bool = true,
        protectBrightTargets: Bool = true
    ) async throws -> ProcessingCommandResult {
        try await run([
            "background",
            "--input", input.path,
            "--output", output.path,
            "--model", model,
            "--mode", mode,
            "--strength", String(strength),
            "--preserve-brightness", preserveBrightness ? "on" : "off",
            "--protect-bright-targets", protectBrightTargets ? "on" : "off",
        ])
    }

    public func detectClouds(input: URL) async throws -> ProcessingCommandResult {
        try await run([
            "clouds",
            "detect",
            "--input", input.path,
        ])
    }

    public func removeClouds(
        input: URL,
        output: URL,
        selectedIndices: [Int]? = nil,
        strength: Double = 0.65
    ) async throws -> ProcessingCommandResult {
        var arguments = [
            "clouds",
            "remove",
            "--input", input.path,
            "--output", output.path,
            "--strength", String(strength),
        ]
        if let selectedIndices, selectedIndices.isEmpty == false {
            arguments.append(contentsOf: [
                "--selected-indices",
                selectedIndices.map(String.init).joined(separator: ","),
            ])
        }
        return try await run(arguments)
    }

    public func normalize(input: URL, output: URL, targetBackground: Double = 0.25, targetScale: Double = 1.0) async throws -> ProcessingCommandResult {
        try await run([
            "normalize",
            "--input", input.path,
            "--output", output.path,
            "--target-background", String(targetBackground),
            "--target-scale", String(targetScale),
        ])
    }

    public func curves(input: URL, output: URL, points: [(Double, Double)], channel: CurveChannel) async throws -> ProcessingCommandResult {
        let encodedPoints = points
            .map { "\($0.0):\($0.1)" }
            .joined(separator: ",")

        return try await run([
            "curves",
            "--input", input.path,
            "--output", output.path,
            "--points", encodedPoints,
            "--channel", channel.rawValue,
        ])
    }

    public func localContrast(input: URL, output: URL, amount: Double = 0.25, radius: Int = 8) async throws -> ProcessingCommandResult {
        try await run([
            "local-contrast",
            "--input", input.path,
            "--output", output.path,
            "--amount", String(amount),
            "--radius", String(radius),
        ])
    }

    public func denoise(input: URL, output: URL, amount: Double = 0.4, chromaAmount: Double = 0.55, radius: Int = 1) async throws -> ProcessingCommandResult {
        try await run([
            "denoise",
            "--input", input.path,
            "--output", output.path,
            "--amount", String(amount),
            "--chroma-amount", String(chromaAmount),
            "--radius", String(radius),
        ])
    }

    public func sharpen(input: URL, output: URL, amount: Double = 0.35, radius: Int = 1) async throws -> ProcessingCommandResult {
        try await run([
            "sharpen",
            "--input", input.path,
            "--output", output.path,
            "--amount", String(amount),
            "--radius", String(radius),
        ])
    }

    public func deconvolve(input: URL, output: URL, iterations: Int = 8, radius: Int = 2, sigma: Double = 1.2) async throws -> ProcessingCommandResult {
        try await run([
            "deconvolve",
            "--input", input.path,
            "--output", output.path,
            "--iterations", String(iterations),
            "--radius", String(radius),
            "--sigma", String(sigma),
        ])
    }

    public func colorNeutralize(input: URL, output: URL, strength: Double = 1.0) async throws -> ProcessingCommandResult {
        try await run([
            "color",
            "neutralize",
            "--input", input.path,
            "--output", output.path,
            "--strength", String(strength),
        ])
    }

    public func colorSaturate(input: URL, output: URL, amount: Double = 0.2) async throws -> ProcessingCommandResult {
        try await run([
            "color",
            "saturate",
            "--input", input.path,
            "--output", output.path,
            "--amount", String(amount),
        ])
    }

    public func autoStretch(input: URL, output: URL, targetBackground: Double = 0.25) async throws -> ProcessingCommandResult {
        try await run([
            "stretch",
            "--input", input.path,
            "--output", output.path,
            "--auto",
            "--target-background", String(targetBackground),
        ])
    }

    public func detectStars(
        input: URL,
        sigmaThreshold: Double = 3.0,
        minPeak: Double = 0.05,
        maxStars: Int = 50_000
    ) async throws -> ProcessingCommandResult {
        try await run([
            "stars",
            "detect",
            "--input", input.path,
            "--sigma-threshold", String(sigmaThreshold),
            "--min-peak", String(minPeak),
            "--max-stars", String(maxStars),
            "--summary-only",
        ])
    }

    public func createStarMask(
        input: URL,
        output: URL,
        radius: Int = 2,
        largeRadius: Int = 6,
        layered: Bool = true,
        sigmaThreshold: Double = 3.0,
        minPeak: Double = 0.05,
        maxStars: Int = 50_000
    ) async throws -> ProcessingCommandResult {
        var arguments = [
            "stars",
            "mask",
            "--input", input.path,
            "--output", output.path,
            "--radius", String(radius),
            "--large-radius", String(largeRadius),
            "--sigma-threshold", String(sigmaThreshold),
            "--min-peak", String(minPeak),
            "--max-stars", String(maxStars),
        ]
        if layered {
            arguments.append("--layered")
        }
        return try await run(arguments)
    }

    public func reduceStars(
        input: URL,
        output: URL,
        amount: Double = 0.35,
        profileAware: Bool = true,
        edgeAware: Bool = true
    ) async throws -> ProcessingCommandResult {
        var arguments = [
            "stars",
            "reduce",
            "--input", input.path,
            "--output", output.path,
            "--amount", String(amount),
            "--edge-aware", edgeAware ? "on" : "off",
        ]

        if profileAware {
            arguments.append("--profile-aware")
        }

        return try await run(arguments)
    }

    public func removeGreenCast(input: URL, output: URL, amount: Double = 0.65, backgroundLimit: Double = 0.32) async throws -> ProcessingCommandResult {
        try await run([
            "color",
            "remove-green",
            "--input", input.path,
            "--output", output.path,
            "--amount", String(amount),
            "--background-limit", String(backgroundLimit),
        ])
    }

    public func reduceComa(
        input: URL,
        output: URL,
        amount: Double = 0.80,
        radius: Int = 10,
        eccentricity: Double = 0.22,
        edgeAware: Bool = true
    ) async throws -> ProcessingCommandResult {
        try await run([
            "stars",
            "coma",
            "--input", input.path,
            "--output", output.path,
            "--amount", String(amount),
            "--radius", String(radius),
            "--eccentricity", String(eccentricity),
            "--edge-aware", edgeAware ? "on" : "off",
        ])
    }

    public func detectArtifacts(input: URL) async throws -> ProcessingCommandResult {
        try await run([
            "artifacts",
            "detect",
            "--input", input.path,
            "--preserve-meteors", "on",
        ])
    }

    public func removeArtifacts(
        input: URL,
        output: URL,
        selectedIndices: [Int]? = nil,
        removeAirplanes: Bool = true,
        removeDrones: Bool = true,
        removeSatellites: Bool = true,
        removeMeteors: Bool = false
    ) async throws -> ProcessingCommandResult {
        try await removeArtifacts(
            input: input,
            output: output,
            selectedIndices: selectedIndices,
            removeAirplanes: removeAirplanes,
            removeDrones: removeDrones,
            removeSatellites: removeSatellites,
            removeMeteors: removeMeteors,
            detectedTrailsV1: nil
        )
    }

    public func removeArtifacts(
        input: URL,
        output: URL,
        selectedIndices: [Int]? = nil,
        removeAirplanes: Bool = true,
        removeDrones: Bool = true,
        removeSatellites: Bool = true,
        removeMeteors: Bool = false,
        detectedTrailsV1: String?
    ) async throws -> ProcessingCommandResult {
        var kinds: [String] = []
        if removeAirplanes {
            kinds.append("airplane")
        }
        if removeDrones {
            kinds.append("drone")
        }
        if removeSatellites {
            kinds.append("satellite")
        }
        if removeMeteors {
            kinds.append("meteor")
        }
        var arguments = [
            "artifacts",
            "remove",
            "--input", input.path,
            "--output", output.path,
            "--remove-kinds", kinds.joined(separator: ","),
            "--preserve-meteors", removeMeteors ? "off" : "on",
        ]
        if let selectedIndices, selectedIndices.isEmpty == false {
            arguments.append(contentsOf: [
                "--selected-indices",
                selectedIndices.map(String.init).joined(separator: ","),
            ])
        }
        if let detectedTrailsV1 {
            arguments.append(contentsOf: ["--detected-trails-v1", detectedTrailsV1])
        }
        return try await run(arguments)
    }

    public func cleanArtifactSequence(
        inputs: [URL],
        outputDirectory: URL,
        outputFormat: String = "png",
        minWeight: Double = 0.30,
        recurrenceThreshold: Int = 2
    ) async throws -> ProcessingCommandResult {
        try await run([
            "artifacts",
            "clean-sequence",
            "--output-dir", outputDirectory.path,
            "--output-format", outputFormat,
            "--min-weight", String(minWeight),
            "--recurrence-threshold", String(recurrenceThreshold),
            "--remove-kinds", "airplane,drone,satellite",
            "--preserve-meteors", "on",
        ] + inputs.map(\.path))
    }

    public func extractMeteors(input: URL, output: URL) async throws -> ProcessingCommandResult {
        try await run([
            "meteors",
            "extract",
            "--input", input.path,
            "--output", output.path,
        ])
    }

    public func restoreMeteors(base: URL, source: URL, output: URL) async throws -> ProcessingCommandResult {
        try Self.validateCompatibleMeteorRestoreColorEncodings([base, source])
        return try await run([
            "meteors",
            "restore",
            "--base", base.path,
            "--source", source.path,
            "--output", output.path,
        ])
    }

    public func drizzle(
        inputs: [URL],
        output: URL,
        scale: Int = 2,
        pixfrac: Double = 1.0,
        alignment: AlignmentMethod = .distortion
    ) async throws -> ProcessingCommandResult {
        try Self.validateCompatibleDecodedColorEncodings(inputs, command: "drizzle")
        let arguments = [
            "drizzle",
            "--output", output.path,
            "--scale", String(scale),
            "--pixfrac", String(pixfrac),
            "--align", alignment.rawValue,
        ] + inputs.map(\.path)
        return try await run(arguments)
    }

    public func runWorkflow(_ workflow: URL) async throws -> ProcessingCommandResult {
        try await run(["run", workflow.path])
    }

    public func runCLI(arguments: [String]) async throws -> ProcessingCommandResult {
        try await run(arguments)
    }

    private enum DecodedColorEncoding {
        case linear
        case srgb
    }

    private static func decodedColorEncoding(for input: URL) -> DecodedColorEncoding? {
        switch AssetKind.detect(from: input) {
        case .raw, .fits:
            return .linear
        case .tiff, .jpeg, .heif, .png:
            return .srgb
        case .unknown:
            return nil
        }
    }

    private static func validateCompatibleDecodedColorEncodings(
        _ inputs: [URL],
        command: String
    ) throws {
        var expected: DecodedColorEncoding?
        for input in inputs {
            guard let encoding = decodedColorEncoding(for: input) else {
                continue
            }
            if let expected, expected != encoding {
                throw ProcessingServiceError.inputColorEncodingMismatch(
                    command: command,
                    inputs: inputs.map(\.path)
                )
            }
            expected = encoding
        }
    }

    private static func validateCompatibleMeteorRestoreColorEncodings(_ inputs: [URL]) throws {
        var expected: DecodedColorEncoding?
        for input in inputs {
            let encoding: DecodedColorEncoding?
            switch AssetKind.detect(from: input) {
            case .fits:
                encoding = .linear
            case .raw, .tiff, .jpeg, .heif, .png:
                encoding = .srgb
            case .unknown:
                encoding = nil
            }
            guard let encoding else {
                continue
            }
            if let expected, expected != encoding {
                throw ProcessingServiceError.inputColorEncodingMismatch(
                    command: "meteors restore",
                    inputs: inputs.map(\.path)
                )
            }
            expected = encoding
        }
    }

    private static func rawArguments(for options: RawProcessingOptions) -> [String] {
        [
            "--raw-white-balance", options.whiteBalanceMode.cliValue,
            "--raw-temperature", String(options.manualWhiteBalanceTemperature),
            "--raw-tint", String(options.manualWhiteBalanceTint),
            "--raw-exposure-bias", String(options.exposureBias),
            "--raw-black-level", options.blackLevelMode.cliValue,
            "--raw-black-value", String(options.manualBlackLevel),
            "--raw-demosaic", options.demosaicQuality.cliValue,
            "--raw-linear", options.linearOutput ? "on" : "off",
        ]
    }

    private func run(_ arguments: [String]) async throws -> ProcessingCommandResult {
        let processBox = ProcessBox()
        let startedAt = Date()
        let progressJobID = ProcessingProgressContext.jobID
        let progressScope = ProcessingProgressContext.progressScope
        let workingDirectory = executableURL.deletingLastPathComponent()
        let outputTransaction = AtomicOutputTransaction(
            arguments: arguments,
            workingDirectory: workingDirectory
        )
        let processArguments = outputTransaction?.processArguments ?? arguments

        return try await withTaskCancellationHandler {
            try Task.checkCancellation()

            return try await withCheckedThrowingContinuation { continuation in
                let process = Process()
                let outputPipe = Pipe()
                let errorPipe = Pipe()

                process.executableURL = executableURL
                process.arguments = processArguments
                process.currentDirectoryURL = workingDirectory
                let inheritedEnvironment = ProcessInfo.processInfo.environment
                var environment: [String: String] = [
                    "PATH": inheritedEnvironment["PATH"] ?? "/usr/bin:/bin:/usr/sbin:/sbin",
                    "HOME": inheritedEnvironment["HOME"] ?? NSHomeDirectory(),
                    "TMPDIR": inheritedEnvironment["TMPDIR"] ?? NSTemporaryDirectory(),
                    "PWD": workingDirectory.path,
                    "LC_ALL": "C",
                    "LANG": "C",
                    "PHOTONSTACK_PROGRESS": "1",
                ]
                if let rawCache = inheritedEnvironment["LIBRAW_DATA_PATH"] {
                    environment["LIBRAW_DATA_PATH"] = rawCache
                }
                process.environment = environment
                process.standardOutput = outputPipe
                process.standardError = errorPipe
                processBox.set(process)

                let accumulator = ProcessOutputAccumulator(jobID: progressJobID, progressScope: progressScope)
                let outputGroup = DispatchGroup()
                outputGroup.enter()
                DispatchQueue.global(qos: .utility).async {
                    while true {
                        let data = outputPipe.fileHandleForReading.availableData
                        if data.isEmpty {
                            accumulator.finishStdout()
                            outputGroup.leave()
                            return
                        }
                        accumulator.appendStdout(data)
                    }
                }
                outputGroup.enter()
                DispatchQueue.global(qos: .utility).async {
                    while true {
                        let data = errorPipe.fileHandleForReading.availableData
                        if data.isEmpty {
                            outputGroup.leave()
                            return
                        }
                        accumulator.appendStderr(data)
                    }
                }

                process.terminationHandler = { process in
                    processBox.clear()

                    outputGroup.notify(queue: .global(qos: .utility)) {
                        let output = accumulator.snapshot()
                        let stdout = outputTransaction?.restoreDestinationPath(in: output.stdout) ?? output.stdout
                        let stderr = outputTransaction?.restoreDestinationPath(in: output.stderr) ?? output.stderr
                        let command = [self.executableURL.path] + arguments

                        if processBox.isCancelled() {
                            outputTransaction?.cleanUp()
                            continuation.resume(throwing: CancellationError())
                        } else if process.terminationStatus == 0 {
                            if let expectedOutput = Self.expectedOutput(in: processArguments) {
                                let outputPath = outputTransaction?.destinationURL.path ?? expectedOutput.path
                                switch Self.validateOutput(expectedOutput, relativeTo: workingDirectory) {
                                case .valid:
                                    break
                                case .missing:
                                    outputTransaction?.cleanUp()
                                    continuation.resume(
                                        throwing: ProcessingServiceError.outputMissing(path: outputPath)
                                    )
                                    return
                                case .invalid:
                                    outputTransaction?.cleanUp()
                                    continuation.resume(
                                        throwing: ProcessingServiceError.outputInvalid(path: outputPath)
                                    )
                                    return
                                }
                            }
                            do {
                                try outputTransaction?.commit()
                            } catch {
                                outputTransaction?.cleanUp()
                                continuation.resume(throwing: error)
                                return
                            }
                            let duration = Date().timeIntervalSince(startedAt) * 1000.0
                            continuation.resume(
                                returning: ProcessingCommandResult(
                                    command: command,
                                    exitCode: process.terminationStatus,
                                    standardOutput: stdout,
                                    standardError: stderr,
                                    durationMilliseconds: duration
                                )
                            )
                        } else {
                            outputTransaction?.cleanUp()
                            continuation.resume(
                                throwing: ProcessingServiceError.commandFailed(
                                    exitCode: process.terminationStatus,
                                    stderr: stderr.isEmpty ? stdout : stderr
                                )
                            )
                        }
                    }
                }

                do {
                    try process.run()
                    if processBox.isCancelled() {
                        processBox.terminate()
                    }
                } catch {
                    processBox.clear()
                    outputTransaction?.cleanUp()
                    continuation.resume(throwing: error)
                }
            }
        } onCancel: {
            processBox.terminate()
        }
    }

    private static func expectedOutput(in arguments: [String]) -> (path: String, isDirectory: Bool)? {
        // Assessment-only runs intentionally contain no image. The typed shared
        // workflow service validates the report and any required image outputs.
        if arguments.first == "deep-sky" { return nil }
        if let index = arguments.firstIndex(of: "--output"), arguments.indices.contains(index + 1) {
            return (arguments[index + 1], false)
        }
        if let index = arguments.firstIndex(of: "--output-dir"), arguments.indices.contains(index + 1) {
            return (arguments[index + 1], true)
        }
        return nil
    }

    private static func validateOutput(
        _ expectedOutput: (path: String, isDirectory: Bool),
        relativeTo workingDirectory: URL
    ) -> ExpectedOutputValidation {
        let url = URL(fileURLWithPath: expectedOutput.path, relativeTo: workingDirectory).standardizedFileURL
        var isDirectory = ObjCBool(false)
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory),
              isDirectory.boolValue == expectedOutput.isDirectory
        else {
            return .missing
        }
        guard expectedOutput.isDirectory == false else {
            guard let enumerator = FileManager.default.enumerator(
                at: url,
                includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey],
                options: [.skipsHiddenFiles]
            ) else {
                return .missing
            }
            var foundRegularFile = false
            var foundReadableImage = false
            for case let childURL as URL in enumerator {
                guard let values = try? childURL.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]) else {
                    return .invalid
                }
                guard values.isRegularFile == true else {
                    continue
                }
                foundRegularFile = true
                guard (values.fileSize ?? 0) > 0 else {
                    return .invalid
                }
                if Self.isRecognizedImage(childURL) {
                    guard Self.isReadableImage(childURL) else {
                        return .invalid
                    }
                    foundReadableImage = true
                }
            }
            guard foundRegularFile else {
                return .missing
            }
            return foundReadableImage ? .valid : .invalid
        }
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
              let size = attributes[.size] as? NSNumber,
              size.int64Value > 0
        else {
            return .missing
        }

        guard Self.isRecognizedImage(url) else {
            return .valid
        }
        return Self.isReadableImage(url) ? .valid : .invalid
    }

    private static func isRecognizedImage(_ url: URL) -> Bool {
        let imageExtensions: Set<String> = [
            "png", "jpg", "jpeg", "tif", "tiff", "heic", "heif", "hif",
            "fit", "fits", "fts",
        ]
        return imageExtensions.contains(url.pathExtension.lowercased())
    }

    private static func isReadableImage(_ url: URL) -> Bool {
        if ["fit", "fits", "fts"].contains(url.pathExtension.lowercased()) {
            return isReadableFITS(url)
        }
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              CGImageSourceGetCount(source) > 0
        else {
            return false
        }
        let options = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceThumbnailMaxPixelSize: 8,
            kCGImageSourceShouldCacheImmediately: true,
        ] as CFDictionary
        return CGImageSourceCreateThumbnailAtIndex(source, 0, options) != nil
    }

    private static func isReadableFITS(_ url: URL) -> Bool {
        guard let handle = try? FileHandle(forReadingFrom: url) else {
            return false
        }
        defer { try? handle.close() }
        let header: Data
        let fileSize: UInt64
        do {
            guard let data = try handle.read(upToCount: 80) else {
                return false
            }
            header = data
            fileSize = try handle.seekToEnd()
        } catch {
            return false
        }
        guard header.count == 80, fileSize >= 2_880 else {
            return false
        }
        let keyword = String(decoding: header.prefix(10), as: UTF8.self)
        return keyword.hasPrefix("SIMPLE  =") || keyword.hasPrefix("XTENSION=")
    }
}
#endif
