import Foundation
import PhotonStackAppCore
import Testing
@testable import PhotonStackProcessing

private func resourceReport(destination: String) -> [String: Any] {
    ["version": 1, "lightCount": 3, "calibrationCount": 0, "inspectedCount": 3,
     "width": 256, "height": 128, "sourceBytes": 1000, "destination": destination,
     "additionalSensorStorage": false, "temporaryBytes": 3000, "productBytes": 4000,
     "plannedDiskBytes": 8000, "stackBufferBytes": 2000, "assessmentSampleBytes": 1000,
     "availableBytes": 7000, "issues": []]
}

@Test func resourcePlanDistinguishesLowSpaceFromUnknown() throws {
    var fields = resourceReport(destination: "/tmp/work")
    var plan = try JSONDecoder().decode(DeepSkyResourcePlan.self, from: JSONSerialization.data(withJSONObject: fields))
    try plan.validate()
    #expect(plan.diskSpaceNeedsAttention)
    fields.removeValue(forKey: "availableBytes")
    plan = try JSONDecoder().decode(DeepSkyResourcePlan.self, from: JSONSerialization.data(withJSONObject: fields))
    try plan.validate()
    #expect(!plan.diskSpaceNeedsAttention && plan.availableBytes == nil)
}

@Test(arguments: ["version", "inspectedCount", "additionalSensorStorage", "width"])
func resourcePlanRejectsInconsistentReports(field: String) throws {
    var fields = resourceReport(destination: "/tmp/work")
    switch field {
    case "version": fields[field] = 2
    case "inspectedCount": fields[field] = 4
    case "additionalSensorStorage": fields[field] = true
    default: fields[field] = 0
    }
    let plan = try JSONDecoder().decode(DeepSkyResourcePlan.self, from: JSONSerialization.data(withJSONObject: fields))
    #expect(throws: DeepSkyRecipeError.self) { try plan.validate() }
}

@Test func preflightServiceRemovesScratchAndLeavesDestinationUntouched() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackPreflightTests-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let destination = root.appendingPathComponent("uncreated", isDirectory: true)
    let fields = resourceReport(destination: destination.path)
    try JSONSerialization.data(withJSONObject: fields).write(to: root.appendingPathComponent("response.json"))
    let executable = root.appendingPathComponent("fake-engine")
    let script = """
    #!/bin/sh
    while [ "$#" -gt 0 ]; do
      if [ "$1" = "--recipe" ]; then printf '%s' "$2" > "$PWD/recipe-path"; fi
      shift
    done
    cat "$PWD/response.json"
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    let plan = try await CLIProcessingService(executableURL: executable).preflightDeepSky(recipeText: "PHOTONSTACK_DEEP_SKY_RECIPE 1\n", destination: destination)
    #expect(plan.inspectedCount == 3)
    #expect(!FileManager.default.fileExists(atPath: destination.path))
    let recipePath = try String(contentsOf: root.appendingPathComponent("recipe-path"), encoding: .utf8)
    #expect(!FileManager.default.fileExists(atPath: URL(fileURLWithPath: recipePath).deletingLastPathComponent().path))
}
