import Foundation
import CryptoKit
import ImageIO
import AppKit
import SwiftUI
import PhotonStackAppCore
import PhotonStackProcessing
import Testing
#if canImport(Darwin)
import Darwin
#endif
@testable import PhotonStackUI

private func writeDeepSkyTestLight(_ url: URL, frame: Int, bayer: Bool = false) throws {
    func card(_ key: String, _ value: String) -> String {
        let text = key.padding(toLength: 8, withPad: " ", startingAt: 0) + "= " + value
        return text.padding(toLength: 80, withPad: " ", startingAt: 0)
    }
    var header = card("SIMPLE", "T") + card("BITPIX", "-32") + card("NAXIS", "2") + card("NAXIS1", "64") + card("NAXIS2", "64")
    if bayer { header += card("BAYERPAT", "'GRBG'") }
    header += "END".padding(toLength: 80, withPad: " ", startingAt: 0)
    header = header.padding(toLength: 2880, withPad: " ", startingAt: 0)
    var data = Data(header.utf8)
    for y in 0..<64 {
        for x in 0..<64 {
            let r2 = Double((x - 32) * (x - 32) + (y - 32) * (y - 32))
            let noise = Double((x * 13 + y * 17 + frame * 7) % 19) * 0.1
            let value: Float
            if bayer {
                // Compact undersampled star on a neutral sky. Its dark halo
                // distinguishes guarded Malvar from the original reconstruction.
                let channelGain = x % 2 == y % 2 ? 0.6 : (y % 2 == 0 ? 2.0 : 0.25)
                let dx = Double(x) - 31.35, dy = Double(y) - 32.6
                value = Float(100 + noise + 20000 * channelGain * exp(-(dx * dx + dy * dy) / (2 * 1.2 * 1.2)))
            } else {
                value = Float(100 + Double(frame * 2) + 30 * exp(-r2 / 80) + noise)
            }
            var big = value.bitPattern.bigEndian
            withUnsafeBytes(of: &big) { data.append(contentsOf: $0) }
        }
    }
    data.append(Data(repeating: 0, count: (2880 - data.count % 2880) % 2880))
    try data.write(to: url)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func deepSkyReviewMatchesNewAndLegacyRecipeDecodePixels() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackReviewDecode-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let input = root.appendingPathComponent("light.fits")
    try writeDeepSkyTestLight(input, frame: 0, bayer: true)
    let service = CLIProcessingService(executableURL: cli)
    let model = PhotonStackWorkspaceModel(autosaveEnabled: false, processingService: service, previewDirectory: root)
    let store = DeepSkyReviewPreviewStore()
    func pixels(_ url: URL) throws -> Data {
        let source = try #require(CGImageSourceCreateWithURL(url as CFURL, nil))
        let image = try #require(CGImageSourceCreateImageAtIndex(source, 0, nil))
        return try #require(image.dataProvider?.data) as Data
    }
    func reference(_ arguments: [String]) async throws -> Data {
        let output = root.appendingPathComponent(UUID().uuidString + ".tiff")
        _ = try await service.runCLI(arguments: ["review-preview", "--input", input.path, "--output", output.path] + arguments)
        return try pixels(output)
    }

    // The canvas loads before the processing inspector and its engine schema.
    let fresh = try await store.load(input.path, model: model)
    let initial = try pixels(fresh)
    let guarded = try await reference(["--fits-demosaic", "malvar-guarded"])
    #expect(initial == guarded)
    await model.loadDeepSkySchema()
    #expect(model.deepSkySchema != nil)
    #expect(store.cachedURL(input.path, model: model) == fresh)
    // Even a legacy RAW option outside the decoder range is irrelevant for FITS.
    model.setDeepSkyValue("9", for: "raw.exposure")
    #expect(store.cachedURL(input.path, model: model) == fresh)

    for (channel, value) in [("red", "1.5"), ("green", "0.82"), ("blue", "2.4")] {
        model.setDeepSkyValue(value, for: "decode.cfa-" + channel)
        #expect(store.cachedURL(input.path, model: model) == nil)
        _ = try await store.load(input.path, model: model)
    }
    let adjusted = try pixels(#require(store.cachedURL(input.path, model: model)))
    let manual = try await reference(["--fits-demosaic", "malvar-guarded", "--fits-cfa-gains", "1.5,0.82,2.4"])
    #expect(adjusted == manual)
    #expect(adjusted != initial)

    // The recipe parser and direct CLI callers retain their historical Malvar
    // default. Importing it should take precedence over the new project default.
    let recipe = root.appendingPathComponent("legacy.txt")
    try "PHOTONSTACK_DEEP_SKY_RECIPE 1\ninput \"\(input.path)\"\n".write(to: recipe, atomically: true, encoding: .utf8)
    await model.importDeepSkyRecipe(from: recipe)
    #expect(model.errorMessage == nil)
    #expect(model.deepSkySettings.values["decode.demosaic"] == "malvar")
    let legacy = try await store.load(input.path, model: model)
    let direct = try await reference([])
    #expect(try pixels(legacy) == direct)
    #expect(direct != guarded)
    #expect(legacy != fresh)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_REVIEW_RAW"] != nil
    && ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func deepSkyReviewRAWMatchesFullResolutionDecodeAndStretch() async throws {
    let env = ProcessInfo.processInfo.environment
    let input = URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_REVIEW_RAW"]))
    let cli = URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackRawReview-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let sourceDigest = SHA256.hash(data: try Data(contentsOf: input))
    let service = CLIProcessingService(executableURL: cli)
    let model = PhotonStackWorkspaceModel(autosaveEnabled: false, processingService: service, previewDirectory: root)
    let store = DeepSkyReviewPreviewStore()
    let camera = try await store.load(input.path, model: model)
    let settings = [("raw.white-balance", "manual"), ("raw.temperature", "3800"), ("raw.tint", "18"),
                    ("raw.exposure", "-0.3"), ("raw.black-level", "manual"), ("raw.manual-black", "0.01"),
                    ("raw.demosaic", "high")]
    for (key, value) in settings { model.setDeepSkyValue(value, for: key) }
    #expect(store.cachedURL(input.path, model: model) == nil)
    let reviewed = try await store.load(input.path, model: model)
    model.setDeepSkyValue("0.3", for: "decode.cfa-red")
    #expect(store.cachedURL(input.path, model: model) == reviewed)

    // Independent reference: decode the RAW normally with the recipe controls,
    // then apply the same display transfer as the one-step review preview.
    let converted = root.appendingPathComponent("decoded.tiff")
    let reference = root.appendingPathComponent("stretched.tiff")
    _ = try await service.runCLI(arguments: ["convert", "--input", input.path, "--output", converted.path,
        "--bit-depth", "16", "--color-space", "srgb", "--raw-white-balance", "manual", "--raw-temperature", "3800",
        "--raw-tint", "18", "--raw-exposure-bias", "-0.3", "--raw-black-level", "manual", "--raw-black-value", "0.01",
        "--raw-demosaic", "high", "--raw-linear", "on"])
    _ = try await service.runCLI(arguments: ["stretch", "--input", converted.path, "--output", reference.path,
        "--auto", "--target-background", "0.18"])

    // Compare in the same display color space and bit depth. Normal conversion
    // writes an intermediate TIFF, so allow one 8-bit level of mean difference.
    func sample(_ url: URL) throws -> [UInt8] {
        let source = try #require(CGImageSourceCreateWithURL(url as CFURL, nil))
        let image = try #require(CGImageSourceCreateThumbnailAtIndex(source, 0, [
            kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceThumbnailMaxPixelSize: 256
        ] as CFDictionary))
        let width = image.width, height = image.height
        let colorSpace = try #require(CGColorSpace(name: CGColorSpace.sRGB))
        let context = try #require(CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
            bytesPerRow: width * 4, space: colorSpace, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        let data = try #require(context.data)
        let rgba = data.bindMemory(to: UInt8.self, capacity: width * height * 4)
        return (0..<(width * height * 4)).compactMap { $0 % 4 == 3 ? nil : rgba[$0] }
    }
    let actual = try sample(reviewed), expected = try sample(reference), defaultPixels = try sample(camera)
    try #require(actual.count == expected.count && actual.count == defaultPixels.count && !actual.isEmpty)
    func meanDifference(_ a: [UInt8], _ b: [UInt8]) -> Double {
        Double(zip(a, b).reduce(0) { $0 + abs(Int($1.0) - Int($1.1)) }) / Double(a.count)
    }
    let referenceError = meanDifference(actual, expected)
    let editDifference = meanDifference(actual, defaultPixels)
    print("RAW review comparison: mean RGB difference vs converted reference \(referenceError), vs camera defaults \(editDifference) / 255")
    #expect(referenceError <= 1)
    #expect(editDifference > 1)
    #expect(SHA256.hash(data: try Data(contentsOf: input)) == sourceDigest)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func deepSkyReviewPreviewPreservesOriginalResolutionAndEditorState() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackReviewTest-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let input = root.appendingPathComponent("original.fits")
    try writeDeepSkyTestLight(input, frame: 0, bayer: true)
    let original = try Data(contentsOf: input)
    let model = PhotonStackWorkspaceModel(autosaveEnabled: false, processingService: CLIProcessingService(executableURL: cli), previewDirectory: root)
    var store: DeepSkyReviewPreviewStore? = DeepSkyReviewPreviewStore()
    let preview = try await store!.load(input.path, model: model)
    let cached = try await store!.load(input.path, model: model)
    #expect(cached == preview)
    let source = try #require(CGImageSourceCreateWithURL(preview as CFURL, nil))
    let image = try #require(CGImageSourceCreateImageAtIndex(source, 0, nil))
    #expect(image.width == 64 && image.height == 64)
    #expect(image.colorSpace?.model == .rgb)
    #expect(try Data(contentsOf: input) == original)
    #expect(model.previewURL == nil)
    #expect(model.project.editGraph.operations.isEmpty)
    #expect(image.bitsPerComponent == 8)
    #expect(try FileManager.default.contentsOfDirectory(atPath: preview.deletingLastPathComponent().path).count == 1)
    // A decoder setting or source revision must invalidate the cached preview.
    model.setDeepSkyValue("off", for: "decode.debayer")
    let mono = try await store!.load(input.path, model: model)
    #expect(mono != preview)
    let monoImage = try #require(NativePreviewImageDecoder.decodeCGImage(contentsOf: mono))
    #expect(monoImage.width == 64)
    try writeDeepSkyTestLight(input, frame: 3, bayer: true)
    let revised = try await store!.load(input.path, model: model)
    #expect(revised != mono)
    store = nil
    #expect(!FileManager.default.fileExists(atPath: preview.deletingLastPathComponent().path))
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func deepSkyColorTransferReplaysHistoricalPixelsAndRefinishesExistingMaster() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackColorTransfer-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let inputs = (0..<3).map { root.appendingPathComponent("light-\($0).fits") }
    for (i, url) in inputs.enumerated() { try writeDeepSkyTestLight(url, frame: i, bayer: true) }
    var project = PhotonStackProject(name: "Color transfer replay")
    project.addAssets(from: inputs)
    var settings = DeepSkySettings()
    settings.values = ["registration.mode": "none", "selection.apply": "off", "selection.measure-stars": "off",
                       "background.model": "none", "develop.align-channels": "off", "develop.stellar-balance": "off",
                       "develop.tone-scale": "100", "develop.white-point": "12000"]
    project.deepSkySettings = settings
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false,
        processingService: CLIProcessingService(executableURL: cli), previewDirectory: root.appendingPathComponent("processing"))
    await model.runDeepSkyWorkflow(analyzeOnly: false)
    try #require(model.errorMessage == nil)
    let fresh = try #require(model.deepSkySettings.lastReport)
    let mode = "develop.color-transfer \"linear-light\"\n"
    try #require(fresh.recipe.contains(mode))
    let master = try Data(contentsOf: URL(fileURLWithPath: fresh.master))
    let historical = root.appendingPathComponent("historical.txt")
    try fresh.recipe.replacingOccurrences(of: mode, with: "").write(to: historical, atomically: true, encoding: .utf8)
    await model.importDeepSkyRecipe(from: historical)
    try #require(model.errorMessage == nil && model.deepSkySettings.values["develop.color-transfer"] == "legacy-code")
    try #require(model.canRefinishDeepSkyMaster)
    await model.refinishDeepSkyMaster()
    try #require(model.errorMessage == nil)
    let old = try #require(model.deepSkySettings.lastReport)
    #expect(old.master == fresh.master && old.frames == fresh.frames)
    let oldPixels = try Data(contentsOf: URL(fileURLWithPath: old.developed))
    #expect(try oldPixels != Data(contentsOf: URL(fileURLWithPath: fresh.developed)))
    // Replaying the original text directly on the same scientific master must
    // match App import and refinish, including the historical display transfer.
    let replay = root.appendingPathComponent("replay")
    _ = try await CLIProcessingService(executableURL: cli).runCLI(arguments: [
        "deep-sky", "--recipe", historical.path, "--from-master", fresh.master, "--output-dir", replay.path
    ])
    #expect(try Data(contentsOf: replay.appendingPathComponent("developed.tiff")) == oldPixels)
    model.setDeepSkyValue("linear-light", for: "develop.color-transfer")
    #expect(model.canRefinishDeepSkyMaster)
    await model.refinishDeepSkyMaster()
    try #require(model.errorMessage == nil)
    let restored = try #require(model.deepSkySettings.lastReport)
    #expect(try Data(contentsOf: URL(fileURLWithPath: restored.developed)) == Data(contentsOf: URL(fileURLWithPath: fresh.developed)))
    #expect(try Data(contentsOf: URL(fileURLWithPath: restored.master)) == master)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func deepSkyWorkspaceExportReplaysFullResolutionResult() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackDeepSkyExport-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let inputs = (0..<7).map { root.appendingPathComponent("light-\($0).fits") }
    for (frame, input) in inputs.enumerated() { try writeDeepSkyTestLight(input, frame: frame) }
    let originals = try inputs.map { try Data(contentsOf: $0) }
    var project = PhotonStackProject(name: "Deep sky export regression")
    project.addAssets(from: inputs)
    var settings = DeepSkySettings()
    settings.values = ["registration.mode": "none", "selection.measure-stars": "off", "background.model": "none",
                       "develop.stellar-balance": "off", "develop.tone-scale": "10", "develop.white-point": "200"]
    project.deepSkySettings = settings
    let previews = root.appendingPathComponent("previews")
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false,
        processingService: CLIProcessingService(executableURL: cli), previewDirectory: previews)
    await model.runDeepSkyWorkflow(analyzeOnly: false)
    try #require(model.errorMessage == nil)
    let report = try #require(model.deepSkySettings.lastReport)
    let reportURL = URL(fileURLWithPath: report.developed).deletingLastPathComponent().appendingPathComponent("app-report.json")
    let receiptURL = reportURL.deletingLastPathComponent().appendingPathComponent(DeepSkyResultReuse.receiptFileName)
    #expect(FileManager.default.fileExists(atPath: receiptURL.path))
    let export = root.appendingPathComponent("export.tiff")
    await model.exportCurrentImage(to: export)
    #expect(model.errorMessage == nil)
    let source = try #require(CGImageSourceCreateWithURL(export as CFURL, nil))
    let image = try #require(CGImageSourceCreateImageAtIndex(source, 0, nil))
    #expect(image.width == 64 && image.height == 64)
    // Historical/missing receipts must still take the full replay path that
    // previously failed when output directory URLs lacked a trailing slash.
    try FileManager.default.removeItem(at: receiptURL)
    let rebuilt = root.appendingPathComponent("export-rebuilt.tiff")
    await model.exportCurrentImage(to: rebuilt)
    #expect(model.errorMessage == nil)
    #expect(try Data(contentsOf: rebuilt) == Data(contentsOf: export))
    #expect(model.deepSkySettings.lastReport == report)
    #expect(try !FileManager.default.contentsOfDirectory(atPath: previews.path).contains { $0.hasPrefix("replay-deep-sky-") })
    for (input, original) in zip(inputs, originals) { #expect(try Data(contentsOf: input) == original) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil), arguments: ["app-build", "engine"]) @MainActor
func deepSkyRefinishChecksV2SourceContentAcrossBuilds(change: String) async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackRefinishProvenance-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let inputs = (0..<7).map { root.appendingPathComponent("light-\($0).fits") }
    for (frame, input) in inputs.enumerated() { try writeDeepSkyTestLight(input, frame: frame) }
    var project = PhotonStackProject(name: "Refinish source provenance")
    project.addAssets(from: inputs)
    var settings = DeepSkySettings()
    settings.values = ["registration.mode": "none", "selection.measure-stars": "off", "background.model": "none",
                       "develop.stellar-balance": "off", "develop.tone-scale": "10", "develop.white-point": "200"]
    project.deepSkySettings = settings
    let engineWrapper = root.appendingPathComponent("engine-wrapper")
    let script = "#!/bin/sh\nexec '\(cli.path.replacingOccurrences(of: "'", with: "'\\''"))' \"$@\"\n# initial engine wrapper\n"
    try script.write(to: engineWrapper, atomically: true, encoding: .utf8)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: engineWrapper.path)
    let service = CLIProcessingService(executableURL: engineWrapper)
    let first = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false, processingService: service,
        previewDirectory: root.appendingPathComponent("first"), cacheBuildIdentity: "build-a")
    await first.runDeepSkyWorkflow(analyzeOnly: false)
    try #require(first.errorMessage == nil)
    let previous = try #require(first.deepSkySettings.lastReport)
    if change == "engine" {
        try (script + "# changed engine bytes\n").write(to: engineWrapper, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: engineWrapper.path)
    }
    let upgraded = PhotonStackWorkspaceModel(project: first.project, autosaveEnabled: false, processingService: service,
        previewDirectory: root.appendingPathComponent("upgraded"), cacheBuildIdentity: change == "engine" ? "build-a" : "build-b")
    await upgraded.loadDeepSkySchema()
    upgraded.setDeepSkyValue("1.2", for: "develop.brightness")
    let original = try Data(contentsOf: inputs[0])
    var info = stat()
    try #require(stat(inputs[0].path, &info) == 0)
    let times = [info.st_atimespec, info.st_mtimespec]
    func restoreTimes() throws {
        let status = times.withUnsafeBufferPointer { utimensat(AT_FDCWD, inputs[0].path, $0.baseAddress, 0) }
        try #require(status == 0)
    }
    var changed = original; changed[changed.count - 1] ^= 1
    try changed.write(to: inputs[0]); try restoreTimes()
    var changedInfo = stat()
    try #require(stat(inputs[0].path, &changedInfo) == 0)
    #expect(changedInfo.st_size == info.st_size && changedInfo.st_mtimespec.tv_sec == info.st_mtimespec.tv_sec
            && changedInfo.st_mtimespec.tv_nsec == info.st_mtimespec.tv_nsec)
    await upgraded.refinishDeepSkyMaster()
    #expect(upgraded.errorMessage?.contains("Source files changed") == true)
    #expect(upgraded.deepSkySettings.lastReport == previous)
    try original.write(to: inputs[0]); try restoreTimes()
    // Refinish depends on the verified master/source evidence, not the previous
    // application build or existence of an obsolete developed preview.
    try FileManager.default.removeItem(at: URL(fileURLWithPath: previous.developed))
    await upgraded.refinishDeepSkyMaster()
    #expect(upgraded.errorMessage == nil)
    let finished = try #require(upgraded.deepSkySettings.lastReport)
    #expect(finished.masterSHA256 == previous.masterSHA256 && finished.developed != previous.developed)
    let receipt = URL(fileURLWithPath: finished.developed).deletingLastPathComponent().appendingPathComponent(DeepSkyResultReuse.receiptFileName)
    #expect(!FileManager.default.fileExists(atPath: receipt.path))
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func deepSkyReviewPublishesFirstAnalysisIntoMountedSheet() async throws {
    struct MountedReview: View {
        let model: PhotonStackWorkspaceModel
        @State private var scope: DeepSkyReviewFilter?
        var body: some View {
            DeepSkyRecipeSheet(model: model, presented: .constant(true), reviewFilter: $scope, onImport: {})
        }
    }
    func reviewTable(in view: NSView) -> NSTableView? {
        if let table = view as? NSTableView, table.identifier?.rawValue == "review.frames" { return table }
        for child in view.subviews {
            if let table = reviewTable(in: child) { return table }
        }
        return nil
    }
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackMountedReview-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let inputs = (0..<12).map { root.appendingPathComponent("light-\($0).fits") }
    for (i, path) in inputs.enumerated() { try writeDeepSkyTestLight(path, frame: i) }
    var project = PhotonStackProject(name: "Mounted frame review")
    project.addAssets(from: inputs)
    var settings = DeepSkySettings()
    settings.values = ["registration.mode": "none", "selection.apply": "off", "selection.measure-stars": "off"]
    settings.overrides = Dictionary(uniqueKeysWithValues: inputs.prefix(3).map {
        (PhotonStackProject.assetIdentityPath(for: $0), "reject")
    })
    project.deepSkySettings = settings
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false,
        processingService: CLIProcessingService(executableURL: cli), previewDirectory: root.appendingPathComponent("work"))
    _ = NSApplication.shared
    let host = NSHostingView(rootView: MountedReview(model: model))
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1080, height: 760),
                          styleMask: .borderless, backing: .buffered, defer: false)
    window.isReleasedWhenClosed = false
    window.contentView = host
    defer { window.contentView = nil; window.close() }
    host.layoutSubtreeIfNeeded()
    #expect(reviewTable(in: host) == nil)
    await model.runDeepSkyWorkflow(analyzeOnly: true)
    #expect(model.deepSkySettings.lastReport?.selectedCount == 9)
    // Exercise the native list after SwiftUI commits the new report and filter.
    let clock = ContinuousClock()
    let deadline = clock.now + .seconds(3)
    while clock.now < deadline {
        host.layoutSubtreeIfNeeded()
        if reviewTable(in: host)?.numberOfRows == 3 { break }
        try await Task.sleep(for: .milliseconds(20))
    }
    #expect(reviewTable(in: host)?.numberOfRows == 3)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func deepSkyAppWorkflowMatchesCLIAndReopensRecipe() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackDeepSkyAppTest-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    var project = PhotonStackProject(name: "Deep sky parity test")
    let inputs = (0..<7).map { root.appendingPathComponent("原片 \($0).fits") }
    for (frame, input) in inputs.enumerated() { try writeDeepSkyTestLight(input, frame: frame) }
    let originals = try inputs.map { try Data(contentsOf: $0) }
    project.addAssets(from: inputs)
    var settings = DeepSkySettings()
    settings.values = ["registration.mode": "none", "selection.measure-stars": "off", "background.model": "none",
                       "develop.stellar-balance": "off", "develop.tone-scale": "10", "develop.white-point": "200"]
    project.deepSkySettings = settings
    let service = CLIProcessingService(executableURL: cli)
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false, processingService: service,
                                        previewDirectory: root.appendingPathComponent("app"))
    await model.runDeepSkyWorkflow(analyzeOnly: true)
    #expect(model.errorMessage == nil)
    #expect(model.deepSkySettings.lastReport?.selectedCount == inputs.count)
    #expect(model.journeyAnalysisIsCurrent)
    #expect(!model.journeyResultIsCurrent)
    #expect(model.journeyKeptCount == inputs.count)
    await model.runDeepSkyWorkflow(analyzeOnly: false)
    #expect(model.errorMessage == nil)
    let report = try #require(model.deepSkySettings.lastReport)
    #expect(report.success && !report.analysisOnly)
    #expect(model.journeyResultIsCurrent)
    #expect(report.crop?.width == 64 && report.crop?.height == 64)
    #expect(report.masterSHA256?.count == 64)
    #expect(report.developedSHA256?.count == 64)
    let operation = try #require(model.project.editGraph.operations.last)
    #expect(operation.parameters["mode"] == "deepSkyRecipe")
    #expect(operation.parameters["recipe"] == report.recipe)
    #expect(operation.parameters["report"]?.hasSuffix("app-report.json") == true)
    #expect(model.previewURL?.path == report.developed)
    let recipeURL = root.appendingPathComponent("cli-recipe.txt")
    try report.recipe.write(to: recipeURL, atomically: true, encoding: .utf8)
    let cliReport = try await service.deepSky(recipe: recipeURL, outputDirectory: root.appendingPathComponent("cli"), analyzeOnly: false)
    #expect(try Data(contentsOf: URL(fileURLWithPath: report.master)) == Data(contentsOf: URL(fileURLWithPath: cliReport.master)))
    #expect(try Data(contentsOf: URL(fileURLWithPath: report.developed)) == Data(contentsOf: URL(fileURLWithPath: cliReport.developed)))
    // On-disk report paths must survive the App processing service's output handling.
    let stored = try JSONDecoder().decode(DeepSkyReport.self, from: Data(contentsOf: URL(fileURLWithPath: report.master).deletingLastPathComponent().appendingPathComponent("report.json")))
    #expect(stored.master == report.master)
    let masterBytes = try Data(contentsOf: URL(fileURLWithPath: report.master))
    #expect(report.denoiseStretchLocked == false)
    model.setDeepSkyValue("on", for: "develop.lock-denoise-stretch")
    #expect(model.journeyAnalysisIsCurrent && !model.journeyResultIsCurrent && model.canRefinishDeepSkyMaster)
    model.setDeepSkyValue("off", for: "develop.lock-denoise-stretch")
    #expect(model.journeyResultIsCurrent)
    model.setDeepSkyValue("on", for: "develop.lock-denoise-stretch")
    model.setDeepSkyValue("0.9", for: "denoise.multiscale-chroma")
    #expect(model.journeyAnalysisIsCurrent && !model.journeyResultIsCurrent)
    #expect(model.canRefinishDeepSkyMaster)
    #expect(DeepSkyReview.sameAnalysisRecipe(try model.deepSkyRecipeText(), report.recipe))
    #expect(!DeepSkyReview.sameResultRecipe(try model.deepSkyRecipeText(), report.recipe))
    model.updateDeepSkyOverrides([inputs[0].path: "reject"])
    #expect(model.journeyKeptCount == inputs.count - 1)
    let reportedInput = try #require(model.deepSkySettings.lastReport?.frames.first?.input)
    model.updateDeepSkyOverrides([reportedInput: "auto"])
    #expect(model.journeyKeptCount == inputs.count)
    model.updateDeepSkyOverrides([reportedInput: "reject"])
    #expect(model.journeyKeptCount == inputs.count - 1)
    #expect(!model.canRefinishDeepSkyMaster)
    let blockedJobs = model.jobs.count
    await model.refinishDeepSkyMaster()
    #expect(model.jobs.count == blockedJobs)
    model.updateDeepSkyOverrides([inputs[0].path: "auto"])
    model.setDeepSkyValue("2", for: "crop.inset")
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage == nil)
    let finished = try #require(model.deepSkySettings.lastReport)
    #expect(finished.denoiseStretchLocked == true)
    let savedFinish = try JSONDecoder().decode(DeepSkyReport.self, from: Data(contentsOf:
        URL(fileURLWithPath: finished.developed).deletingLastPathComponent().appendingPathComponent("app-report.json")))
    #expect(savedFinish.denoiseStretchLocked == true)
    #expect(finished.master == report.master && finished.frames == report.frames)
    #expect(DeepSkyReview.sameResultRecipe(try model.deepSkyRecipeText(), finished.recipe))
    #expect(finished.developed != report.developed)
    #expect(finished.masterSHA256 == report.masterSHA256)
    #expect(finished.developedSHA256?.count == 64 && finished.developedSHA256 != report.developedSHA256)
    #expect(finished.crop?.x == 2 && finished.crop?.width == 60)
    let finishedOperation = try #require(model.project.editGraph.operations.last)
    let geometry = try await ResultComparisonProvenanceLoader().load(
        ResultComparisonSources(currentReport: URL(fileURLWithPath: try #require(finishedOperation.parameters["report"])),
                                referenceReport: URL(fileURLWithPath: try #require(operation.parameters["report"]))),
        currentURL: URL(fileURLWithPath: finished.developed), referenceURL: URL(fileURLWithPath: report.developed),
        currentSize: CGSize(width: 60, height: 60), referenceSize: CGSize(width: 64, height: 64))
    #expect(geometry.common == CGRect(x: 2, y: 2, width: 60, height: 60))
    #expect(model.previewURL?.path == finished.developed)
    #expect(try Data(contentsOf: URL(fileURLWithPath: finished.master)) == masterBytes)
    try finished.validate()
    // A file with the same path and length is not necessarily the same master.
    let masterURL = URL(fileURLWithPath: finished.master)
    var changedMaster = masterBytes; changedMaster[changedMaster.count - 1] ^= 1
    try changedMaster.write(to: masterURL)
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage?.contains("Linear master changed") == true)
    #expect(model.deepSkySettings.lastReport == finished)
    try masterBytes.write(to: masterURL)
    var changedSource = originals[0]; changedSource.append(0)
    try changedSource.write(to: inputs[0])
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage?.contains("Source files changed") == true)
    #expect(model.deepSkySettings.lastReport == finished)
    try originals[0].write(to: inputs[0])
    // Re-analysis replaces the report but must not lose access to the completed stack.
    await model.runDeepSkyWorkflow(analyzeOnly: true)
    #expect(model.deepSkySettings.lastReport?.analysisOnly == true)
    #expect(model.latestDeepSkyResultURL?.path == finished.developed)
    let historyBeforeNavigation = model.project.editGraph
    let jobsBeforeNavigation = model.jobs.count
    model.showLatestDeepSkyResult()
    #expect(model.previewURL?.path == finished.developed)
    #expect(model.project.editGraph == historyBeforeNavigation)
    #expect(model.jobs.count == jobsBeforeNavigation)
    let output = URL(fileURLWithPath: finished.developed)
    let hidden = output.appendingPathExtension("temporarily-missing")
    try FileManager.default.moveItem(at: output, to: hidden)
    #expect(model.latestDeepSkyResultURL == nil)
    model.showLatestDeepSkyResult()
    #expect(model.project.editGraph == historyBeforeNavigation)
    try FileManager.default.moveItem(at: hidden, to: output)
    let saved = root.appendingPathComponent("saved-project")
    await model.saveProject(to: saved)
    let reopened = try await ProjectRepository().load(from: saved)
    #expect(reopened.deepSkySettings == model.project.deepSkySettings)
    #expect(reopened.deepSkySettings?.values["develop.lock-denoise-stretch"] == "on")
    for (input, original) in zip(inputs, originals) { #expect(try Data(contentsOf: input) == original) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func deepSkyIndependentNoiseSurvivesReprocessingAndProjectReopen() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackNoiseApp-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let inputs = (0..<12).map { root.appendingPathComponent("light-\($0).fits") }
    for (frame, input) in inputs.enumerated() { try writeDeepSkyTestLight(input, frame: frame) }
    let originals = try inputs.map { try Data(contentsOf: $0) }
    var project = PhotonStackProject(name: "Independent luminance noise integration")
    project.addAssets(from: inputs)
    var settings = DeepSkySettings()
    settings.values = ["stack.method": "sigma", "denoise.noise-model": "independent-luminance", "denoise.h": "0",
        "stack.sigma-low": "20", "stack.sigma-high": "20", "selection.apply": "off",
        "registration.mode": "none", "selection.measure-stars": "off", "develop.stellar-balance": "off",
        "develop.tone-scale": "10", "develop.white-point": "200"]
    project.deepSkySettings = settings
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false,
        processingService: CLIProcessingService(executableURL: cli), previewDirectory: root.appendingPathComponent("processing"))
    await model.runDeepSkyWorkflow(analyzeOnly: false)
    #expect(model.errorMessage == nil)
    let first = try #require(model.deepSkySettings.lastReport)
    try #require(first.success && first.independentLuminanceNoise == true && first.noiseReferencePaths.count == 3)
    let masterBytes = try Data(contentsOf: URL(fileURLWithPath: first.master))
    let noiseBytes = try first.noiseReferencePaths.map { try Data(contentsOf: URL(fileURLWithPath: $0)) }
    #expect(model.canRefinishDeepSkyMaster)
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage == nil)
    #expect(model.deepSkySettings.lastReport?.developedSHA256 == first.developedSHA256)
    model.setDeepSkyValue("scene", for: "denoise.noise-model")
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage == nil)
    #expect(model.deepSkySettings.lastReport?.independentLuminanceNoise == false)
    #expect(model.deepSkySettings.lastReport?.noiseReferencePaths == first.noiseReferencePaths)
    model.setDeepSkyValue("independent-luminance", for: "denoise.noise-model")
    #expect(model.canRefinishDeepSkyMaster)
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage == nil)
    let latest = try #require(model.deepSkySettings.lastReport)
    #expect(latest.developedSHA256 == first.developedSHA256)
    let saved = root.appendingPathComponent("saved")
    await model.saveProject(to: saved)
    let reopened = try await ProjectRepository().load(from: saved)
    #expect(reopened.deepSkySettings?.lastReport?.noiseReferencePaths == first.noiseReferencePaths)
    #expect(try Data(contentsOf: URL(fileURLWithPath: first.master)) == masterBytes)
    for (path, bytes) in zip(first.noiseReferencePaths, noiseBytes) {
        #expect(try Data(contentsOf: URL(fileURLWithPath: path)) == bytes)
    }
    let noiseURL = URL(fileURLWithPath: try #require(first.noiseReference))
    var damaged = noiseBytes[0]; damaged[damaged.count - 1] ^= 1
    try damaged.write(to: noiseURL)
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage != nil && model.deepSkySettings.lastReport == latest)
    for (input, original) in zip(inputs, originals) { #expect(try Data(contentsOf: input) == original) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_SENSOR_FIXTURE"] != nil && ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil), arguments: [false, true]) @MainActor
func deepSkySensorCorrectionRunsThroughAppAndReusesMaster(independentNoise: Bool) async throws {
    // The C++ workflow fixture generator supplies moving CFA stars and a known
    // fixed sensor component. This exercises the real service and App model.
    let fixture = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_SENSOR_FIXTURE"]))
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackSensorApp-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let inputs = try (0..<36).map { index in
        let url = root.appendingPathComponent("light-\(index).fits")
        try FileManager.default.copyItem(at: fixture.appendingPathComponent("\(index).fits"), to: url)
        return url
    }
    let originals = try inputs.map { try Data(contentsOf: $0) }
    var settings = DeepSkySettings()
    settings.values = ["calibration.sensor-pattern": "on", "registration.reference": "0", "registration.minimum-matches": "6",
                       "selection.measure-stars": "off", "selection.measure-trails": "off", "selection.apply": "off",
                       "background.model": "none", "develop.align-channels": "off", "develop.stellar-balance": "off",
                       "develop.tone-scale": "10", "develop.white-point": "700", "crop.coverage": "1",
                       "denoise.multiscale-luminance": "0", "denoise.multiscale-chroma": "0"]
    var project = PhotonStackProject(name: "Sensor correction integration")
    if independentNoise {
        settings.values["stack.method"] = "sigma"
        settings.values["denoise.noise-model"] = "independent-luminance"
        settings.values["denoise.h"] = "0"
        settings.values["denoise.multiscale-luminance"] = "0.05"
    }
    project.addAssets(from: inputs); project.deepSkySettings = settings
    let model = PhotonStackWorkspaceModel(project: project, autosaveEnabled: false,
        processingService: CLIProcessingService(executableURL: cli), previewDirectory: root.appendingPathComponent("processing"))
    await model.runDeepSkyWorkflow(analyzeOnly: false)
    #expect(model.errorMessage == nil)
    let first = try #require(model.deepSkySettings.lastReport)
    #expect(first.success && first.selectedCount == 36)
    #expect(first.frames.allSatisfy { !$0.warnings.contains("stellar-measurements-unavailable") })
    #expect(first.frames.allSatisfy { $0.sha256?.count == 64 && (0...2).contains($0.sensorPatternModel ?? -1) })
    #expect(first.sensorPatternReport?.isEmpty == false)
    if independentNoise { #expect(first.independentLuminanceNoise == true && first.noiseReferencePaths.count == 3) }
    let masterBytes = try Data(contentsOf: URL(fileURLWithPath: first.master))
    #expect(model.canRefinishDeepSkyMaster)
    model.setDeepSkyValue("0.05", for: "denoise.multiscale-luminance")
    await model.refinishDeepSkyMaster()
    #expect(model.errorMessage == nil)
    let finished = try #require(model.deepSkySettings.lastReport)
    #expect(finished.master == first.master && finished.developed != first.developed)
    #expect(finished.frames == first.frames && finished.sensorPatternReport == first.sensorPatternReport)
    #expect(finished.recipe.contains("calibration.sensor-pattern \"on\""))
    #expect(try Data(contentsOf: URL(fileURLWithPath: finished.master)) == masterBytes)
    #expect(try inputs.map { try Data(contentsOf: $0) } == originals)
    try finished.validate()
}
