import Foundation

/// Defaults and limits are supplied by the bundled engine's versioned schema.
public struct DeepSkySchema: Codable, Equatable, Sendable {
    public var version: Int
    public var fields: [Field]
    public struct Field: Codable, Equatable, Sendable, Identifiable {
        public var id: String { key }
        public var key, label, chineseLabel, group, type, defaultValue: String
        public var minimum, maximum: Double
        public var choices: [String]
    }
}

public struct DeepSkySettings: Codable, Equatable, Sendable {
    public var version = 1
    public var values: [String: String] = [:]
    /// Path-keyed overrides remain attached to the correct exposure after reordering.
    public var overrides: [String: String] = [:]
    public var lastReport: DeepSkyReport?
    public init() {}

    private static func quoted(_ value: String) -> String {
        "\"" + value.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
    }

    public static func replacingPath(inRecipe recipe: String, oldPath: String, newPath: String) -> String {
        guard recipe.hasPrefix("PHOTONSTACK_DEEP_SKY_RECIPE 1\n") else { return recipe }
        return recipe.split(separator: "\n", omittingEmptySubsequences: false).map { line in
            if line == "input " + quoted(oldPath) { return "input " + quoted(newPath) }
            if ["calibration.darks ", "calibration.biases ", "calibration.flats "].contains(where: { line.hasPrefix($0) }) {
                let old = String(quoted(quoted(oldPath)).dropFirst().dropLast())
                let new = String(quoted(quoted(newPath)).dropFirst().dropLast())
                return String(line).replacingOccurrences(of: old, with: new)
            }
            return String(line)
        }.joined(separator: "\n")
    }

    public mutating func relink(oldURL: URL, newURL: URL) {
        let new = PhotonStackProject.assetIdentityPath(for: newURL)
        for old in Set([oldURL.path, PhotonStackProject.assetIdentityPath(for: oldURL)]) {
            if let selection = overrides.removeValue(forKey: old) { overrides[new] = selection }
            for key in ["calibration.darks", "calibration.biases", "calibration.flats"] {
                if let list = values[key] { values[key] = list.replacingOccurrences(of: Self.quoted(old), with: Self.quoted(new)) }
            }
        }
        lastReport = nil
    }

    public func recipe(schema: DeepSkySchema, inputs: [URL]) throws -> String {
        guard schema.version == version, version == 1 else { throw DeepSkyRecipeError.unsupportedVersion }
        guard Set(values.keys).isSubset(of: Set(schema.fields.map(\.key))) else { throw DeepSkyRecipeError.unknownSetting }
        func quote(_ text: String) throws -> String {
            guard !text.contains("\n"), !text.contains("\r"), !text.contains("\0") else { throw DeepSkyRecipeError.invalidText }
            return "\"" + text.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
        }
        var lines = ["PHOTONSTACK_DEEP_SKY_RECIPE \(version)"]
        for field in schema.fields {
            let value: String
            if field.key == "selection.overrides" {
                value = inputs.map { overrides[PhotonStackProject.assetIdentityPath(for: $0)] ?? "auto" }.joined(separator: ",")
            } else {
                value = values[field.key] ?? field.defaultValue
            }
            lines.append("\(field.key) \(try quote(value))")
        }
        for input in inputs { lines.append("input \(try quote(PhotonStackProject.assetIdentityPath(for: input)))") }
        return lines.joined(separator: "\n") + "\n"
    }
}

public enum DeepSkyRecipeError: Error, LocalizedError {
    case unsupportedVersion, unknownSetting, invalidText, invalidReport, inputsChanged, masterChanged
    public var errorDescription: String? {
        switch self {
        case .unsupportedVersion: "Unsupported deep-sky recipe version."
        case .unknownSetting: "The recipe contains a setting this engine does not support."
        case .invalidText: "Recipe values cannot contain newlines or NUL characters."
        case .invalidReport: "The engine returned an incomplete deep-sky report."
        case .inputsChanged: "原片已变化，请重新分析并堆栈。Source files changed; analyze and stack again."
        case .masterChanged: "线性母片已变化，不能继续复用；请重新堆栈。Linear master changed; rebuild the stack before reprocessing."
        }
    }
}

public struct DeepSkyInspectedRecipe: Decodable, Sendable {
    public var version: Int
    public var values: [String: String]
    public var inputs: [String]
    public var recipe: String
}

public struct DeepSkyReport: Codable, Equatable, Sendable {
    public var version: Int
    public var success, analysisOnly: Bool
    public var inputCount, selectedCount, referenceIndex: Int
    public var recipe, errorCode, message: String
    public var master, backgroundMaster, developed, rejectionLow, rejectionHigh: String
    public var sensorPatternReport: String? = nil
    public var noiseReference: String? = nil
    public var noiseReferenceManifest: String? = nil
    public var noiseReferenceInputs: String? = nil
    public var independentLuminanceNoise: Bool? = nil
    public var noiseReferencePaths: [String] {
        [noiseReference, noiseReferenceManifest, noiseReferenceInputs].compactMap { $0 }.filter { !$0.isEmpty }
    }
    /// Optional for older projects. Coordinates are in the linear master's canvas.
    public var crop: Crop? = nil
    public var masterSHA256: String? = nil
    public var developedSHA256: String? = nil
    public struct Crop: Codable, Equatable, Sendable {
        public var x, y, width, height: Int
    }
    public var frames: [Frame]
    public struct Frame: Codable, Equatable, Sendable, Identifiable {
        public var id: Int { index }
        public var index: Int
        public var input: String
        public var sha256: String? = nil
        public var sensorPatternModel: Int? = nil
        public var bytes: UInt64
        public var modifiedTicks: Int64
        public var reference, usable, selected, recommendedKeep, temporalAvailable: Bool
        public var sky, noise, noiseRatio, coverage, fwhm, eccentricity: Double
        public var starCount: Int
        public var starsMeasured: Bool
        public var starCountEstimator: String? = nil
        public var starShapeCount: Int? = nil
        public var fwhmEstimator: String? = nil
        public var matches: Int
        public var positiveFraction, negativeFraction, residualRms: Double
        public var rejectedLowSamples, rejectedHighSamples, stackComparedSamples: UInt64
        public var errorCode, message: String
        public var reasons, warnings: [String]
        public var trails: [Trail]?
        public struct Trail: Codable, Equatable, Sendable {
            public var sign, samples: Int
            public var x1, y1, x2, y2: Double
        }
    }
    public func validate() throws {
        guard version == 1, inputCount >= 0, inputCount <= 2000, inputCount == frames.count,
              Set(frames.map(\.index)) == Set(0..<inputCount),
              Set(frames.map(\.input)).count == inputCount,
              selectedCount == frames.filter(\.selected).count,
              frames.allSatisfy({ !$0.selected || $0.usable }) else { throw DeepSkyRecipeError.invalidReport }
        if !noiseReferencePaths.isEmpty || independentLuminanceNoise == true {
            guard !analysisOnly, noiseReferencePaths.count == 3,
                  frames.allSatisfy({ !$0.selected || $0.sha256?.count == 64 }) else {
                throw DeepSkyRecipeError.invalidReport
            }
        }
        if let sensorPatternReport, !sensorPatternReport.isEmpty {
            guard !analysisOnly, frames.allSatisfy({ frame in
                !frame.selected || ((0...2).contains(frame.sensorPatternModel ?? -1) && frame.sha256?.count == 64)
            }) else { throw DeepSkyRecipeError.invalidReport }
        }
    }
}
