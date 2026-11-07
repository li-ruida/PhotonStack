import Foundation
import PhotonStackAppCore

/// Header-only planning snapshot. Estimates are not process memory limits,
/// successful pixel decoding, calibration compatibility or a disk reservation.
public struct DeepSkyResourcePlan: Decodable, Sendable {
    public struct Issue: Decodable, Sendable {
        public let code: String
        public let path: String
        public let message: String
    }
    public let version: Int
    public let lightCount: Int
    public let calibrationCount: Int
    public let inspectedCount: Int
    public let width: UInt32
    public let height: UInt32
    public let sourceBytes: UInt64
    public let destination: String
    public let additionalSensorStorage: Bool
    public let temporaryBytes: UInt64?
    public let productBytes: UInt64?
    public let plannedDiskBytes: UInt64?
    public let stackBufferBytes: UInt64?
    public let assessmentSampleBytes: UInt64?
    public let availableBytes: UInt64?
    public let issues: [Issue]

    public var diskSpaceNeedsAttention: Bool {
        guard let plannedDiskBytes, let availableBytes else { return false }
        return plannedDiskBytes > availableBytes
    }

    public func validate() throws {
        guard version == 1, lightCount >= 0, calibrationCount >= 0, inspectedCount >= 0,
              lightCount <= Int.max - calibrationCount,
              inspectedCount <= lightCount + calibrationCount, destination.hasPrefix("/"),
              !additionalSensorStorage || plannedDiskBytes == nil,
              plannedDiskBytes == nil || (temporaryBytes != nil && productBytes != nil && width > 0 && height > 0)
        else { throw DeepSkyRecipeError.invalidReport }
    }
}

public extension ProcessingService {
    func preflightDeepSky(recipeText: String, destination: URL) async throws -> DeepSkyResourcePlan {
        try Task.checkCancellation()
        let scratch = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackPreflight-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: scratch, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: scratch) }
        let recipe = scratch.appendingPathComponent("recipe.txt")
        try recipeText.write(to: recipe, atomically: true, encoding: .utf8)
        let result = try await runCLI(arguments: ["deep-sky", "--preflight", "--recipe", recipe.path, "--output-dir", destination.path])
        try Task.checkCancellation()
        guard let line = result.standardOutput.split(separator: "\n").last else { throw DeepSkyRecipeError.invalidReport }
        let plan = try JSONDecoder().decode(DeepSkyResourcePlan.self, from: Data(line.utf8))
        try plan.validate()
        guard URL(fileURLWithPath: plan.destination).standardizedFileURL.path == destination.standardizedFileURL.path else {
            throw DeepSkyRecipeError.invalidReport
        }
        return plan
    }
}
