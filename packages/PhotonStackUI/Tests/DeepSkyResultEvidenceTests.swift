import Foundation
import PhotonStackAppCore
import Testing
@testable import PhotonStackUI

private func evidenceReport() throws -> DeepSkyReport {
    let frame: [String: Any] = [
        "index": 0, "input": "/tmp/evidence-light.fits", "bytes": 100, "modifiedTicks": 0,
        "reference": true, "usable": true, "selected": true, "recommendedKeep": true,
        "temporalAvailable": true, "sky": 100, "noise": 2, "noiseRatio": 1,
        "coverage": 0.98, "fwhm": 3, "eccentricity": 0.3, "starCount": 100,
        "starsMeasured": true, "matches": 50, "positiveFraction": 0,
        "negativeFraction": 0, "residualRms": 1, "rejectedLowSamples": 0,
        "rejectedHighSamples": 0, "stackComparedSamples": 1000,
        "errorCode": "", "message": "", "reasons": [], "warnings": []
    ]
    let report: [String: Any] = [
        "version": 1, "success": true, "analysisOnly": false,
        "inputCount": 1, "selectedCount": 1, "referenceIndex": 0,
        "recipe": "PHOTONSTACK_DEEP_SKY_RECIPE 1\n", "errorCode": "", "message": "",
        "master": "/tmp/master.fits", "backgroundMaster": "/tmp/background.fits",
        "developed": "/tmp/developed.tiff", "rejectionLow": "", "rejectionHigh": "",
        "frames": [frame]
    ]
    return try JSONDecoder().decode(DeepSkyReport.self, from: JSONSerialization.data(withJSONObject: report))
}

@Test func resultEvidenceDoesNotTreatUnmeasuredLegacyFramesAsPassing() throws {
    var report = try evidenceReport()
    report.frames[0].starsMeasured = false
    report.frames[0].temporalAvailable = false
    try report.validate()
    let original = report
    let assessment = DeepSkyResultAssessment(report)
    #expect(assessment.attention == 1)
    #expect(assessment.issues.map(\.code) == [
        "stellar-measurements-unavailable", "temporal-measurements-unavailable"
    ])
    #expect(assessment.issues.allSatisfy { $0.frameIndices == [0] })
    #expect(DeepSkyResultAssessment.advice("temporal-measurements-unavailable", chinese: false).contains("not a pass"))
    #expect(DeepSkyReview.reason("temporal-measurements-unavailable", chinese: true) == "帧间异常测量不可用")
    #expect(DeepSkyReview.listing(report.frames, settings: DeepSkySettings(), filter: .attention,
                               query: "帧间异常测量", sort: .attention).frames.map(\.index) == [0])
    #expect(report == original)
}

@Test func resultEvidenceKeepsAttentionCountConsistentWithReasonOnlyIssues() throws {
    var report = try evidenceReport()
    report.frames[0].reasons = ["future-quality-cue", "future-quality-cue"]
    let assessment = DeepSkyResultAssessment(report)
    #expect(assessment.attention == 1)
    #expect(assessment.issues.count == 1)
    #expect(assessment.issues.first?.frameIndices == [0])
    #expect(DeepSkyReview.attention(report.frames[0]))
    #expect(DeepSkyReview.reasons(report.frames[0], chinese: false) == ["future quality cue"])
}

@Test func resultEvidenceDeduplicatesKnownUnavailableChecksAndExcludesUnusedFrames() throws {
    var report = try evidenceReport()
    report.frames[0].starsMeasured = false
    report.frames[0].temporalAvailable = false
    report.frames[0].warnings = ["stellar-measurements-unavailable", "insufficient-temporal-peers"]
    let assessment = DeepSkyResultAssessment(report)
    #expect(assessment.attention == 1)
    #expect(assessment.issues.map(\.code) == ["insufficient-temporal-peers", "stellar-measurements-unavailable"])
    report.frames[0].selected = false
    report.selectedCount = 0
    try report.validate()
    let excluded = DeepSkyResultAssessment(report)
    #expect(excluded.attention == 0 && excluded.issues.isEmpty)
}

@Test func resultEvidenceIgnoresEmptyAndSelectionOnlyCues() throws {
    var report = try evidenceReport()
    report.frames[0].reasons = ["", "user-excluded"]
    report.frames[0].warnings = [""]
    let assessment = DeepSkyResultAssessment(report)
    #expect(assessment.attention == 0 && assessment.issues.isEmpty)
    #expect(!DeepSkyReview.attention(report.frames[0]))
}

@Test func resultEvidenceKeepsUniformlyElongatedFramesVisibleForReview() throws {
    var report = try evidenceReport()
    report.frames[0].warnings = ["elongated-stars-absolute"]
    let assessment = DeepSkyResultAssessment(report)
    #expect(report.frames[0].recommendedKeep)
    #expect(DeepSkyReview.kept(report.frames[0], settings: DeepSkySettings()))
    #expect(assessment.attention == 1)
    #expect(DeepSkyReview.listing(report.frames, settings: DeepSkySettings(), filter: .attention,
                               query: "偏心率", sort: .attention).frames.count == 1)
    #expect(DeepSkyResultAssessment.advice("elongated-stars-absolute", chinese: true).contains("1:1"))
}

@Test func failedAnalysisCannotAdvanceTheJourneyEvenWhenItsRecipeMatches() throws {
    var report = try evidenceReport()
    report.analysisOnly = true
    #expect(DeepSkyReview.analysisIsCurrent(report, recipe: report.recipe))
    report.success = false
    report.errorCode = "RegistrationRejected"
    #expect(!DeepSkyReview.analysisIsCurrent(report, recipe: report.recipe))
    let state = WorkspaceJourneyState(lightCount: 10,
        analysisCurrent: DeepSkyReview.analysisIsCurrent(report, recipe: report.recipe),
        resultCurrent: false, finishingRequested: false)
    #expect(state.step == .review)
    #expect(!state.completed(.review))
    report.success = true
    #expect(!DeepSkyReview.analysisIsCurrent(report, recipe: report.recipe + "input \"/tmp/new.fits\"\n"))
    #expect(!DeepSkyReview.analysisIsCurrent(nil, recipe: report.recipe))
}
