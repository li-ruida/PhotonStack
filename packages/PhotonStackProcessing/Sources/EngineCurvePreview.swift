#if os(macOS)
import Foundation
import CoreGraphics
import PhotonStackAppCore
import PhotonStackEngineBridge

public enum EngineCurvePreview {
    fileprivate static func makeRequest(width: Int, rawOptions: RawProcessingOptions, points: [(Double, Double)],
                                        channel: CurveChannel, retainDecodedSource: Bool) throws -> PhotonStackCurvePreviewRequest {
        let numbers = [rawOptions.manualWhiteBalanceTemperature, rawOptions.manualWhiteBalanceTint,
                       rawOptions.exposureBias, rawOptions.manualBlackLevel] + points.flatMap { [$0.0, $0.1] }
        guard width > 0, width <= Int(UInt32.max), numbers.allSatisfy({ $0.isFinite && Float($0).isFinite }) else {
            throw ProcessingServiceError.invalidArguments("Curve preview dimensions and numeric controls must be finite and valid.")
        }
        try Task.checkCancellation()
        var request = photonstack_default_curve_preview_request()
        request.width = UInt32(max(width, 1))
        request.rawWhiteBalanceMode = bridgeRawWhiteBalanceMode(rawOptions.whiteBalanceMode)
        request.rawManualWhiteBalanceTemperature = Float(rawOptions.manualWhiteBalanceTemperature)
        request.rawManualWhiteBalanceTint = Float(rawOptions.manualWhiteBalanceTint)
        request.rawExposureBias = Float(rawOptions.exposureBias)
        request.rawBlackLevelMode = bridgeRawBlackLevelMode(rawOptions.blackLevelMode)
        request.rawManualBlackLevel = Float(rawOptions.manualBlackLevel)
        request.rawDemosaicQuality = bridgeRawDemosaicQuality(rawOptions.demosaicQuality)
        request.rawLinearOutput = rawOptions.linearOutput ? 1 : 0
        request.retainDecodedSource = retainDecodedSource ? 1 : 0
        request.curveChannel = bridgeCurveChannel(channel)

        return request
    }

    public static func interactiveImage(using service: any ProcessingService, input: URL, width: Int,
                                        rawOptions: RawProcessingOptions, points: [(Double, Double)],
                                        channel: CurveChannel) async throws -> CGImage? {
        guard service is CLIProcessingService else { return nil }
        return try await EngineCurvePreviewWorker.shared.image(input: input, width: width, rawOptions: rawOptions,
                                                               points: points, channel: channel)
    }

    public static func clearCachedSourceAsync() async {
        await EngineCurvePreviewWorker.shared.clearCache()
    }

    public static func runIfAvailableAsync(
        using service: any ProcessingService,
        input: URL,
        output: URL,
        width: Int,
        rawOptions: RawProcessingOptions,
        points: [(Double, Double)],
        channel: CurveChannel,
        retainDecodedSource: Bool = false
    ) async throws -> ProcessingCommandResult? {
        try await EngineCurvePreviewWorker.shared.run(
            using: service,
            input: input,
            output: output,
            width: width,
            rawOptions: rawOptions,
            points: points,
            channel: channel,
            retainDecodedSource: retainDecodedSource
        )
    }

    public static func runIfAvailable(
        using service: any ProcessingService,
        input: URL,
        output: URL,
        width: Int,
        rawOptions: RawProcessingOptions,
        points: [(Double, Double)],
        channel: CurveChannel,
        retainDecodedSource: Bool = false
    ) throws -> ProcessingCommandResult? {
        guard service is CLIProcessingService else {
            return nil
        }

        var request = try makeRequest(width: width, rawOptions: rawOptions, points: points, channel: channel,
                                      retainDecodedSource: retainDecodedSource)
        let transaction = AtomicOutputTransaction(arguments: ["preview", "--output", output.path],
                                                  workingDirectory: output.deletingLastPathComponent())!
        defer { transaction.cleanUp() }
        let start = Date()
        let fallbackCodeBufferSize = 256
        let fallbackMessageBufferSize = 1024
        let errorBufferSize = 1024
        var fallbackCode = Array(repeating: CChar(0), count: fallbackCodeBufferSize)
        var fallbackMessage = Array(repeating: CChar(0), count: fallbackMessageBufferSize)
        var errorBuffer = Array(repeating: CChar(0), count: errorBufferSize)

        let bridgeResult = try input.path.withCString { inputCString in
            try transaction.temporaryURL.path.withCString { outputCString in
                try points.withUnsafeBufferPointer { pointsBuffer in
                    let bridgePoints = pointsBuffer.map { point in
                        PhotonStackCurvePoint(input: Float(point.0), output: Float(point.1))
                    }
                    return try bridgePoints.withUnsafeBufferPointer { bridgePointsBuffer in
                        if bridgePointsBuffer.isEmpty == false {
                            request.curvePoints = bridgePointsBuffer.baseAddress
                            request.curvePointCount = bridgePointsBuffer.count
                        }
                        let result = photonstack_curve_preview_path(
                            inputCString,
                            outputCString,
                            request,
                            &fallbackCode,
                            fallbackCodeBufferSize,
                            &fallbackMessage,
                            fallbackMessageBufferSize,
                            &errorBuffer,
                            errorBufferSize
                        )
                        if result.ok == 0 {
                            let message = decodeCString(errorBuffer)
                            throw ProcessingServiceError.commandFailed(
                                exitCode: 1,
                                stderr: message.isEmpty ? "curve preview bridge failed" : message
                            )
                        }
                        return result
                    }
                }
            }
        }

        try Task.checkCancellation()
        try transaction.commit()

        let fallbackCodeText = decodeCString(fallbackCode)
        let fallbackMessageText = decodeCString(fallbackMessage)
        let duration = Date().timeIntervalSince(start) * 1000.0
        let standardOutput = """
        {
          "type": "complete",
          "command": "preview",
          "width": \(bridgeResult.width),
          "height": \(bridgeResult.height),
          "decodedWidth": \(bridgeResult.decodedWidth),
          "decodedHeight": \(bridgeResult.decodedHeight),
          "decodeBackend": "\(decodeBackendName(bridgeResult.backend))",
          "decodeFallback": \(bridgeResult.usedFallback != 0 ? "true" : "false"),
          "decodeFallbackCode": "\(jsonEscape(fallbackCodeText))",
          "decodeFallbackMessage": "\(jsonEscape(fallbackMessageText))",
          "rawWhiteBalance": "\(rawWhiteBalanceName(rawOptions.whiteBalanceMode))",
          "rawTemperature": \(rawOptions.manualWhiteBalanceTemperature),
          "rawTint": \(rawOptions.manualWhiteBalanceTint),
          "rawExposureBias": \(rawOptions.exposureBias),
          "rawBlackLevel": "\(rawBlackLevelName(rawOptions.blackLevelMode))",
          "rawBlackValue": \(rawOptions.manualBlackLevel),
          "rawDemosaic": "\(rawDemosaicName(rawOptions.demosaicQuality))",
          "rawLinear": \(rawOptions.linearOutput ? "true" : "false"),
          "curveApplied": \(!points.isEmpty),
          "curveChannel": "\(curveChannelName(channel))",
          "output": "\(jsonEscape(output.path))"
        }
        """

        return ProcessingCommandResult(
            command: ["preview"],
            exitCode: 0,
            standardOutput: standardOutput,
            standardError: "",
            durationMilliseconds: duration
        )
    }

    private static func bridgeRawWhiteBalanceMode(_ mode: RawWhiteBalanceMode) -> PhotonStackRawWhiteBalanceMode {
        switch mode {
        case .camera:
            return PHOTONSTACK_RAW_WHITE_BALANCE_CAMERA
        case .auto:
            return PHOTONSTACK_RAW_WHITE_BALANCE_AUTO
        case .daylight:
            return PHOTONSTACK_RAW_WHITE_BALANCE_DAYLIGHT
        case .manual:
            return PHOTONSTACK_RAW_WHITE_BALANCE_MANUAL
        }
    }

    private static func bridgeRawBlackLevelMode(_ mode: RawBlackLevelMode) -> PhotonStackRawBlackLevelMode {
        switch mode {
        case .camera:
            return PHOTONSTACK_RAW_BLACK_LEVEL_CAMERA
        case .auto:
            return PHOTONSTACK_RAW_BLACK_LEVEL_AUTO
        case .manual:
            return PHOTONSTACK_RAW_BLACK_LEVEL_MANUAL
        }
    }

    private static func bridgeRawDemosaicQuality(_ quality: RawDemosaicQuality) -> PhotonStackRawDemosaicQuality {
        switch quality {
        case .fast:
            return PHOTONSTACK_RAW_DEMOSAIC_FAST
        case .balanced:
            return PHOTONSTACK_RAW_DEMOSAIC_BALANCED
        case .high:
            return PHOTONSTACK_RAW_DEMOSAIC_HIGH
        }
    }

    private static func bridgeCurveChannel(_ channel: CurveChannel) -> PhotonStackCurveChannel {
        switch channel {
        case .rgb:
            return PHOTONSTACK_CURVE_CHANNEL_RGB
        case .red:
            return PHOTONSTACK_CURVE_CHANNEL_RED
        case .green:
            return PHOTONSTACK_CURVE_CHANNEL_GREEN
        case .blue:
            return PHOTONSTACK_CURVE_CHANNEL_BLUE
        case .luminance:
            return PHOTONSTACK_CURVE_CHANNEL_LUMINANCE
        }
    }

    private static func decodeBackendName(_ backend: PhotonStackImageReadBackend) -> String {
        switch backend {
        case PHOTONSTACK_IMAGE_READ_BACKEND_IMAGEIO:
            return "imageio"
        case PHOTONSTACK_IMAGE_READ_BACKEND_APPLE_RAW:
            return "apple-raw"
        case PHOTONSTACK_IMAGE_READ_BACKEND_FITS:
            return "fits"
        case PHOTONSTACK_IMAGE_READ_BACKEND_UNKNOWN:
            fallthrough
        default:
            return "unknown"
        }
    }

    private static func rawWhiteBalanceName(_ mode: RawWhiteBalanceMode) -> String {
        switch mode {
        case .camera:
            return "camera"
        case .auto:
            return "auto"
        case .daylight:
            return "daylight"
        case .manual:
            return "manual"
        }
    }

    private static func rawBlackLevelName(_ mode: RawBlackLevelMode) -> String {
        switch mode {
        case .camera:
            return "camera"
        case .auto:
            return "auto"
        case .manual:
            return "manual"
        }
    }

    private static func rawDemosaicName(_ quality: RawDemosaicQuality) -> String {
        switch quality {
        case .fast:
            return "fast"
        case .balanced:
            return "balanced"
        case .high:
            return "high"
        }
    }

    private static func curveChannelName(_ channel: CurveChannel) -> String {
        switch channel {
        case .rgb:
            return "rgb"
        case .red:
            return "red"
        case .green:
            return "green"
        case .blue:
            return "blue"
        case .luminance:
            return "luminance"
        }
    }

    private static func jsonEscape(_ value: String) -> String {
        var result = ""
        result.reserveCapacity(value.count)
        for scalar in value.unicodeScalars {
            switch scalar.value {
            case 0x08:
                result += "\\b"
            case 0x09:
                result += "\\t"
            case 0x0A:
                result += "\\n"
            case 0x0C:
                result += "\\f"
            case 0x0D:
                result += "\\r"
            case 0x22:
                result += "\\\""
            case 0x5C:
                result += "\\\\"
            case 0x00...0x1F:
                result += String(format: "\\u%04X", scalar.value)
            default:
                result.unicodeScalars.append(scalar)
            }
        }
        return result
    }

    private static func decodeCString(_ buffer: [CChar]) -> String {
        let bytes = buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }
        return String(decoding: bytes, as: UTF8.self)
    }
}

private actor EngineCurvePreviewWorker {
    static let shared = EngineCurvePreviewWorker()

    func image(input: URL, width: Int, rawOptions: RawProcessingOptions, points: [(Double, Double)],
               channel: CurveChannel) throws -> CGImage {
        var request = try EngineCurvePreview.makeRequest(width: width, rawOptions: rawOptions, points: points,
                                                         channel: channel, retainDecodedSource: true)
        let controls = points.map { PhotonStackCurvePoint(input: Float($0.0), output: Float($0.1)) }
        var pixels = PhotonStackPreviewPixels()
        var message = [CChar](repeating: 0, count: 1024)
        let result = controls.withUnsafeBufferPointer { buffer in
            request.curvePoints = buffer.baseAddress
            request.curvePointCount = buffer.count
            return photonstack_curve_preview_pixels(input.path, request, &pixels, &message, 1024)
        }
        guard result.ok != 0, let storage = pixels.storage, let data = pixels.data else {
            throw ProcessingServiceError.commandFailed(exitCode: 1, stderr: String(cString: message))
        }
        let w = Int(result.width), h = Int(result.height)
        guard let provider = CGDataProvider(dataInfo: storage, data: data, size: w * h * 4,
                                            releaseData: { info, _, _ in photonstack_free_preview_pixels(info) }) else {
            photonstack_free_preview_pixels(storage)
            throw ProcessingServiceError.invalidArguments("Could not create curve preview provider.")
        }
        try Task.checkCancellation()
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let image = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                                  space: space, bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue)
                                    .union(.byteOrder32Big), provider: provider, decode: nil,
                                  shouldInterpolate: false, intent: .defaultIntent) else {
            throw ProcessingServiceError.invalidArguments("Could not create curve preview image.")
        }
        return image
    }

    func clearCache() {
        photonstack_clear_curve_preview_cache()
    }

    func run(
        using service: any ProcessingService,
        input: URL,
        output: URL,
        width: Int,
        rawOptions: RawProcessingOptions,
        points: [(Double, Double)],
        channel: CurveChannel,
        retainDecodedSource: Bool
    ) throws -> ProcessingCommandResult? {
        try Task.checkCancellation()
        return try EngineCurvePreview.runIfAvailable(
            using: service,
            input: input,
            output: output,
            width: width,
            rawOptions: rawOptions,
            points: points,
            channel: channel,
            retainDecodedSource: retainDecodedSource
        )
    }
}
#endif
