import Foundation
import PhotonStackAppCore

public protocol ProcessingService: Sendable {
    var requiresExistingInputFiles: Bool { get }

    func inspect(_ asset: PhotonStackAsset) async throws -> ProcessingCommandResult
    func convert(input: URL, output: URL, options: ExportOptions, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult
    func createMaster(output: URL, method: StackMethod, inputs: [URL], rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult
    func calibrate(
        light: URL,
        output: URL,
        dark: URL?,
        bias: URL?,
        flat: URL?,
        darkBiasState: CalibrationBiasState,
        flatBiasState: CalibrationBiasState,
        rawOptions: RawProcessingOptions
    ) async throws -> ProcessingCommandResult
    func quality(input: URL) async throws -> ProcessingCommandResult
    func register(reference: URL, moving: URL, output: URL, alignment: AlignmentMethod) async throws -> ProcessingCommandResult
    func registerBatch(reference: URL, inputs: [URL], outputDirectory: URL, alignment: AlignmentMethod, outputFormat: String) async throws -> ProcessingCommandResult
    func stack(inputs: [URL], output: URL, method: StackMethod, alignment: AlignmentMethod) async throws -> ProcessingCommandResult
    func mosaic(
        inputs: [URL],
        output: URL,
        overlapPixels: Int,
        projection: MosaicProjection,
        layout: MosaicLayoutMode,
        alignment: MosaicAlignmentMode,
        blendMode: MosaicBlendMode,
        exposureMatching: Bool,
        columns: Int,
        previewWidth: Int?
    ) async throws -> ProcessingCommandResult
    func makePreview(input: URL, output: URL, width: Int, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult
    func makeCurvePreview(
        input: URL,
        output: URL,
        width: Int,
        rawOptions: RawProcessingOptions,
        points: [(Double, Double)],
        channel: CurveChannel
    ) async throws -> ProcessingCommandResult
    func adjustRawPreview(input: URL, output: URL, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult
    func histogram(input: URL, bins: Int) async throws -> ProcessingCommandResult
    func background(
        input: URL,
        output: URL,
        model: String,
        mode: String,
        strength: Double,
        preserveBrightness: Bool,
        protectBrightTargets: Bool
    ) async throws -> ProcessingCommandResult
    func detectClouds(input: URL) async throws -> ProcessingCommandResult
    func removeClouds(input: URL, output: URL, selectedIndices: [Int]?, strength: Double) async throws -> ProcessingCommandResult
    func normalize(input: URL, output: URL, targetBackground: Double, targetScale: Double) async throws -> ProcessingCommandResult
    func curves(input: URL, output: URL, points: [(Double, Double)], channel: CurveChannel) async throws -> ProcessingCommandResult
    func autoStretch(input: URL, output: URL, targetBackground: Double) async throws -> ProcessingCommandResult
    func localContrast(input: URL, output: URL, amount: Double, radius: Int) async throws -> ProcessingCommandResult
    func denoise(input: URL, output: URL, amount: Double, chromaAmount: Double, radius: Int) async throws -> ProcessingCommandResult
    func sharpen(input: URL, output: URL, amount: Double, radius: Int) async throws -> ProcessingCommandResult
    func deconvolve(input: URL, output: URL, iterations: Int, radius: Int, sigma: Double) async throws -> ProcessingCommandResult
    func colorNeutralize(input: URL, output: URL, strength: Double) async throws -> ProcessingCommandResult
    func colorSaturate(input: URL, output: URL, amount: Double) async throws -> ProcessingCommandResult
    func removeGreenCast(input: URL, output: URL, amount: Double, backgroundLimit: Double) async throws -> ProcessingCommandResult
    func detectStars(input: URL, sigmaThreshold: Double, minPeak: Double, maxStars: Int) async throws -> ProcessingCommandResult
    func createStarMask(input: URL, output: URL, radius: Int, largeRadius: Int, layered: Bool, sigmaThreshold: Double, minPeak: Double, maxStars: Int) async throws -> ProcessingCommandResult
    func reduceStars(input: URL, output: URL, amount: Double, profileAware: Bool, edgeAware: Bool) async throws -> ProcessingCommandResult
    func reduceComa(input: URL, output: URL, amount: Double, radius: Int, eccentricity: Double, edgeAware: Bool) async throws -> ProcessingCommandResult
    func detectArtifacts(input: URL) async throws -> ProcessingCommandResult
    func removeArtifacts(input: URL, output: URL, selectedIndices: [Int]?, removeAirplanes: Bool, removeDrones: Bool, removeSatellites: Bool, removeMeteors: Bool) async throws -> ProcessingCommandResult
    func cleanArtifactSequence(inputs: [URL], outputDirectory: URL, outputFormat: String, minWeight: Double, recurrenceThreshold: Int) async throws -> ProcessingCommandResult
    func extractMeteors(input: URL, output: URL) async throws -> ProcessingCommandResult
    func restoreMeteors(base: URL, source: URL, output: URL) async throws -> ProcessingCommandResult
    func drizzle(inputs: [URL], output: URL, scale: Int, pixfrac: Double, alignment: AlignmentMethod) async throws -> ProcessingCommandResult
    func runCLI(arguments: [String]) async throws -> ProcessingCommandResult
    func runWorkflow(_ workflow: URL) async throws -> ProcessingCommandResult
}

public extension ProcessingService {
    var requiresExistingInputFiles: Bool {
        true
    }

    func convert(input: URL, output: URL, options: ExportOptions) async throws -> ProcessingCommandResult {
        try await convert(input: input, output: output, options: options, rawOptions: RawProcessingOptions())
    }

    func convert(input: URL, output: URL) async throws -> ProcessingCommandResult {
        try await convert(input: input, output: output, options: ExportOptions())
    }

    func makePreview(input: URL, output: URL, width: Int) async throws -> ProcessingCommandResult {
        try await makePreview(input: input, output: output, width: width, rawOptions: RawProcessingOptions())
    }

    func makeCurvePreview(
        input: URL,
        output: URL,
        width: Int,
        rawOptions: RawProcessingOptions,
        points: [(Double, Double)],
        channel: CurveChannel
    ) async throws -> ProcessingCommandResult {
        _ = try await makePreview(input: input, output: output, width: width, rawOptions: rawOptions)
        return try await curves(input: output, output: output, points: points, channel: channel)
    }

    func registerBatch(
        reference: URL,
        inputs: [URL],
        outputDirectory: URL,
        alignment: AlignmentMethod
    ) async throws -> ProcessingCommandResult {
        try await registerBatch(
            reference: reference,
            inputs: inputs,
            outputDirectory: outputDirectory,
            alignment: alignment,
            outputFormat: "tiff"
        )
    }

    func adjustRawPreview(input: URL, output: URL, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        try await convert(input: input, output: output, options: ExportOptions(), rawOptions: rawOptions)
    }
}

public struct ProcessingCommandResult: Equatable, Sendable {
    public var command: [String]
    public var exitCode: Int32
    public var standardOutput: String
    public var standardError: String
    public var durationMilliseconds: Double?

    public init(
        command: [String],
        exitCode: Int32,
        standardOutput: String,
        standardError: String,
        durationMilliseconds: Double? = nil
    ) {
        self.command = command
        self.exitCode = exitCode
        self.standardOutput = standardOutput
        self.standardError = standardError
        self.durationMilliseconds = durationMilliseconds
    }
}

public struct ProcessingProgressEvent: Codable, Equatable, Sendable {
    public var jobID: UUID?
    public var task: String
    public var command: String
    public var stage: String
    public var progress: Double
    public var step: Int?
    public var total: Int?

    public init(
        jobID: UUID? = nil,
        task: String,
        command: String,
        stage: String,
        progress: Double,
        step: Int? = nil,
        total: Int? = nil
    ) {
        self.jobID = jobID
        self.task = task
        self.command = command
        self.stage = stage
        self.progress = progress
        self.step = step
        self.total = total
    }
}

public final class ProcessingProgressScope: @unchecked Sendable {
    private let lock = NSLock()
    private var lowerBound: Double
    private var upperBound: Double

    public init(lowerBound: Double = 0, upperBound: Double = 1) {
        let lower = lowerBound.isFinite ? min(max(lowerBound, 0), 1) : 0
        self.lowerBound = lower
        self.upperBound = upperBound.isFinite ? min(max(upperBound, lower), 1) : 1
    }

    public func update(lowerBound: Double, upperBound: Double) {
        lock.lock()
        let lower = lowerBound.isFinite ? min(max(lowerBound, 0), 1) : self.lowerBound
        self.lowerBound = lower
        self.upperBound = upperBound.isFinite ? min(max(upperBound, lower), 1) : max(self.upperBound, lower)
        lock.unlock()
    }

    public func map(_ progress: Double) -> Double {
        lock.lock()
        let lower = lowerBound
        let upper = upperBound
        lock.unlock()
        let fraction = progress.isFinite ? min(max(progress, 0), 1) : 0
        return lower + fraction * (upper - lower)
    }
}

public enum ProcessingProgressContext {
    @TaskLocal public static var jobID: UUID?
    @TaskLocal public static var progressScope: ProcessingProgressScope?
}

public extension Notification.Name {
    static let photonStackProcessingProgress = Notification.Name("PhotonStackProcessingProgress")
}

public enum ProcessingServiceError: Error, LocalizedError, Equatable {
    case executableNotFound
    case commandFailed(exitCode: Int32, stderr: String)
    case outputMissing(path: String)
    case outputInvalid(path: String)
    case outputCommitFailed(path: String, reason: String)
    case invalidReport(command: String)
    case invalidSelection(command: String)
    case inputColorEncodingMismatch(command: String, inputs: [String])
    case unsupportedPlatform

    public var errorDescription: String? {
        switch self {
        case .executableNotFound:
            return "PhotonStack CLI executable was not found. Build it with tools/scripts/build.sh debug or set PHOTONSTACK_CLI."
        case let .commandFailed(exitCode, stderr):
            return "PhotonStack command failed with exit code \(exitCode): \(stderr)"
        case let .outputMissing(path):
            return "PhotonStack command completed without creating its expected output: \(path)"
        case let .outputInvalid(path):
            return "PhotonStack command created an unreadable image output: \(path)"
        case let .outputCommitFailed(path, reason):
            return "PhotonStack could not atomically replace its output at \(path): \(reason)"
        case let .invalidReport(command):
            return "PhotonStack command returned an invalid or incomplete report: \(command)"
        case let .invalidSelection(command):
            return "The selected candidate no longer exists in the current detection report: \(command)"
        case let .inputColorEncodingMismatch(command, inputs):
            let names = inputs.map { URL(fileURLWithPath: $0).lastPathComponent }.joined(separator: ", ")
            return "PhotonStack \(command) inputs mix sRGB and linear color encodings. Convert them to one working color space first: \(names)"
        case .unsupportedPlatform:
            return "The current ProcessingService implementation is only available on macOS."
        }
    }
}

public struct UnavailableProcessingService: ProcessingService {
    private let error: ProcessingServiceError

    public init(error: ProcessingServiceError = .executableNotFound) {
        self.error = error
    }

    public func inspect(_ asset: PhotonStackAsset) async throws -> ProcessingCommandResult {
        _ = asset
        throw error
    }

    public func makePreview(input: URL, output: URL, width: Int, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        _ = (input, output, width, rawOptions)
        throw error
    }

    public func makeCurvePreview(
        input: URL,
        output: URL,
        width: Int,
        rawOptions: RawProcessingOptions,
        points: [(Double, Double)],
        channel: CurveChannel
    ) async throws -> ProcessingCommandResult {
        _ = (input, output, width, rawOptions, points, channel)
        throw error
    }

    public func adjustRawPreview(input: URL, output: URL, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        _ = (input, output, rawOptions)
        throw error
    }

    public func convert(input: URL, output: URL, options: ExportOptions, rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        _ = (input, output, options, rawOptions)
        throw error
    }

    public func createMaster(output: URL, method: StackMethod, inputs: [URL], rawOptions: RawProcessingOptions) async throws -> ProcessingCommandResult {
        _ = (output, method, inputs, rawOptions)
        throw error
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
        _ = (light, output, dark, bias, flat, darkBiasState, flatBiasState, rawOptions)
        throw error
    }

    public func quality(input: URL) async throws -> ProcessingCommandResult {
        _ = input
        throw error
    }

    public func register(reference: URL, moving: URL, output: URL, alignment: AlignmentMethod) async throws -> ProcessingCommandResult {
        _ = (reference, moving, output, alignment)
        throw error
    }

    public func registerBatch(reference: URL, inputs: [URL], outputDirectory: URL, alignment: AlignmentMethod, outputFormat: String) async throws -> ProcessingCommandResult {
        _ = (reference, inputs, outputDirectory, alignment, outputFormat)
        throw error
    }

    public func stack(inputs: [URL], output: URL, method: StackMethod, alignment: AlignmentMethod) async throws -> ProcessingCommandResult {
        _ = (inputs, output, method, alignment)
        throw error
    }

    public func mosaic(
        inputs: [URL],
        output: URL,
        overlapPixels: Int,
        projection: MosaicProjection,
        layout: MosaicLayoutMode,
        alignment: MosaicAlignmentMode,
        blendMode: MosaicBlendMode,
        exposureMatching: Bool,
        columns: Int,
        previewWidth: Int?
    ) async throws -> ProcessingCommandResult {
        _ = (inputs, output, overlapPixels, projection, layout, alignment, blendMode, exposureMatching, columns, previewWidth)
        throw error
    }

    public func autoStretch(input: URL, output: URL, targetBackground: Double) async throws -> ProcessingCommandResult {
        _ = (input, output, targetBackground)
        throw error
    }

    public func histogram(input: URL, bins: Int) async throws -> ProcessingCommandResult {
        _ = (input, bins)
        throw error
    }

    public func background(
        input: URL,
        output: URL,
        model: String,
        mode: String,
        strength: Double,
        preserveBrightness: Bool,
        protectBrightTargets: Bool
    ) async throws -> ProcessingCommandResult {
        _ = (input, output, model, mode, strength, preserveBrightness, protectBrightTargets)
        throw error
    }

    public func detectClouds(input: URL) async throws -> ProcessingCommandResult {
        _ = input
        throw error
    }

    public func removeClouds(input: URL, output: URL, selectedIndices: [Int]?, strength: Double) async throws -> ProcessingCommandResult {
        _ = (input, output, selectedIndices, strength)
        throw error
    }

    public func normalize(input: URL, output: URL, targetBackground: Double, targetScale: Double) async throws -> ProcessingCommandResult {
        _ = (input, output, targetBackground, targetScale)
        throw error
    }

    public func curves(input: URL, output: URL, points: [(Double, Double)], channel: CurveChannel) async throws -> ProcessingCommandResult {
        _ = (input, output, points, channel)
        throw error
    }

    public func localContrast(input: URL, output: URL, amount: Double, radius: Int) async throws -> ProcessingCommandResult {
        _ = (input, output, amount, radius)
        throw error
    }

    public func denoise(input: URL, output: URL, amount: Double, chromaAmount: Double, radius: Int) async throws -> ProcessingCommandResult {
        _ = (input, output, amount, chromaAmount, radius)
        throw error
    }

    public func sharpen(input: URL, output: URL, amount: Double, radius: Int) async throws -> ProcessingCommandResult {
        _ = (input, output, amount, radius)
        throw error
    }

    public func deconvolve(input: URL, output: URL, iterations: Int, radius: Int, sigma: Double) async throws -> ProcessingCommandResult {
        _ = (input, output, iterations, radius, sigma)
        throw error
    }

    public func colorNeutralize(input: URL, output: URL, strength: Double) async throws -> ProcessingCommandResult {
        _ = (input, output, strength)
        throw error
    }

    public func colorSaturate(input: URL, output: URL, amount: Double) async throws -> ProcessingCommandResult {
        _ = (input, output, amount)
        throw error
    }

    public func removeGreenCast(input: URL, output: URL, amount: Double, backgroundLimit: Double) async throws -> ProcessingCommandResult {
        _ = (input, output, amount, backgroundLimit)
        throw error
    }

    public func detectStars(input: URL, sigmaThreshold: Double, minPeak: Double, maxStars: Int) async throws -> ProcessingCommandResult {
        _ = (input, sigmaThreshold, minPeak, maxStars)
        throw error
    }

    public func createStarMask(input: URL, output: URL, radius: Int, largeRadius: Int, layered: Bool, sigmaThreshold: Double, minPeak: Double, maxStars: Int) async throws -> ProcessingCommandResult {
        _ = (input, output, radius, largeRadius, layered, sigmaThreshold, minPeak, maxStars)
        throw error
    }

    public func reduceStars(input: URL, output: URL, amount: Double, profileAware: Bool, edgeAware: Bool) async throws -> ProcessingCommandResult {
        _ = (input, output, amount, profileAware, edgeAware)
        throw error
    }

    public func reduceComa(input: URL, output: URL, amount: Double, radius: Int, eccentricity: Double, edgeAware: Bool) async throws -> ProcessingCommandResult {
        _ = (input, output, amount, radius, eccentricity, edgeAware)
        throw error
    }

    public func detectArtifacts(input: URL) async throws -> ProcessingCommandResult {
        _ = input
        throw error
    }

    public func removeArtifacts(input: URL, output: URL, selectedIndices: [Int]?, removeAirplanes: Bool, removeDrones: Bool, removeSatellites: Bool, removeMeteors: Bool) async throws -> ProcessingCommandResult {
        _ = (input, output, selectedIndices, removeAirplanes, removeDrones, removeSatellites, removeMeteors)
        throw error
    }

    public func cleanArtifactSequence(inputs: [URL], outputDirectory: URL, outputFormat: String, minWeight: Double, recurrenceThreshold: Int) async throws -> ProcessingCommandResult {
        _ = (inputs, outputDirectory, outputFormat, minWeight, recurrenceThreshold)
        throw error
    }

    public func extractMeteors(input: URL, output: URL) async throws -> ProcessingCommandResult {
        _ = (input, output)
        throw error
    }

    public func restoreMeteors(base: URL, source: URL, output: URL) async throws -> ProcessingCommandResult {
        _ = (base, source, output)
        throw error
    }

    public func drizzle(inputs: [URL], output: URL, scale: Int, pixfrac: Double, alignment: AlignmentMethod) async throws -> ProcessingCommandResult {
        _ = (inputs, output, scale, pixfrac, alignment)
        throw error
    }

    public func runCLI(arguments: [String]) async throws -> ProcessingCommandResult {
        _ = arguments
        throw error
    }

    public func runWorkflow(_ workflow: URL) async throws -> ProcessingCommandResult {
        _ = workflow
        throw error
    }
}
