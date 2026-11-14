import Foundation
import PhotonStackAppCore
import PhotonStackProcessing

struct ParsedRegistrationMetrics {
    var mode: AlignmentMethod
    var dx: Double
    var dy: Double
    var scale: Double
    var rotationRadians: Double
    var matches: Int
    var usedFallback: Bool
}

/// Pure validation of engine reports, independent of workspace selection, tasks,
/// caches and UI mutation. Keep wire-format handling at this single boundary.
enum WorkspaceReportDecoder {
    static func parseHistogram(
        _ result: ProcessingCommandResult,
        expectedBins: Int
    ) throws -> HistogramSnapshot {
        guard let object = try parseJSONObject(from: result.standardOutput),
              object["type"] as? String == "complete",
              object["command"] as? String == "histogram",
              expectedBins > 0,
              let rawBins = object["bins"] as? [NSNumber],
              rawBins.count == expectedBins,
              rawBins.allSatisfy({
                  let value = $0.doubleValue
                  return CFGetTypeID($0) != CFBooleanGetTypeID()
                      && value.isFinite
                      && value >= 0
              }),
              let minimum = jsonFiniteDouble(object["minimum"]),
              let maximum = jsonFiniteDouble(object["maximum"]),
              let mean = jsonFiniteDouble(object["mean"])
        else {
            throw ProcessingServiceError.invalidReport(command: "histogram")
        }

        guard maximum >= minimum,
              mean >= minimum,
              mean <= maximum
        else {
            throw ProcessingServiceError.invalidReport(command: "histogram")
        }

        return HistogramSnapshot(
            bins: rawBins.map(\.doubleValue),
            minimum: minimum,
            maximum: maximum,
            mean: mean
        )
    }

    static func parseRawDecodeStatus(_ result: ProcessingCommandResult) throws -> RawDecodeStatus? {
        guard let object = try parseJSONObject(from: result.standardOutput) else {
            return nil
        }
        let decodeKeys = [
            "decodeBackend",
            "decodeFallback",
            "decodeFallbackCode",
            "decodeFallbackMessage",
        ]
        guard decodeKeys.contains(where: { object[$0] != nil }) else {
            return nil
        }
        guard object["type"] as? String == "complete",
              object["command"] as? String == "preview",
              let backendName = object["decodeBackend"] as? String,
              let backend = RawDecodeBackend(rawValue: backendName),
              let usedFallback = jsonBoolean(object["decodeFallback"]),
              let fallbackErrorCode = object["decodeFallbackCode"] as? String,
              let fallbackMessage = object["decodeFallbackMessage"] as? String,
              (usedFallback && backend == .imageIO && fallbackErrorCode.isEmpty == false &&
                  fallbackMessage.isEmpty == false) ||
                  (usedFallback == false && backend == .appleRaw && fallbackErrorCode.isEmpty &&
                      fallbackMessage.isEmpty)
        else {
            throw ProcessingServiceError.invalidReport(command: "preview")
        }
        return RawDecodeStatus(
            backend: backend,
            usedFallback: usedFallback,
            fallbackErrorCode: fallbackErrorCode,
            fallbackMessage: fallbackMessage
        )
    }

    static func parseInspectMetadata(_ result: ProcessingCommandResult) throws -> AssetMetadata {
        guard let object = try parseJSONObject(from: result.standardOutput),
              let width = jsonInteger(object["width"]),
              let height = jsonInteger(object["height"]),
              let channels = jsonInteger(object["channels"]),
              let bitsPerChannel = jsonInteger(object["bitsPerChannel"]),
              let format = object["format"] as? String,
              width > 0,
              height > 0,
              channels > 0,
              bitsPerChannel > 0,
              format.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false
        else {
            throw ProcessingServiceError.invalidReport(command: "inspect")
        }

        let exposureTimeSeconds = try positiveMetadataDouble("exposureTimeSeconds", in: object)
        let fNumber = try positiveMetadataDouble("fNumber", in: object)
        let focalLengthMM = try positiveMetadataDouble("focalLengthMM", in: object)
        let iso = try positiveMetadataInteger("iso", in: object)
        let orientation = try metadataInteger("orientation", in: object, range: 1...8)

        return AssetMetadata(
            width: width,
            height: height,
            channels: channels,
            bitsPerChannel: bitsPerChannel,
            formatDescription: format,
            cameraMake: metadataString("cameraMake", in: object),
            cameraModel: metadataString("cameraModel", in: object),
            lensModel: metadataString("lensModel", in: object),
            captureDate: metadataString("captureDate", in: object),
            colorModel: metadataString("colorModel", in: object),
            colorProfile: metadataString("colorProfile", in: object),
            rawDecoder: metadataString("rawDecoder", in: object),
            whiteBalance: metadataString("whiteBalance", in: object),
            exposureBias: metadataString("exposureBias", in: object),
            exposureTimeSeconds: exposureTimeSeconds,
            fNumber: fNumber,
            focalLengthMM: focalLengthMM,
            iso: iso,
            orientation: orientation
        )
    }

    static func metadataObject(in object: [String: Any]) -> [String: Any] {
        object["metadata"] as? [String: Any] ?? object
    }

    static func metadataString(_ key: String, in object: [String: Any]) -> String? {
        let metadata = metadataObject(in: object)
        let value = (metadata[key] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines)
        return value?.isEmpty == false ? value : nil
    }

    static func positiveMetadataDouble(_ key: String, in object: [String: Any]) throws -> Double? {
        let metadata = metadataObject(in: object)
        guard let rawValue = metadata[key] else {
            return nil
        }
        guard let value = jsonFiniteDouble(rawValue), value > 0 else {
            throw ProcessingServiceError.invalidReport(command: "inspect")
        }
        return value
    }

    static func positiveMetadataInteger(_ key: String, in object: [String: Any]) throws -> Int? {
        try metadataInteger(key, in: object, range: 1...Int.max)
    }

    static func metadataInteger(
        _ key: String,
        in object: [String: Any],
        range: ClosedRange<Int>
    ) throws -> Int? {
        let metadata = metadataObject(in: object)
        guard let rawValue = metadata[key] else {
            return nil
        }
        guard let value = jsonInteger(rawValue), range.contains(value) else {
            throw ProcessingServiceError.invalidReport(command: "inspect")
        }
        return value
    }

    static func parseMosaicQualityReport(
        _ result: ProcessingCommandResult,
        expectedPanelCount: Int
    ) throws -> MosaicQualityReport {
        guard let object = try parseJSONObject(from: result.standardOutput),
              object["type"] as? String == "complete",
              object["command"] as? String == "mosaic",
              let autoAligned = jsonBoolean(object["autoAligned"]),
              let matches = jsonInteger(object["matches"]),
              let fallbackPanels = jsonInteger(object["fallbackPanels"]),
              let exposureMatched = jsonBoolean(object["exposureMatched"]),
              let blend = object["blend"] as? String,
              blend.isEmpty == false,
              let rawPlacements = object["placements"] as? [[String: Any]],
              rawPlacements.count == expectedPanelCount,
              matches >= 0,
              fallbackPanels >= 0,
              fallbackPanels <= expectedPanelCount
        else {
            throw ProcessingServiceError.invalidReport(command: "mosaic")
        }

        let placements = try rawPlacements.map { placement in
            guard let index = jsonInteger(placement["index"]),
                  let x = jsonFiniteDouble(placement["x"]),
                  let y = jsonFiniteDouble(placement["y"]),
                  let a = jsonFiniteDouble(placement["a"]),
                  let b = jsonFiniteDouble(placement["b"]),
                  let c = jsonFiniteDouble(placement["c"]),
                  let d = jsonFiniteDouble(placement["d"]),
                  let dx = jsonFiniteDouble(placement["dx"]),
                  let dy = jsonFiniteDouble(placement["dy"]),
                  let model = placement["model"] as? String,
                  model.isEmpty == false,
                  let placementMatches = jsonInteger(placement["matches"]),
                  let references = jsonInteger(placement["references"]),
                  let placementAutoAligned = jsonBoolean(placement["autoAligned"]),
                  let fallback = jsonBoolean(placement["fallback"]),
                  let reducedModel = jsonBoolean(placement["reducedModel"]),
                  let coarseAlignment = jsonBoolean(placement["coarseAlignment"]),
                  index >= 0,
                  index < expectedPanelCount,
                  placementMatches >= 0,
                  references >= 0
            else {
                throw ProcessingServiceError.invalidReport(command: "mosaic")
            }
            return MosaicPlacementReport(
                index: index,
                x: x,
                y: y,
                a: a,
                b: b,
                c: c,
                d: d,
                dx: dx,
                dy: dy,
                model: model,
                matches: placementMatches,
                references: references,
                autoAligned: placementAutoAligned,
                fallback: fallback,
                reducedModel: reducedModel,
                coarseAlignment: coarseAlignment,
                locallyRefined: (placement["residuals"] as? [[Double]])?.isEmpty == false
            )
        }
        guard Set(placements.map(\.index)).count == expectedPanelCount else {
            throw ProcessingServiceError.invalidReport(command: "mosaic")
        }

        return MosaicQualityReport(
            autoAligned: autoAligned,
            matches: matches,
            fallbackPanels: fallbackPanels,
            exposureMatched: exposureMatched,
            blend: blend,
            placements: placements
        )
    }

    static func parseJSONObject(from output: String) throws -> [String: Any]? {
        var objects: [[String: Any]] = []
        var searchIndex = output.startIndex

        while searchIndex < output.endIndex,
              let startIndex = output[searchIndex...].firstIndex(of: "{") {
            var depth = 0
            var inString = false
            var isEscaping = false
            var endIndex: String.Index?

            for index in output.indices[startIndex...] {
                let character = output[index]
                if inString {
                    if isEscaping {
                        isEscaping = false
                    } else if character == "\\" {
                        isEscaping = true
                    } else if character == "\"" {
                        inString = false
                    }
                    continue
                }

                if character == "\"" {
                    inString = true
                } else if character == "{" {
                    depth += 1
                } else if character == "}" {
                    depth -= 1
                    if depth == 0 {
                        endIndex = index
                        break
                    }
                }
            }

            if let endIndex, depth == 0, inString == false {
                let jsonText = String(output[startIndex...endIndex])
                if let data = jsonText.data(using: .utf8),
                   let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                    objects.append(object)
                }
                searchIndex = output.index(after: endIndex)
            } else {
                searchIndex = output.index(after: startIndex)
            }
        }

        var selectedObject: [String: Any]?
        var selectedRank = -1
        for object in objects {
            let isComplete = object["type"] as? String == "complete"
            let isLegacyInspect = object["width"] != nil && object["height"] != nil &&
                object["channels"] != nil && object["bitsPerChannel"] != nil && object["format"] != nil
            let rank = isComplete ? 2 : (isLegacyInspect ? 1 : 0)
            if rank >= selectedRank {
                selectedObject = object
                selectedRank = rank
            }
        }
        return selectedObject
    }

    static func parseRegistrationMetrics(
        _ result: ProcessingCommandResult,
        mode: AlignmentMethod,
        output: URL
    ) throws -> ParsedRegistrationMetrics {
        guard let object = try parseJSONObject(from: result.standardOutput),
              object["type"] as? String == "complete",
              object["command"] as? String == "register",
              let reportedMode = object["mode"] as? String,
              let parsedMode = AlignmentMethod(rawValue: reportedMode),
              let dx = jsonFiniteDouble(object["dx"]),
              let dy = jsonFiniteDouble(object["dy"]),
              let scale = jsonFiniteDouble(object["scale"]),
              let rotationRadians = jsonFiniteDouble(object["rotationRadians"]),
              let matches = jsonInteger(object["matches"]),
              let detectedReferenceStars = jsonInteger(object["detectedReferenceStars"]),
              let detectedMovingStars = jsonInteger(object["detectedMovingStars"]),
              let inlierRatio = jsonFiniteDouble(object["inlierRatio"]),
              let usedFallback = jsonBoolean(object["usedFallback"]),
              let reportedOutput = object["output"] as? String,
              parsedMode == mode,
              scale > 0,
              matches >= 0,
              detectedReferenceStars >= 0,
              detectedMovingStars >= 0,
              inlierRatio.isFinite,
              (0...1).contains(inlierRatio),
              reportedOutput.isEmpty == false,
              URL(fileURLWithPath: reportedOutput).standardizedFileURL == output.standardizedFileURL
        else {
            throw ProcessingServiceError.invalidReport(command: "register")
        }

        return ParsedRegistrationMetrics(
            mode: parsedMode,
            dx: dx,
            dy: dy,
            scale: scale,
            rotationRadians: rotationRadians,
            matches: matches,
            usedFallback: usedFallback
        )
    }

    static func jsonFiniteDouble(_ value: Any?) -> Double? {
        guard let number = value as? NSNumber,
              CFGetTypeID(number) != CFBooleanGetTypeID()
        else {
            return nil
        }
        let result = number.doubleValue
        return result.isFinite ? result : nil
    }

    static func jsonInteger(_ value: Any?) -> Int? {
        guard let number = jsonFiniteDouble(value),
              number.rounded() == number,
              number >= Double(Int.min),
              number < Double(Int.max)
        else {
            return nil
        }
        return Int(number)
    }

    static func jsonBoolean(_ value: Any?) -> Bool? {
        guard let number = value as? NSNumber,
              CFGetTypeID(number) == CFBooleanGetTypeID()
        else {
            return nil
        }
        return number.boolValue
    }
}
