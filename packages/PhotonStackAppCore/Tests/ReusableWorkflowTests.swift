import Foundation
import Testing
@testable import PhotonStackAppCore

@Test func reusableWorkflowStripsSourceIdentityButRetainsProcessingControls() throws {
    let op = EditOperation(kind: .curves, parameters: ["input": "/old/source.fits", "output": "/old/result.tiff",
        "artifactID": UUID().uuidString, "channel": "rgb", "points": "0,0;1,1", "midOutput": "0.7"])
    let step = try WorkflowStep.capture(op)
    #expect(step.parameters == ["channel": "rgb", "points": "0,0;1,1", "midOutput": "0.7"])
    let workflow = ReusableWorkflow(name: "星轨", steps: [step])
    try workflow.validate()
    #expect(!workflow.combinesFrames)
    #expect(!String(decoding: try JSONEncoder().encode(workflow), as: UTF8.self).contains("/old/"))
}

@Test func reusableWorkflowManualSelectionsNeedExplicitRedetection() throws {
    let op = EditOperation(kind: .artifactRemove, parameters: ["selectedIndices": "1,2", "removeMeteors": "false"])
    #expect(throws: WorkflowError.self) { try WorkflowStep.capture(op) }
    let step = try WorkflowStep.capture(op, redetectSelections: true)
    #expect(step.parameters == ["removeMeteors": "false"])
    #expect(throws: WorkflowError.self) { try WorkflowStep.capture(EditOperation(kind: .meteorRestore)) }
}

@Test func reusableWorkflowDeepSkyRebindsCalibrationAndSelection() throws {
    let captured = try WorkflowStep.deepSky(values: ["calibration.darks": "\"/old/dark.fit\"", "stack.method": "maximum",
        "selection.overrides": "reject,keep", "registration.reference": "4", "stack.weights": "0.2,0.8"])
    #expect(captured.step.calibration == [.darks])
    #expect(captured.step.deepSkyValues == ["stack.method": "maximum"])
    #expect(!captured.notices.isEmpty)
    #expect(throws: WorkflowError.self) { try WorkflowStep.deepSky(values: ["background.exclusions": "20,30,4,5,0"]) }
}

@Test func reusableWorkflowRejectsStructuralInjectionAndUnknownVersion() throws {
    var workflow = ReusableWorkflow(name: "test", steps: [.init(kind: .stretch, parameters: ["input": "/secret/file"])])
    #expect(throws: WorkflowError.self) { try workflow.validate() }
    workflow.steps = [.init(kind: .stretch, parameters: ["targetBackground": "/secret/file"])]
    #expect(throws: WorkflowError.self) { try workflow.validate() }
    workflow.steps = [.init(kind: .stretch), .init(kind: .stack)]
    #expect(throws: WorkflowError.self) { try workflow.validate() }
    workflow.steps = [.init(kind: .stretch)]
    workflow.version = 2
    #expect(throws: WorkflowError.self) { try workflow.validate() }
}

@Test func reusableWorkflowRepositoryPersistsCopiesAndRejectsBadImport() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("WorkflowStore-\(UUID())")
    defer { try? FileManager.default.removeItem(at: root) }
    let location = root.appendingPathComponent("library.json")
    let repository = WorkflowRepository(url: location)
    let workflow = ReusableWorkflow(name: "test", steps: [.init(kind: .stretch)])
    _ = try await repository.save(workflow)
    let reopened = WorkflowRepository(url: location)
    #expect(try await reopened.load() == [workflow])
    let export = root.appendingPathComponent("export.json")
    try await repository.exportWorkflow(workflow, to: export)
    let copy = try await repository.importWorkflow(from: export)
    #expect(copy.id != workflow.id)
    #expect(try await repository.load().count == 2)
    var invalid = workflow
    invalid.version = 99
    try JSONEncoder().encode(invalid).write(to: export)
    await #expect(throws: WorkflowError.self) { try await repository.importWorkflow(from: export) }
    #expect(try await repository.load().count == 2)
    _ = try await repository.remove(id: copy.id)
    #expect(try await reopened.load() == [workflow])
    await #expect(throws: WorkflowError.self) { try await repository.exportWorkflow(workflow, to: location) }
    #expect(try await reopened.load() == [workflow])
}

@Test func reusableWorkflowDisabledCalibrationDoesNotBecomeEnabled() throws {
    let step = try WorkflowStep.capture(EditOperation(kind: .stack, parameters: [
        "calibrationApplied": "false", "calibrationDarkIDs": UUID().uuidString, "stackMethod": "maximum",
    ]))
    #expect(step.calibration.isEmpty)
    #expect(step.parameters == ["stackMethod": "maximum"])
}

@Test func reusableWorkflowInputPlanningSeparatesTargetsAndCalibration() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("WorkflowInputs-\(UUID())")
    defer { try? FileManager.default.removeItem(at: root) }
    let lights = root.appendingPathComponent("lights")
    let darks = root.appendingPathComponent("darks")
    try FileManager.default.createDirectory(at: lights, withIntermediateDirectories: true)
    try FileManager.default.createDirectory(at: darks, withIntermediateDirectories: true)
    for name in ["frame10.fits", "frame2.fits"] { try Data([1]).write(to: lights.appendingPathComponent(name)) }
    try Data([1]).write(to: root.appendingPathComponent("ignore.fits"))
    try Data([1]).write(to: darks.appendingPathComponent("dark.fits"))
    let workflow = ReusableWorkflow(name: "stack", steps: [.init(kind: .stack, parameters: ["method": "maximum"], calibration: [.darks])])
    let plan = try WorkflowInputPlan.inspect(folder: root, workflow: workflow)
    #expect(plan.lights.map(\.lastPathComponent) == ["frame2.fits", "frame10.fits"])
    #expect(plan.calibration[.darks]?.count == 1)
    try FileManager.default.removeItem(at: darks)
    #expect(throws: (any Error).self) { try WorkflowInputPlan.inspect(folder: root, workflow: workflow) }
}
