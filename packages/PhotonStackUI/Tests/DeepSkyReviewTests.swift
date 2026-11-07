import Foundation
import PhotonStackAppCore
import PhotonStackProcessing
import Testing
import SwiftUI
import ImageIO
import UniformTypeIdentifiers
import AppKit
@testable import PhotonStackUI

// ImageRenderer replaces AppKit-backed controls with unsupported-view symbols.
// Render in an isolated, never-shown host window; do not launch or alter the App.
@MainActor private func reviewSnapshot<V: View>(_ view: V) throws -> CGImage {
    _ = NSApplication.shared
    let host = NSHostingView(rootView: view)
    let size = host.fittingSize
    let window = NSWindow(contentRect: CGRect(origin: .zero, size: size),
                          styleMask: .borderless, backing: .buffered, defer: false)
    window.isReleasedWhenClosed = false
    window.contentView = host
    defer { window.contentView = nil; window.close() }
    host.frame = CGRect(origin: .zero, size: size)
    host.layoutSubtreeIfNeeded()
    let bitmap = try #require(NSBitmapImageRep(bitmapDataPlanes: nil,
        pixelsWide: Int(size.width.rounded(.up)), pixelsHigh: Int(size.height.rounded(.up)),
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0))
    host.cacheDisplay(in: host.bounds, to: bitmap)
    return try #require(bitmap.cgImage)
}

private func resultReviewReport() throws -> DeepSkyReport {
    let json = #"{"version":1,"success":true,"analysisOnly":false,"inputCount":0,"selectedCount":0,"referenceIndex":0,"recipe":"PHOTONSTACK_DEEP_SKY_RECIPE 1\n","errorCode":"","message":"","master":"/tmp/master.fits","backgroundMaster":"/tmp/bg.fits","developed":"/tmp/developed.tiff","rejectionLow":"","rejectionHigh":"","frames":[]}"#
    return try JSONDecoder().decode(DeepSkyReport.self, from: Data(json.utf8))
}

@Test func deepSkyResultReviewKeepsMissingMetricsUnknownAndTracksProvenance() throws {
    var report = try resultReviewReport()
    try report.validate()
    #expect(report.displayReview == nil && report.masterReused == nil)
    #expect(report.denoiseStretchLocked == nil)
    #expect(DeepSkyResultReview.cues(report) == ["reused-master", "unmeasured"])
    report.masterReused = true
    report.frames = try (0..<6).map { try reviewFrame($0) }
    report.inputCount = 6; report.selectedCount = 6
    report.frames[2].recommendedKeep = false
    try report.validate()
    #expect(DeepSkyResultReview.cues(report) == ["reused-master", "few-frames", "override", "unmeasured"])
    let restored = try JSONDecoder().decode(DeepSkyReport.self, from: JSONEncoder().encode(report))
    #expect(restored.masterReused == true && restored.displayReview == nil)
    report.analysisOnly = true
    #expect(DeepSkyResultReview.cues(report).isEmpty)
    report.analysisOnly = false; report.success = false
    #expect(DeepSkyResultReview.cues(report).isEmpty)
}

@Test func deepSkyResultReviewValidatesCountsAndUsesReviewCuesNotQualityPass() throws {
    var report = try resultReviewReport()
    for (black,white,expected) in [(9,0,["reused-master"]), (10,1,["reused-master","near-black","near-white"])] {
        report.displayReview = try JSONDecoder().decode(DeepSkyReport.DisplayReview.self,
            from: Data("{\"version\":1,\"pixels\":1000,\"nearBlackPixels\":\(black),\"nearWhitePixels\":\(white)}".utf8))
        try report.validate()
        #expect(DeepSkyResultReview.cues(report) == expected)
        let restored = try JSONDecoder().decode(DeepSkyReport.self, from: JSONEncoder().encode(report))
        #expect(restored.displayReview == report.displayReview)
    }
    for bad in [#"{"version":2,"pixels":100,"nearBlackPixels":0,"nearWhitePixels":0}"#,
                #"{"version":1,"pixels":0,"nearBlackPixels":0,"nearWhitePixels":0}"#,
                #"{"version":1,"pixels":100,"nearBlackPixels":101,"nearWhitePixels":0}"#,
                #"{"version":1,"pixels":100,"nearBlackPixels":99,"nearWhitePixels":2}"#] {
        report.displayReview = try JSONDecoder().decode(DeepSkyReport.DisplayReview.self, from: Data(bad.utf8))
        #expect(throws: DeepSkyRecipeError.self) { try report.validate() }
    }
    #expect(throws: DecodingError.self) {
        try JSONDecoder().decode(DeepSkyReport.DisplayReview.self,
            from: Data(#"{"version":1,"pixels":100,"nearBlackPixels":-1,"nearWhitePixels":0}"#.utf8))
    }
    report.displayReview = try JSONDecoder().decode(DeepSkyReport.DisplayReview.self,
        from: Data(#"{"version":1,"pixels":100,"nearBlackPixels":0,"nearWhitePixels":0}"#.utf8))
    report.analysisOnly = true
    #expect(throws: DeepSkyRecipeError.self) { try report.validate() }
}

@Test @MainActor func deepSkyResultReviewCanRenderChineseAndEnglish() throws {
    var report = try resultReviewReport()
    report.denoiseStretchLocked = true
    report.displayReview = try JSONDecoder().decode(DeepSkyReport.DisplayReview.self,
        from: Data(#"{"version":1,"pixels":1000,"nearBlackPixels":140,"nearWhitePixels":3}"#.utf8))
    for chinese in [true,false] {
        let view = DeepSkyResultReviewView(report: report, chinese: chinese, viewImage: {}, editFinishing: {}, expanded: true)
            .padding(18).frame(width: 620).background(Color.white).environment(\.colorScheme, .light)
        let image = try reviewSnapshot(view)
        #expect(image.width == 620 && image.height > 150)
        if let directory = ProcessInfo.processInfo.environment["PHOTONSTACK_REVIEW_SNAPSHOT"] {
            let url = URL(fileURLWithPath: directory).appendingPathComponent(chinese ? "review-zh.png" : "review-en.png")
            try #require(!FileManager.default.fileExists(atPath: url.path))
            let destination = try #require(CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil))
            CGImageDestinationAddImage(destination, image, nil)
            #expect(CGImageDestinationFinalize(destination))
        }
    }
}

@Test func deepSkyToneReferenceReportRetainsVerifiedState() throws {
    var report = try resultReviewReport()
    report.denoiseStretchLocked = true
    try report.validate()
    let decoded = try JSONDecoder().decode(DeepSkyReport.self, from: JSONEncoder().encode(report))
    #expect(decoded.denoiseStretchLocked == true)
    report.analysisOnly = true
    #expect(throws: DeepSkyRecipeError.self) { try report.validate() }
    report.analysisOnly = false; report.success = false
    #expect(throws: DeepSkyRecipeError.self) { try report.validate() }
}

@Test @MainActor func deepSkyToneReferenceControlRendersBothLanguagesAndThemes() throws {
    for chinese in [true, false] {
        for dark in [true, false] {
            let view = DeepSkyToneReferenceControl(value: .constant(dark ? "off" : "on"),
                title: chinese ? "固定降噪前的色调参考" : "Use pre-denoise tone reference", chinese: chinese)
                .padding(18).frame(width: 620)
                .background(dark ? Color.black : Color.white)
                .environment(\.colorScheme, dark ? .dark : .light)
            let image = try reviewSnapshot(view)
            #expect(image.width == 620 && image.height > 80)
            if let directory = ProcessInfo.processInfo.environment["PHOTONSTACK_TONE_REFERENCE_SNAPSHOT"] {
                let name = "tone-\(chinese ? "zh" : "en")-\(dark ? "dark" : "light").png"
                let url = URL(fileURLWithPath: directory).appendingPathComponent(name)
                try #require(!FileManager.default.fileExists(atPath: url.path))
                let destination = try #require(CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil))
                CGImageDestinationAddImage(destination, image, nil)
                #expect(CGImageDestinationFinalize(destination))
            }
        }
    }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_REVIEW_REPORT"] != nil))
func deepSkyResultReviewDecodesRealEngineReport() throws {
    let path = try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_REVIEW_REPORT"])
    let data = try Data(contentsOf: URL(fileURLWithPath: path))
    let report = try JSONDecoder().decode(DeepSkyReport.self, from: data)
    try report.validate()
    #expect(report.success && !report.analysisOnly)
    let metrics = try #require(report.displayReview)
    #expect(metrics.pixels > 0)
    let restored = try JSONDecoder().decode(DeepSkyReport.self, from: JSONEncoder().encode(report))
    #expect(restored.displayReview == metrics)
    #expect(restored.masterReused == report.masterReused)
    #expect(DeepSkyResultReview.cues(restored) == DeepSkyResultReview.cues(report))
}

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

private func sourceReviewReport() throws -> DeepSkyReport {
    var report = try resultReviewReport()
    report.inputCount = 10
    report.frames = try (0..<10).map { try reviewFrame($0) }
    report.frames[0].warnings = ["localized-transients-use-pixel-rejection", "stellar-shapes-unavailable"]
    report.frames[0].reasons = ["stellar-shapes-unavailable"] // duplicated code counts one exposure
    report.frames[1].warnings = ["localized-transients-use-pixel-rejection"]
    report.frames[2].recommendedKeep = false
    report.frames[2].reasons = ["elongated-stars"]
    report.frames[7].selected = false
    report.frames[7].recommendedKeep = false
    report.frames[7].reasons = ["high-noise"]
    report.frames[8].selected = false; report.frames[8].usable = false
    report.frames[8].errorCode = "RegistrationRejected"
    report.frames[9].selected = false; report.frames[9].usable = false
    report.frames[9].errorCode = "DecodeFailed"
    report.selectedCount = 7
    return report
}

@Test func deepSkyResultAssessmentSeparatesExcludedFailuresAndRetainedRisks() throws {
    let report = try sourceReviewReport()
    try report.validate()
    let original = report
    let assessment = DeepSkyResultAssessment(report)
    #expect(assessment.input == 10 && assessment.stacked == 7 && assessment.excluded == 3)
    #expect(assessment.attention == 3) // overlap is not counted twice
    #expect(assessment.registrationFailed == 1 && assessment.otherFailed == 1)
    #expect(assessment.issues.map(\.code) == ["localized-transients-use-pixel-rejection", "elongated-stars", "stellar-shapes-unavailable"])
    #expect(assessment.issues.map(\.frameIndices) == [[0,1], [2], [0]])
    #expect(!assessment.issues.contains { $0.code == "high-noise" }) // already excluded
    #expect(assessment.summary(chinese: true).contains("入栈中需复查 3 张"))
    #expect(assessment.summary(chinese: false).contains("3 stacked frames need review"))
    #expect(report == original)
    let restored = try JSONDecoder().decode(DeepSkyReport.self, from: JSONEncoder().encode(report))
    #expect(DeepSkyResultAssessment(restored).issues == assessment.issues)
}

@Test func deepSkyResultAssessmentDoesNotInventExposureOrHideUnknownWarnings() throws {
    let master = try resultReviewReport()
    #expect(DeepSkyResultAssessment(master).issues.isEmpty)
    #expect(DeepSkyResultAssessment(master).input == 0)
    var report = try sourceReviewReport()
    report.masterReused = true // inherited source records still describe the original stack
    report.frames[3].warnings = ["future-warning", "future-warning", ""]
    let assessment = DeepSkyResultAssessment(report)
    #expect(assessment.attention == 4)
    #expect(assessment.issues.first { $0.code == "future-warning" }?.frameIndices == [3])
    #expect(DeepSkyResultAssessment.advice("future-warning", chinese: true).contains("不自动改变"))
    #expect(DeepSkyResultAssessment.advice("localized-transients-use-pixel-rejection", chinese: true).contains("不等于残留"))
    #expect(DeepSkyResultAssessment.advice("stellar-shapes-unavailable", chinese: false).contains("not a pass"))
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_SOURCE_REVIEW_REPORT"] != nil))
func deepSkyResultAssessmentReadsArchivedC28SourceEvidence() throws {
    let path = try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_SOURCE_REVIEW_REPORT"])
    let report = try JSONDecoder().decode(DeepSkyReport.self, from: Data(contentsOf: URL(fileURLWithPath: path)))
    try report.validate()
    let assessment = DeepSkyResultAssessment(report)
    #expect(assessment.input == 102 && assessment.stacked == 93 && assessment.excluded == 9)
    #expect(assessment.attention == 5 && assessment.registrationFailed == 0 && assessment.otherFailed == 0)
    #expect(assessment.issues.count == 1)
    #expect(assessment.issues.first?.code == "localized-transients-use-pixel-rejection")
    #expect(assessment.issues.first?.frameIndices.count == 5)
}

@Test @MainActor func deepSkyResultAssessmentRendersSourceCues() throws {
    let report = try sourceReviewReport()
    for chinese in [true, false] {
        let view = DeepSkyResultReviewView(report: report, chinese: chinese, viewImage: {},
            editFinishing: {}, reviewFrames: {}, expanded: true)
            .padding(18).frame(width: 620).background(Color.white).environment(\.colorScheme, .light)
        let image = try reviewSnapshot(view)
        #expect(image.width == 620 && image.height > 180 && image.height < 440)
        if let directory = ProcessInfo.processInfo.environment["PHOTONSTACK_SOURCE_REVIEW_SNAPSHOT"] {
            let url = URL(fileURLWithPath: directory).appendingPathComponent(chinese ? "source-zh.png" : "source-en.png")
            try #require(!FileManager.default.fileExists(atPath: url.path))
            let destination = try #require(CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil))
            CGImageDestinationAddImage(destination, image, nil)
            #expect(CGImageDestinationFinalize(destination))
        }
    }
}

@Test func deepSkyReviewUsesCurrentDecisionsWithoutChangingReport() throws {
    let frame = try reviewFrame(0)
    var settings = DeepSkySettings()
    #expect(DeepSkyReview.kept(frame, settings: settings))
    settings.overrides[frame.input] = "reject"
    #expect(!DeepSkyReview.kept(frame, settings: settings))
    #expect(DeepSkyReview.listing([frame], settings: settings, filter: .excluded, query: "", sort: .capture).frames.count == 1)
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

@Test func deepSkyReviewHistoricalSelectionUsesReportAndItsOwnOverrides() throws {
    var report = try resultReviewReport()
    var good = try reviewFrame(0)
    var softRejection = try reviewFrame(1, recommended: false)
    let warning = try reviewFrame(2, warning: "localized-transients-use-pixel-rejection")
    good.selected = false
    softRejection.selected = true
    report.frames = [warning, good, softRejection]   // Frame index defines recipe override order.
    report.inputCount = 3
    report.selectedCount = 2
    report.recipe = "PHOTONSTACK_DEEP_SKY_RECIPE 1\nselection.apply \"on\"\nselection.overrides \"reject,keep,auto\"\n"
    var settings = DeepSkySettings()
    settings.overrides = [good.input: "keep", softRejection.input: "reject"]
    let history = DeepSkyReviewSelection(report: report, settings: settings, isCurrent: false)
    #expect(history.decision(good) == "reject" && history.decision(softRejection) == "keep")
    #expect(history.appliesRecommendations == true)
    let old = DeepSkyReview.listing(report.frames, selection: history, filter: .excluded, query: "", sort: .capture)
    #expect(old.frames.map(\.index) == [0])
    #expect(old.counts[.kept] == 2 && old.counts[.manual] == 2)
    let kept = DeepSkyReview.listing(report.frames, selection: history, filter: .kept, query: "", sort: .capture)
    #expect(kept.frames.map(\.index) == [1, 2])
    let current = DeepSkyReviewSelection(report: report, settings: settings, isCurrent: true)
    #expect(DeepSkyReview.listing(report.frames, selection: current, filter: .excluded, query: "", sort: .capture).frames.map(\.index) == [1])
    #expect(DeepSkyReview.initialFilter(report: report, settings: settings, isCurrent: true) == .excluded)
    settings.overrides = [softRejection.input: "keep"]
    #expect(DeepSkyReview.initialFilter(report: report, settings: settings, isCurrent: true) == .all)
    #expect(DeepSkyReview.initialFilter(report: report, settings: settings, isCurrent: false) == .excluded)
    report.recipe = "PHOTONSTACK_DEEP_SKY_RECIPE 1\nselection.apply \"off\"\n"
    let unknown = DeepSkyReviewSelection(report: report, settings: settings, isCurrent: false)
    #expect(unknown.appliesRecommendations == false && unknown.decision(good) == "unknown" && !unknown.kept(good))
    good.reasons = ["user-excluded"]
    #expect(unknown.decision(good) == "reject")
    report.recipe += "selection.overrides \"\"\n"
    #expect(DeepSkyReviewSelection(report: report, settings: settings, isCurrent: false).decision(warning) == "auto")
}

@Test func deepSkyReviewRecoversOnlyMatchingAssessmentSafeguardAndRejectsPlaceholderCounts() throws {
    var report = try resultReviewReport()
    report.success = false
    report.errorCode = "InputChanged"
    report.frames = [try reviewFrame(0, recommended: false, usable: false)]
    report.frames[0].temporalAvailable = false
    report.inputCount = 1
    #expect(!DeepSkyReview.hasRecordedSelection(report))
    #expect(DeepSkyReview.initialFilter(report: report, settings: DeepSkySettings(), isCurrent: false) == .all)
    #expect(!DeepSkyReview.canRecoverSelection(report, recipe: report.recipe))
    report.frames[0].usable = true
    report.errorCode = "SelectionNeedsReview"
    #expect(DeepSkyReview.hasRecordedSelection(report))
    #expect(DeepSkyReview.canRecoverSelection(report, recipe: report.recipe))
    #expect(!DeepSkyReview.canRecoverSelection(report, recipe: report.recipe + "input \"/different.fit\"\n"))
    #expect(!DeepSkyReview.analysisIsCurrent(report, recipe: report.recipe))
}

@Test func deepSkyReviewOpeningScopeResolvesFirstPublishedReportAndPreservesUserChoice() throws {
    var report = try resultReviewReport()
    let good = try reviewFrame(0)
    let rejected = try reviewFrame(1, recommended: false)
    report.frames = [good, rejected]
    report.inputCount = 2
    report.selectedCount = 1
    var settings = DeepSkySettings()
    let opening = DeepSkyReviewOpeningScope(settings: settings)
    #expect(opening.filter(report: nil, isCurrent: false, requested: nil) == .all)
    #expect(opening.filter(report: report, isCurrent: true, requested: nil) == .excluded)
    #expect(opening.filter(report: report, isCurrent: true, requested: .kept) == .kept)
    settings.overrides[rejected.input] = "keep"
    #expect(DeepSkyReview.keptCount(report.frames, settings: settings) == 2)
    #expect(opening.filter(report: report, isCurrent: true, requested: nil) == .excluded)
    let next = DeepSkyReviewOpeningScope(settings: settings)
    #expect(next.filter(report: report, isCurrent: true, requested: nil) == .all)
    #expect(next.filter(report: report, isCurrent: false, requested: nil) == .excluded)
    #expect(next.filter(report: report, isCurrent: false, requested: .all) == .all)
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
    #expect(DeepSkyReview.listing([unknown], settings: DeepSkySettings(), filter: .attention,
                               query: "星形样本不足", sort: .capture).frames.count == 1)
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
    #expect(DeepSkyReview.listing(frames, settings: settings, filter: .attention, query: "", sort: .attention).frames.map(\.index) == [3, 2, 1])
    #expect(DeepSkyReview.listing(frames, settings: settings, filter: .all, query: "局部异常", sort: .capture).frames.map(\.index) == [1])
    #expect(DeepSkyReview.listing(frames, settings: settings, filter: .all, query: "", sort: .noise).frames.first?.index == 1)
    #expect(DeepSkyReview.kept(warning, settings: settings))
}

@Test func deepSkyReviewSearchMatchesDisplayedDecisionsEvidenceAndExactFrameNumbers() throws {
    var settings = DeepSkySettings()
    var rejected = try reviewFrame(6, recommended: false)
    rejected.reasons = ["elongated-stars"]
    let forced = try reviewFrame(69, recommended: false)
    let excluded = try reviewFrame(699)
    let unusable = try reviewFrame(70, usable: false)
    let warning = try reviewFrame(71, warning: "localized-transients-use-pixel-rejection")
    let frames = [rejected, forced, excluded, unusable, warning]
    settings.overrides = [forced.input: "keep", excluded.input: "reject"]
    func found(_ query: String) -> [Int] {
        DeepSkyReview.listing(frames, settings: settings, filter: .all, query: query, sort: .capture).frames.map(\.index)
    }
    for query in ["#7", "7", "星点拉长", "自动排除", "automatically excluded"] { #expect(found(query) == [6]) }
    for query in ["强制保留", "forced keep", " #70 "] { #expect(found(query) == [69]) }
    for query in ["手动排除", "manually excluded"] { #expect(found(query) == [699]) }
    #expect(found("不可用") == [70])
    #expect(found("局部像素异常") == [71])
    #expect(found("localized pixel outliers") == [71])
    #expect(found("#8").isEmpty)
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
    for line in ["denoise.h \"5\"", "denoise.multiscale-luminance \"0.2\"", "denoise.noise-model \"independent-luminance\"", "background.model \"grid\"", "crop.coverage \"0.9\"", "develop.saturation \"0.8\"", "develop.lock-denoise-stretch \"on\""] {
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
    #expect(DeepSkyReview.sameResultRecipe(original + "develop.sky-percentile \"0.5\"\n", original))
    #expect(DeepSkyReview.sameResultRecipe(original + "develop.lock-denoise-stretch \"off\"\n", original))
    let legacyColor = original + "develop.color-transfer \"legacy-code\"\n"
    let linearColor = original + "develop.color-transfer \"linear-light\"\n"
    #expect(DeepSkyReview.sameResultRecipe(legacyColor, original))
    #expect(!DeepSkyReview.sameResultRecipe(linearColor, original))
    #expect(!DeepSkyReview.sameResultRecipe(linearColor, legacyColor))
    #expect(DeepSkyReview.sameAnalysisRecipe(linearColor, original))
    #expect(DeepSkyReview.sameStackRecipe(linearColor, legacyColor))
    #expect(DeepSkyReview.sameResultRecipe(original + "background.polynomial-degree \"2\"\n", original))
    let changedDegree = original + "background.polynomial-degree \"4\"\n"
    #expect(!DeepSkyReview.sameResultRecipe(changedDegree, original))
    #expect(DeepSkyReview.sameAnalysisRecipe(changedDegree, original))
    #expect(DeepSkyReview.sameStackRecipe(changedDegree, original))
    let changedSky = original + "develop.sky-percentile \"0.2\"\n"
    #expect(!DeepSkyReview.sameResultRecipe(changedSky, original))
    #expect(DeepSkyReview.sameAnalysisRecipe(changedSky, original))
    #expect(DeepSkyReview.sameStackRecipe(changedSky, original))
    #expect(DeepSkyReview.sameResultRecipe(original.replacingOccurrences(of: "auto,auto", with: ""), original))
    #expect(!DeepSkyReview.sameResultRecipe(nil, original))
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
@MainActor func deepSkyReviewLargeSequenceActivities() async throws {
    let cli = try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let frames = try (0..<711).map { index in
        var frame = try reviewFrame(index, recommended: index % 9 != 0, usable: index % 17 != 0,
                                    warning: index % 5 == 0 ? "localized-transients-use-pixel-rejection" : nil)
        let url = directory.appendingPathComponent("Light-\(index).fits")
        try Data().write(to: url)
        frame.input = url.path
        return frame
    }
    var settings = DeepSkySettings()
    settings.overrides[frames[1].input] = "reject"
    var project = PhotonStackProject(assets: frames.map {
        PhotonStackAsset(originalURL: URL(fileURLWithPath: $0.input), kind: .fits)
    })
    project.deepSkySettings = settings
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false,
        processingService: CLIProcessingService(executableURL: URL(fileURLWithPath: cli)), previewDirectory: directory)
    await model.loadDeepSkySchema()
    let schema = try #require(model.deepSkySchema)
    let expectedKept = frames.filter { $0.usable && $0.recommendedKeep && $0.index != 1 }.count
    let expectedAttention = frames.filter { !$0.usable || !$0.recommendedKeep || !$0.warnings.isEmpty }.count
    let clock = ContinuousClock()
    for _ in 0..<3 {
        let idle = clock.now
        #expect(!model.journeyAnalysisIsCurrent)
        let recipeStart = clock.now
        let recipe = try settings.recipe(schema: schema, inputs: project.assets.map(\.originalURL))
        #expect(recipe.components(separatedBy: "\ninput ").count - 1 == frames.count)
        let keptStart = clock.now
        let kept = DeepSkyReview.keptCount(frames, settings: model.deepSkySettings)
        #expect(kept == expectedKept)
        let listStart = clock.now
        let listing = DeepSkyReview.listing(frames, settings: settings, filter: .excluded, query: "", sort: .attention)
        let visible = listing.frames, counts = listing.counts
        let end = clock.now
        #expect(visible.count == frames.count - expectedKept)
        #expect(counts[.all] == 711 && counts[.manual] == 1 && counts[.attention] == expectedAttention)
        #expect(visible.first?.index == 0)
        print("Review711 idle=\(idle.duration(to: recipeStart)) recipe=\(recipeStart.duration(to: keptStart)) kept=\(keptStart.duration(to: listStart)) list=\(listStart.duration(to: end))")
    }
}

@Test @MainActor func deepSkyReviewDefersRecipeAndFullResourceEstimateUntilNeeded() throws {
    var evaluations = 0
    func recipe() -> String {
        evaluations += 1
        return "PHOTONSTACK_DEEP_SKY_RECIPE 1\n"
    }

    let directory = URL(fileURLWithPath: "/tmp/working")
    let collapsed = DeepSkyPreflightView.Request(expanded: false, destination: directory, refresh: 0, recipe: recipe())
    let missing = DeepSkyReview.analysisIsCurrent(nil, recipe: recipe())
    var report = try resultReviewReport()
    report.success = false
    let failed = DeepSkyReview.analysisIsCurrent(report, recipe: recipe())
    #expect(collapsed == nil && !missing && !failed)
    #expect(evaluations == 0)

    report.success = true
    let completed = DeepSkyReview.analysisIsCurrent(report, recipe: recipe())
    let expanded = DeepSkyPreflightView.Request(expanded: true, destination: directory, refresh: 0, recipe: recipe())
    #expect(completed)
    let initial = try #require(expanded)
    #expect(evaluations == 2)
    #expect(initial != DeepSkyPreflightView.Request(expanded: true, destination: directory, refresh: 1, recipe: recipe()))
    #expect(initial != DeepSkyPreflightView.Request(expanded: true, destination: directory, refresh: 0, recipe: "changed"))
    #expect(initial != DeepSkyPreflightView.Request(expanded: true, destination: directory.appendingPathComponent("moved"),
                                                  refresh: 0, recipe: recipe()))
}
