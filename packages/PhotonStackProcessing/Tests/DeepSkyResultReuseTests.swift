import Foundation
import PhotonStackAppCore
import Testing
@testable import PhotonStackProcessing

@Test func deepSkyReuseParsesQuotedCalibrationPathsWithoutLosingEscapes() throws {
    #expect(try DeepSkyResultReuse.quotedPaths(#""/tmp/dark one.fit" "/tmp/quoted\"dark.fit" "/tmp/back\\slash.fit""#)
        == ["/tmp/dark one.fit", "/tmp/quoted\"dark.fit", "/tmp/back\\slash.fit"])
    #expect(throws: DeepSkyRecipeError.self) { try DeepSkyResultReuse.quotedPaths("unquoted.fit") }
    #expect(throws: DeepSkyRecipeError.self) { try DeepSkyResultReuse.quotedPaths("\"unclosed.fit") }
}

@Test func deepSkyReuseRejectsChangedContentEvenWithRestoredModificationDate() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackReuse-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let light = root.appendingPathComponent("light.fits"), dark = root.appendingPathComponent("dark.fits")
    let engine = root.appendingPathComponent("engine"), output = root.appendingPathComponent("developed.tiff")
    let reportURL = root.appendingPathComponent("app-report.json")
    for url in [light, dark, engine, output] { try Data("original".utf8).write(to: url) }
    let recipe = "PHOTONSTACK_DEEP_SKY_RECIPE 1\n"
    let inspected = try JSONDecoder().decode(DeepSkyInspectedRecipe.self, from: JSONSerialization.data(withJSONObject: [
        "version": 1, "inputs": [light.path], "recipe": recipe,
        "values": ["calibration.darks": "\"\(dark.path)\""]
    ]))
    let report = try JSONDecoder().decode(DeepSkyReport.self, from: JSONSerialization.data(withJSONObject: [
        "version": 1, "success": true, "analysisOnly": false, "inputCount": 0, "selectedCount": 0, "referenceIndex": 0,
        "recipe": recipe, "errorCode": "", "message": "", "master": "", "backgroundMaster": "", "developed": output.path,
        "rejectionLow": "", "rejectionHigh": "", "frames": []
    ]))
    let prepared = try await DeepSkyResultReuse.prepare(inspected, buildIdentity: "build-a", engine: engine)
    try JSONEncoder().encode(report).write(to: reportURL)
    try await DeepSkyResultReuse.publish(prepared, report: report, reportURL: reportURL)
    func reused(recipe expected: String? = nil, build: String = "build-a") async throws -> URL? {
        try await DeepSkyResultReuse.validatedOutput(reportURL: reportURL, recipe: expected ?? recipe, output: output, buildIdentity: build, engine: engine)
    }
    #expect(try await reused() == output)
    let engineAlias = root.appendingPathComponent("engine-alias")
    try FileManager.default.createSymbolicLink(at: engineAlias, withDestinationURL: engine)
    #expect(try await DeepSkyResultReuse.validatedOutput(reportURL: reportURL, recipe: recipe,
        output: output, buildIdentity: "build-a", engine: engineAlias) == output)
    let verifiedSources = try #require(try await DeepSkyResultReuse.validateSourcesForRefinish(report: report, reportURL: reportURL))
    let activeReceipt = root.appendingPathComponent(DeepSkyResultReuse.receiptFileName)
    let receiptBytes = try Data(contentsOf: activeReceipt)
    for downgrade in [false, true] {
        if downgrade { try Data(#"{"version":1}"#.utf8).write(to: activeReceipt) }
        else { try FileManager.default.removeItem(at: activeReceipt) }
        try Data("modified".utf8).write(to: light)
        do {
            try await DeepSkyResultReuse.revalidateSourcesForRefinish(verifiedSources, reportURL: reportURL)
            Issue.record("Removing or downgrading a receipt must not weaken the captured source check")
        } catch DeepSkyRecipeError.inputsChanged { }
        try Data("original".utf8).write(to: light)
        try await DeepSkyResultReuse.revalidateSourcesForRefinish(verifiedSources, reportURL: reportURL)
        try receiptBytes.write(to: activeReceipt)
    }
    #expect(try await reused(build: "build-b") == nil)
    #expect(try await reused(recipe: recipe + "changed") == nil)
    for url in [light, dark, engine, output, reportURL] {
        let original = try Data(contentsOf: url)
        let date = try #require(FileManager.default.attributesOfItem(atPath: url.path)[.modificationDate] as? Date)
        var changed = original; changed[0] ^= 1
        try changed.write(to: url)
        try FileManager.default.setAttributes([.modificationDate: date], ofItemAtPath: url.path)
        #expect(try await reused() == nil, "Changed content must invalidate reuse: \(url.lastPathComponent)")
        try original.write(to: url)
        try FileManager.default.setAttributes([.modificationDate: date], ofItemAtPath: url.path)
        #expect(try await reused() == output)
    }
    let receiptURL = root.appendingPathComponent(DeepSkyResultReuse.receiptFileName)
    var historical = try #require(JSONSerialization.jsonObject(with: Data(contentsOf: receiptURL)) as? [String: Any])
    historical["version"] = 1
    historical["sources"] = (historical["sources"] as? [[String: Any]])?.map { source in
        source.filter { $0.key != "resolvedPath" }
    }
    try JSONSerialization.data(withJSONObject: historical).write(to: receiptURL)
    #expect(try await reused() == nil)
    try FileManager.default.removeItem(at: receiptURL)
    #expect(try await reused() == nil)
}

@Test(arguments: ["light", "calibration"])
func deepSkyReuseRejectsRetargetedSourceParentAliases(sourceKind: String) async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackReuseAlias-\(UUID().uuidString)")
    let first = root.appendingPathComponent("first"), second = root.appendingPathComponent("second")
    try FileManager.default.createDirectory(at: first, withIntermediateDirectories: true)
    try FileManager.default.createDirectory(at: second, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    for directory in [first, second] {
        // Equal bytes intentionally prove the resolved-target identity is checked,
        // rather than merely rehashing the old canonical target or new contents.
        for name in ["light.fits", "dark.fits"] { try Data("original".utf8).write(to: directory.appendingPathComponent(name)) }
    }
    let alias = root.appendingPathComponent("current")
    try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: first)
    let light = (sourceKind == "light" ? alias : first).appendingPathComponent("light.fits")
    let dark = (sourceKind == "calibration" ? alias : first).appendingPathComponent("dark.fits")
    let engine = root.appendingPathComponent("engine"), output = root.appendingPathComponent("developed.tiff")
    let reportURL = root.appendingPathComponent("app-report.json")
    for file in [engine, output] { try Data("product".utf8).write(to: file) }
    let recipe = "PHOTONSTACK_DEEP_SKY_RECIPE 1\n"
    let inspected = try JSONDecoder().decode(DeepSkyInspectedRecipe.self, from: JSONSerialization.data(withJSONObject: [
        "version": 1, "inputs": [light.path], "recipe": recipe,
        "values": ["calibration.darks": "\"\(dark.path)\""]
    ]))
    let report = try JSONDecoder().decode(DeepSkyReport.self, from: JSONSerialization.data(withJSONObject: [
        "version": 1, "success": true, "analysisOnly": false, "inputCount": 0, "selectedCount": 0, "referenceIndex": 0,
        "recipe": recipe, "errorCode": "", "message": "", "master": "", "backgroundMaster": "", "developed": output.path,
        "rejectionLow": "", "rejectionHigh": "", "frames": []
    ]))
    let prepared = try await DeepSkyResultReuse.prepare(inspected, buildIdentity: "build", engine: engine)
    try JSONEncoder().encode(report).write(to: reportURL)
    try await DeepSkyResultReuse.publish(prepared, report: report, reportURL: reportURL)
    func reused() async throws -> URL? {
        try await DeepSkyResultReuse.validatedOutput(reportURL: reportURL, recipe: recipe,
            output: output, buildIdentity: "build", engine: engine)
    }
    #expect(try await reused() == output)
    try FileManager.default.removeItem(at: alias)
    try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: second)
    #expect(try await reused() == nil)
    do {
        try await DeepSkyResultReuse.publish(prepared, report: report, reportURL: reportURL)
        Issue.record("Retargeting a source during processing must prevent receipt publication")
    } catch DeepSkyRecipeError.inputsChanged { }
    try FileManager.default.removeItem(at: alias)
    try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: first)
    #expect(try await reused() == output)
}
