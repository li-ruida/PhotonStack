import Foundation
import Testing
@testable import PhotonStackAppCore

@Test func deepSkyColorTransferPreservesSparseProjectsAndReusableWorkflows() throws {
    let key = "develop.color-transfer"
    let current = DeepSkySchema(version: 1, fields: [
        (key, "linear-light"), ("stack.method", "winsorized"), ("registration.reference", "-1"),
        ("stack.weights", ""), ("background.exclusions", ""), ("calibration.darks", ""),
        ("calibration.biases", ""), ("calibration.flats", "")
    ].map { name, value in
        DeepSkySchema.Field(key: name, label: name, chineseLabel: name, group: "develop", type: "text",
                            defaultValue: value, minimum: 0, maximum: 0, choices: [])
    })
    let decoder = JSONDecoder(), encoder = JSONEncoder()
    let newProject = try decoder.decode(DeepSkySettings.self, from: encoder.encode(DeepSkySettings()))
    #expect(try newProject.recipe(schema: current, inputs: []).contains("develop.color-transfer \"linear-light\""))
    let old = Data(#"{"version":1,"stackDefaultsVersion":2,"values":{},"overrides":{}}"#.utf8)
    var saved = try decoder.decode(DeepSkySettings.self, from: old)
    #expect(saved.values[key] == "legacy-code")
    #expect(try decoder.decode(DeepSkySettings.self, from: encoder.encode(saved)) == saved)
    saved.setCompatibleValue("linear-light", for: key, schema: current)
    #expect(try saved.recipe(schema: current, inputs: []).contains("develop.color-transfer \"linear-light\""))

    let plan = WorkflowInputPlan(folder: URL(fileURLWithPath: "/tmp"), lights: [], calibration: [:])
    let newStep = try WorkflowStep.deepSky(values: [:]).step
    let reopened = try decoder.decode(WorkflowStep.self, from: encoder.encode(newStep))
    #expect(try plan.deepSkyRecipe(for: reopened, schema: current).contains("develop.color-transfer \"linear-light\""))
    let savedWorkflow = Data(#"{"kind":"stack","parameters":{},"deepSkyValues":{"stack.method":"winsorized"},"calibration":[]}"#.utf8)
    let oldStep = try decoder.decode(WorkflowStep.self, from: savedWorkflow)
    #expect(try plan.deepSkyRecipe(for: oldStep, schema: current).contains("develop.color-transfer \"legacy-code\""))
}
