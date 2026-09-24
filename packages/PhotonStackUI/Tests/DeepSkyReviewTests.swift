import Foundation
import PhotonStackAppCore
import PhotonStackProcessing
import Testing
@testable import PhotonStackUI

@Test @MainActor func applicationPersistenceWaitDrainsCompletedAndNewTasks() async {
    let completed = Task<Void, Never> {}
    await completed.value
    PhotonStackApplicationActivity.trackSettingsPersistence(completed)
    await PhotonStackApplicationActivity.waitForPendingSettingsPersistence()
    #expect(!PhotonStackApplicationActivity.hasPendingSettingsPersistence)

    var saved = false
    let registering = Task { @MainActor in
        PhotonStackApplicationActivity.trackSettingsPersistence(Task { @MainActor in
            await Task.yield()
            saved = true
        })
    }
    PhotonStackApplicationActivity.trackSettingsPersistence(registering)
    await PhotonStackApplicationActivity.waitForPendingSettingsPersistence()
    #expect(saved && !PhotonStackApplicationActivity.hasPendingSettingsPersistence)
}

private func reviewFrame(_ index: Int, recommended: Bool = true, usable: Bool = true,
                         warning: String? = nil, noise: Double = 1) throws -> DeepSkyReport.Frame {
    var object: [String: Any] = ["index": index, "input": "/tmp/review-\(index).fits", "bytes": 100, "modifiedTicks": 0,
        "reference": index == 0, "usable": usable, "selected": usable && recommended, "recommendedKeep": recommended,
        "temporalAvailable": true, "sky": 100, "noise": 2, "noiseRatio": noise, "coverage": 0.98, "fwhm": 3,
        "eccentricity": 0.3, "starCount": 100, "starsMeasured": true, "matches": 50,
        "positiveFraction": 0.00003, "negativeFraction": 0, "residualRms": 1,
        "rejectedLowSamples": 0, "rejectedHighSamples": 10, "stackComparedSamples": 1000,
        "errorCode": "", "message": "", "reasons": [], "warnings": [], "trails": []]
    if let warning { object["warnings"] = [warning] }
    return try JSONDecoder().decode(DeepSkyReport.Frame.self, from: JSONSerialization.data(withJSONObject: object))
}

@Test func deepSkyReviewUsesCurrentDecisionsWithoutChangingReport() throws {
    let frame = try reviewFrame(0)
    var settings = DeepSkySettings()
    #expect(DeepSkyReview.kept(frame, settings: settings))
    settings.overrides[frame.input] = "reject"
    #expect(!DeepSkyReview.kept(frame, settings: settings))
    #expect(DeepSkyReview.frames([frame], settings: settings, filter: .excluded, query: "", sort: .capture).count == 1)
    #expect(frame.selected) // The stored last-run result is not rewritten.
    settings.overrides[frame.input] = "auto"
    #expect(DeepSkyReview.kept(frame, settings: settings))
    #expect(!DeepSkyReview.manual(frame, settings: settings))
    let invalid = try reviewFrame(1, usable: false)
    settings.overrides[invalid.input] = "keep"
    #expect(!DeepSkyReview.kept(invalid, settings: settings))
    let softReject = try reviewFrame(2, recommended: false)
    settings.values["selection.apply"] = "off"
    #expect(DeepSkyReview.kept(softReject, settings: settings))
}

@Test func deepSkyReviewRetainsNativeShapeProvenanceAndReadsOldReports() throws {
    var frame = try reviewFrame(0)
    #expect(frame.starShapeCount == nil && frame.fwhmEstimator == nil)
    frame.starShapeCount = 83
    frame.fwhmEstimator = "native-gaussian-v1"
    let restored = try JSONDecoder().decode(DeepSkyReport.Frame.self, from: JSONEncoder().encode(frame))
    #expect(restored.starShapeCount == 83 && restored.fwhmEstimator == "native-gaussian-v1")
    let unknown = try reviewFrame(1, warning: "stellar-shapes-unavailable")
    #expect(DeepSkyReview.attention(unknown))
    #expect(DeepSkyReview.frames([unknown], settings: DeepSkySettings(), filter: .attention,
                               query: "星形样本不足", sort: .capture).count == 1)
}

@Test func deepSkyReviewDistinguishesCappedCountsFromNewMeasurements() throws {
    var frame = try reviewFrame(0)
    #expect(frame.starCountEstimator == nil)
    frame.starCount = 1000
    #expect(DeepSkyReview.starCountText(frame) == "≥1000")
    #expect(DeepSkyReview.starCountHelp(frame, chinese: true).contains("重新分析"))
    frame.starCountEstimator = "deduplicated-detection-v2"
    #expect(DeepSkyReview.starCountText(frame) == "1000")
    frame.starCount = 2345
    let restored = try JSONDecoder().decode(DeepSkyReport.Frame.self, from: JSONEncoder().encode(frame))
    #expect(restored.starCountEstimator == "deduplicated-detection-v2")
    #expect(DeepSkyReview.starCountText(restored) == "2345")
    frame.starsMeasured = false
    #expect(DeepSkyReview.starCountText(frame) == "—")
}

@Test func deepSkyReviewFindsTranslatedReasonsAndSortsAttentionStably() throws {
    let good = try reviewFrame(0)
    let warning = try reviewFrame(1, warning: "localized-transients-use-pixel-rejection", noise: 1.5)
    let bad = try reviewFrame(2, recommended: false)
    let invalid = try reviewFrame(3, usable: false)
    let frames = [good, warning, bad, invalid]
    let settings = DeepSkySettings()
    #expect(DeepSkyReview.frames(frames, settings: settings, filter: .attention, query: "", sort: .attention).map(\.index) == [3, 2, 1])
    #expect(DeepSkyReview.frames(frames, settings: settings, filter: .all, query: "局部异常", sort: .capture).map(\.index) == [1])
    #expect(DeepSkyReview.frames(frames, settings: settings, filter: .all, query: "", sort: .noise).first?.index == 1)
    #expect(DeepSkyReview.kept(warning, settings: settings))
}

@Test func deepSkyReviewSeparatesDecisionsFromStaleAnalysisAndGuardsMinimum() {
    let old = "PHOTONSTACK_DEEP_SKY_RECIPE 1\nselection.overrides \"auto\"\nselection.apply \"on\"\ninput \"/tmp/a.fit\"\n"
    #expect(DeepSkyReview.sameAnalysisRecipe(old.replacingOccurrences(of: "auto", with: "reject"), old))
    #expect(!DeepSkyReview.sameAnalysisRecipe(old.replacingOccurrences(of: "/tmp/a", with: "/tmp/b"), old))
    #expect(!DeepSkyReview.sameAnalysisRecipe(old.replacingOccurrences(of: "\"on\"", with: "\"off\""), old))
    var settings = DeepSkySettings()
    #expect(DeepSkyReview.minimumKept(settings: settings, count: 359) == 180)
    settings.values["selection.minimum-kept-fraction"] = "nan"
    #expect(DeepSkyReview.minimumKept(settings: settings, count: 359) == 3)
    settings.values["calibration.sensor-pattern"] = "on"
    #expect(DeepSkyReview.minimumKept(settings: settings, count: 20) == 36)
}

@Test @MainActor func deepSkyReviewBulkDecisionsAreReversibleAndBoundToProject() throws {
    var project = PhotonStackProject()
    let a = try reviewFrame(0), b = try reviewFrame(1)
    project.addAssets(from: [URL(fileURLWithPath: a.input), URL(fileURLWithPath: b.input)])
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false, processingService: UnavailableProcessingService())
    model.updateDeepSkyOverrides([a.input: "reject", b.input: "keep", "/unrelated.fits": "reject"])
    #expect(model.deepSkySettings.overrides[a.input] == "reject")
    #expect(model.deepSkySettings.overrides[b.input] == "keep")
    #expect(model.deepSkySettings.overrides["/unrelated.fits"] == nil)
    model.updateDeepSkyOverrides([a.input: "auto", b.input: "auto"])
    #expect(model.deepSkySettings.overrides.isEmpty)
    #expect(model.project.assets.count == 2)
    #expect(model.project.editGraph.operations.isEmpty)
}

@Test func deepSkyMasterReuseRejectsChangedSelectionAndRegistration() {
    let original = "PHOTONSTACK_DEEP_SKY_RECIPE 1\nregistration.mode \"similarity\"\nselection.overrides \"auto,auto\"\nbackground.model \"grid\"\ninput \"/tmp/a.fit\"\ninput \"/tmp/b.fit\"\n"
    #expect(DeepSkyReview.sameStackRecipe(original + "denoise.multiscale-chroma \"0.8\"\n", original))
    #expect(DeepSkyReview.sameStackRecipe(original.replacingOccurrences(of: "auto,auto", with: ""), original))
    #expect(!DeepSkyReview.sameStackRecipe(original.replacingOccurrences(of: "auto,auto", with: "reject,auto"), original))
    #expect(!DeepSkyReview.sameStackRecipe(original.replacingOccurrences(of: "similarity", with: "affine"), original))
    #expect(!DeepSkyReview.sameStackRecipe(original.replacingOccurrences(of: "/tmp/a.fit", with: "/tmp/other.fit"), original))
    #expect(!DeepSkyReview.sameStackRecipe(nil, original))
    for mode in ["on", "off"] {
        let withOutput = original + "output.save-denoise-stages \"\(mode)\"\n"
        #expect(DeepSkyReview.sameStackRecipe(withOutput, original))
        #expect(DeepSkyReview.sameAnalysisRecipe(withOutput, original))
    }
    #expect(DeepSkyReview.sameStackRecipe(original + "calibration.sensor-pattern \"off\"\n", original))
    #expect(!DeepSkyReview.sameStackRecipe(original + "calibration.sensor-pattern \"on\"\n", original))
}

@Test func deepSkyFinishingKeepsAnalysisButInvalidatesDisplayedResult() {
    let original = "PHOTONSTACK_DEEP_SKY_RECIPE 1\nregistration.mode \"similarity\"\nselection.overrides \"auto,auto\"\ninput \"/tmp/a.fit\"\ninput \"/tmp/b.fit\"\n"
    for line in ["denoise.h \"5\"", "denoise.multiscale-luminance \"0.2\"", "denoise.noise-model \"independent-luminance\"", "background.model \"grid\"", "crop.coverage \"0.9\"", "develop.saturation \"0.8\""] {
        let changed = original + line + "\n"
        #expect(DeepSkyReview.sameAnalysisRecipe(changed, original))
        #expect(DeepSkyReview.sameStackRecipe(changed, original))
        #expect(!DeepSkyReview.sameResultRecipe(changed, original))
    }
    let changedSelection = original.replacingOccurrences(of: "auto,auto", with: "reject,auto")
    #expect(DeepSkyReview.sameAnalysisRecipe(changedSelection, original))
    #expect(!DeepSkyReview.sameResultRecipe(changedSelection, original))
    #expect(!DeepSkyReview.sameStackRecipe(changedSelection, original))
    for changed in [original.replacingOccurrences(of: "similarity", with: "affine"), original.replacingOccurrences(of: "/tmp/a.fit", with: "/tmp/c.fit"), original + "selection.max-noise-ratio \"2\"\n"] {
        #expect(!DeepSkyReview.sameAnalysisRecipe(changed, original))
        #expect(!DeepSkyReview.sameResultRecipe(changed, original))
    }
    #expect(DeepSkyReview.sameResultRecipe(original + "output.save-denoise-stages \"on\"\n", original))
    #expect(DeepSkyReview.sameResultRecipe(original + "denoise.noise-model \"scene\"\n", original))
    #expect(DeepSkyReview.sameResultRecipe(original.replacingOccurrences(of: "auto,auto", with: ""), original))
    #expect(!DeepSkyReview.sameResultRecipe(nil, original))
}
