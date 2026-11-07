import Foundation
import PhotonStackAppCore
import Testing
@testable import PhotonStackUI

private func reviewEvidenceFixture(_ count: Int = 5) throws -> DeepSkyReport {
    let base = #"{"index":0,"input":"/tmp/a.fit","bytes":100,"modifiedTicks":0,"reference":false,"usable":true,"selected":true,"recommendedKeep":true,"temporalAvailable":true,"sky":20,"noise":2,"noiseRatio":1,"coverage":0.99,"fwhm":3,"eccentricity":0.71,"starCount":80,"starsMeasured":true,"matches":50,"positiveFraction":0,"negativeFraction":0,"residualRms":1,"rejectedLowSamples":0,"rejectedHighSamples":0,"stackComparedSamples":0,"errorCode":"","message":"","reasons":[],"warnings":[]}"#
    let frame = try JSONDecoder().decode(DeepSkyReport.Frame.self, from: Data(base.utf8))
    let envelope = #"{"version":1,"success":true,"analysisOnly":true,"inputCount":0,"selectedCount":0,"referenceIndex":0,"recipe":"","errorCode":"","message":"","master":"","backgroundMaster":"","developed":"","rejectionLow":"","rejectionHigh":"","frames":[]}"#
    var report = try JSONDecoder().decode(DeepSkyReport.self, from: Data(envelope.utf8))
    report.inputCount = count
    report.selectedCount = count
    report.recipe = "PHOTONSTACK_DEEP_SKY_RECIPE 1\nselection.maximum-eccentricity \"0.8\"\nselection.maximum-noise-ratio \"2\"\nselection.maximum-residual-fraction \"0.15\"\nregistration.minimum-matches \"30\"\n"
    report.frames = (0..<count).map { i in
        var copy = frame
        copy.index = i
        copy.input = "/tmp/review-fixture-\(i).fit"
        return copy
    }
    return report
}

@Test func deepSkyReviewEvidenceReportsTrueCausesSeparatelyFromWarningsAndManualOverrides() throws {
    var report = try reviewEvidenceFixture()
    report.frames[0].recommendedKeep = false
    report.frames[0].selected = false
    report.frames[0].reasons = ["elongated-stars", "high-noise", "elongated-stars"]
    report.frames[0].warnings = ["localized-transients-use-pixel-rejection"]
    report.frames[0].eccentricity = 0.88534
    report.frames[0].noiseRatio = 2.15
    report.frames[1].eccentricity = 0.58
    report.frames[1].positiveFraction = 0.003
    report.frames[1].warnings = ["localized-transients-use-pixel-rejection", "localized-transients-use-pixel-rejection"]
    report.frames[2].warnings = ["elongated-stars-absolute"]
    report.frames[2].eccentricity = 0.82845
    report.frames[3].recommendedKeep = false
    report.frames[3].selected = false
    report.frames[3].usable = false
    report.frames[3].errorCode = "RegistrationRejected"
    report.frames[3].reasons = ["RegistrationRejected"]
    report.frames[3].message = "Similarity consensus failed"
    report.frames[3].fwhm = 75
    report.frames[3].eccentricity = 0.99
    report.frames[3].matches = 0
    report.frames[4].recommendedKeep = false
    report.frames[4].reasons = ["broad-stars", "user-excluded"]
    report.frames[4].eccentricity = 0.59
    report.frames[4].fwhm = 6
    var settings = DeepSkySettings()
    settings.overrides = [report.frames[1].input: "reject", report.frames[3].input: "keep", report.frames[4].input: "keep"]
    // Editing the current threshold must not rewrite last analysis evidence.
    settings.values["selection.maximum-eccentricity"] = "0.99"
    let selection = DeepSkyReviewSelection(report: report, settings: settings, isCurrent: true)
    let evidence = DeepSkyReviewEvidence(report: report)
    let items = report.frames.map { evidence.frame($0, selection: selection, chinese: true) }
    #expect(items.map(\.decision) == [.automaticallyExcluded, .manuallyExcluded, .automaticallyKept, .unavailable, .manuallyKept])
    #expect(items.map(\.kept) == [false, false, true, false, true])
    #expect(items[0].reasons.map(\.code) == ["elongated-stars", "high-noise"])
    #expect(items[0].warnings.map(\.code) == ["localized-transients-use-pixel-rejection"])
    let shape = try #require(items[0].reasons.first)
    #expect(shape.detail.contains("0.885") && shape.detail.contains("0.709") && shape.detail.contains("0.800"))
    #expect(!shape.detail.contains("0.990"))
    #expect(items[1].reasons.isEmpty && items[1].warnings.count == 1)
    #expect(items[2].reasons.isEmpty && items[2].warnings.map(\.code) == ["elongated-stars-absolute"])
    #expect(items[3].reasons.map(\.code) == ["RegistrationRejected"])
    #expect(items[3].reasons.first?.detail.contains("Similarity consensus failed") == true)
    #expect(items[4].decisionTitle == "强制保留" && items[4].reasons.map(\.code) == ["broad-stars"])
    let summary = DeepSkyExclusionSummary(report: report, selection: selection, chinese: true)
    #expect(summary.hasMultipleReasonsPerFrame)
    #expect(Dictionary(uniqueKeysWithValues: summary.items.map { ($0.code, $0.count) }) ==
            ["elongated-stars": 1, "high-noise": 1, "user-excluded": 1, "RegistrationRejected": 1])
}

@Test func deepSkyReviewEvidenceHonorsHistoricalSourceAndMissingRecipeThresholds() throws {
    var report = try reviewEvidenceFixture(2)
    report.frames[0].recommendedKeep = false
    report.frames[0].reasons = ["high-noise"]
    report.frames[0].selected = false
    report.frames[1].selected = true
    report.frames[1].recommendedKeep = false
    report.frames[1].reasons = ["few-stars"]
    report.recipe = ""
    var settings = DeepSkySettings()
    settings.values["selection.apply"] = "off"
    settings.values["selection.maximum-noise-ratio"] = "99"
    settings.overrides[report.frames[0].input] = "keep"
    var selection = DeepSkyReviewSelection(report: report, settings: settings, isCurrent: false)
    var evidence = DeepSkyReviewEvidence(report: report)
    let unknown = evidence.frame(report.frames[0], selection: selection, chinese: false)
    #expect(unknown.decision == .recordedExcluded && !unknown.kept)
    #expect(unknown.reasons.map(\.code) == ["high-noise"] && !unknown.reasons[0].detail.contains("99"))
    #expect(DeepSkyReviewEvidence.exclusionCodes(report.frames[0], selection: selection) == ["reported-exclusion"])
    #expect(evidence.frame(report.frames[1], selection: selection, chinese: true).decision == .recordedKept)
    report.recipe = "PHOTONSTACK_DEEP_SKY_RECIPE 1\nselection.apply \"off\"\nselection.overrides \"reject,auto\"\n"
    selection = DeepSkyReviewSelection(report: report, settings: settings, isCurrent: false)
    evidence = DeepSkyReviewEvidence(report: report)
    #expect(evidence.frame(report.frames[0], selection: selection, chinese: true).decision == .manuallyExcluded)
    #expect(evidence.frame(report.frames[1], selection: selection, chinese: true).decision == .selectionDisabled)
}

@Test func deepSkyReviewEvidenceUsesMeasuredPeersForGroupLimits() throws {
    var report = try reviewEvidenceFixture(5)
    for i in report.frames.indices {
        report.frames[i].positiveFraction = [0.01, 0.02, 0.03, 0.04, 0.21][i]
    }
    report.frames[4].negativeFraction = 0.01
    report.frames[4].selected = false
    report.frames[4].recommendedKeep = false
    report.frames[4].reasons = ["broad-temporal-residuals"]
    report.frames[3].starsMeasured = false
    report.frames[3].fwhm = 0
    report.frames[3].eccentricity = 0
    report.frames[2].warnings = ["elongated-stars-absolute"]
    report.frames[2].eccentricity = 0.828
    let selection = DeepSkyReviewSelection(settings: DeepSkySettings())
    let evidence = DeepSkyReviewEvidence(report: report)
    let broad = try #require(evidence.frame(report.frames[4], selection: selection, chinese: false).reasons.first)
    #expect(broad.detail.contains("22.00%") && broad.detail.contains("15.00%"))
    #expect(evidence.frame(report.frames[2], selection: selection, chinese: true).warnings.first?.detail.contains("0.710") == true)
    report.recipe += "selection.measure-stars \"off\"\n"
    #expect(DeepSkyReviewEvidence(report: report).frame(report.frames[3], selection: selection, chinese: true).warnings.isEmpty)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_REVIEW_M31_711_REPORT"] != nil))
func deepSkyReviewEvidenceMatchesRealM31Exclusions() throws {
    let path = try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_REVIEW_M31_711_REPORT"])
    let report = try JSONDecoder().decode(DeepSkyReport.self, from: Data(contentsOf: URL(fileURLWithPath: path)))
    try report.validate()
    let context = DeepSkyReviewEvidence(report: report)
    let selection = DeepSkyReviewSelection(report: report, settings: DeepSkySettings(), isCurrent: false)
    let summary = DeepSkyExclusionSummary(report: report, selection: selection, chinese: true)
    #expect(report.inputCount == 711 && report.selectedCount == 654 && summary.items.count == 1)
    #expect(summary.items.first?.code == "elongated-stars" && summary.items.first?.count == 57)
    let excluded = context.frame(report.frames[6], selection: selection, chinese: true)
    #expect(excluded.decision == .automaticallyExcluded && excluded.reasons.map(\.code) == ["elongated-stars"])
    #expect(excluded.warnings.map(\.code) == ["localized-transients-use-pixel-rejection"])
    #expect(excluded.reasons[0].detail.contains("0.885") && excluded.reasons[0].detail.contains("0.710") &&
            excluded.reasons[0].detail.contains("0.860"))
    let kept = context.frame(report.frames[0], selection: selection, chinese: true)
    #expect(kept.decision == .automaticallyKept && kept.reasons.isEmpty)
    #expect(kept.warnings.first?.detail.contains("0.828") == true)
    let borderline = try #require(report.frames.filter { !$0.selected }.min { $0.eccentricity < $1.eccentricity })
    let detail = try #require(context.frame(borderline, selection: selection, chinese: true).reasons.first?.detail)
    #expect(detail.contains("本帧偏心率 0.8597") && detail.contains("相对排除线约 0.8596"))
    let partial = context.frame(report.frames[622], selection: selection, chinese: true)
    #expect(partial.kept && partial.warnings.contains { $0.code == "stellar-shapes-unavailable" })
}
