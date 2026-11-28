import Foundation
import Testing
@testable import PhotonStackAppCore

@Test(arguments: [CalibrationFrameRole.dark, .bias, .flat])
func auditNewCalibrationAssetsUpdateExplicitLists(role: CalibrationFrameRole) throws {
    var project = PhotonStackProject()
    var settings = DeepSkySettings()
    let key = role == .dark ? "calibration.darks" : role == .bias ? "calibration.biases" : "calibration.flats"
    settings.values[key] = "\"/tmp/external-master.fits\""
    project.deepSkySettings = settings
    let input = URL(fileURLWithPath: "/tmp/new-calibration.fits")
    project.addAssets(from: [input], role: role)
    #expect(project.deepSkySettings?.values[key] == "\"/tmp/external-master.fits\" \"/tmp/new-calibration.fits\"")
    project.removeAsset(try #require(project.assets.first?.id))
    project.deepSkySettings?.values[key] = ""
    project.addAssets(from: [input, input], role: role)
    #expect(project.deepSkySettings?.values[key] == "\"/tmp/new-calibration.fits\"")
}

@Test func auditRegroupedMosaicSourcesKeepReferenceAndWeightsAttached() {
    var project = PhotonStackProject()
    project.addAssets(from: ["a", "b"].map { URL(fileURLWithPath: "/tmp/audit-panel/lights/\($0).fits") }, role: .mosaic)
    project.addAssets(from: [URL(fileURLWithPath: "/tmp/existing-light.fits")], role: .light)
    let referenceID = project.assets[2].id
    var settings = DeepSkySettings()
    settings.values = ["registration.reference": "0", "stack.weights": "7"]
    project.deepSkySettings = settings
    project.regroupMosaicLightSequences()
    let lights = project.assets.filter { $0.role == .light }
    #expect(lights.count == 2)
    #expect(lights.last?.id == referenceID)
    #expect(project.deepSkySettings?.values["registration.reference"] == "1")
    #expect(project.deepSkySettings?.values["stack.weights"] == "1,7")
}

@Test func auditPortableCurvesRetainRawDecodeSettings() throws {
    let raw = ["rawWhiteBalance": "manual", "rawTemperature": "4500", "rawTint": "12",
               "rawExposureBias": "1.5", "rawBlackLevel": "manual", "rawBlackValue": "0.01",
               "rawDemosaic": "high", "rawLinear": "true"]
    var parameters = raw; parameters["points"] = "0:0,1:1"; parameters["channel"] = "rgb"
    let step = try WorkflowStep.capture(EditOperation(kind: .curves, parameters: parameters))
    for (key, value) in raw { #expect(step.parameters[key] == value) }
    let decoded = try JSONDecoder().decode(WorkflowStep.self, from: JSONEncoder().encode(step))
    #expect(decoded == step)
}
