import AppKit
import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func backgroundReviewRecipeRoundTripAndSourceProtection() async throws {
    let cli = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))
    let service = CLIProcessingService(executableURL: cli)
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("BackgroundReviewTest-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let input = root.appendingPathComponent("input.tiff")
    let bitmap = try #require(NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 160, pixelsHigh: 120,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
        bytesPerRow: 640, bitsPerPixel: 32))
    let bytes = try #require(bitmap.bitmapData)
    for y in 0..<120 { for x in 0..<160 {
        for c in 0..<3 { bytes[(y*160+x)*4+c] = UInt8(20+c*20+x/3+y/4) }
        bytes[(y*160+x)*4+3] = 255
    }}
    let original = try #require(bitmap.representation(using: .tiff, properties: [:]))
    try original.write(to: input)
    let session = BackgroundReviewSession(source: input, service: service)
    defer { session.close() }
    try await session.evaluate(inspect: true)
    #expect(session.recipe?.samples.count == 96)
    #expect(session.image != nil)
    #expect(!session.isCurrent)
    session.tap(x: 80, y: 60, tolerance: 1)
    #expect(session.recipe?.samples.count == 97)
    session.editMode = "remove"
    session.tap(x: 80, y: 60, tolerance: 1)
    #expect(session.recipe?.samples.count == 96)
    session.recipe?.samples[20].enabled = false
    session.recipe?.strength = 0.6
    session.recipe?.model = "rbf"
    session.recipe?.rbfSmoothing = 0.002
    session.recipe?.rbfRejectOutliers = false
    let inspectionDirectory = try #require(session.output)
    try await session.evaluate(inspect: false)
    #expect(!FileManager.default.fileExists(atPath: inspectionDirectory.path))
    #expect(session.isCurrent)
    #expect(session.report?.model == "rbf")
    #expect(session.report?.validationRMS?.count == 3)
    let rendered = try #require(session.output)
    let expected = try Data(contentsOf: rendered.appendingPathComponent("corrected.fits"))
    let recipeFile = root.appendingPathComponent("recipe.json")
    try session.saveRecipe(to: recipeFile)
    let reopened = BackgroundReviewSession(source: input, service: service)
    defer { reopened.close() }
    try reopened.loadRecipe(recipeFile)
    #expect(reopened.recipe == session.recipe)
    try await reopened.evaluate(inspect: false)
    #expect(try Data(contentsOf: try #require(reopened.output).appendingPathComponent("corrected.fits")) == expected)
    let saved = try await reopened.saveResult(to: root.appendingPathComponent("saved"))
    #expect(try Data(contentsOf: saved) == expected)
    do {
        _ = try await reopened.saveResult(to: root.appendingPathComponent("saved"))
        Issue.record("Existing result must not be replaced")
    } catch {}
    #expect(try Data(contentsOf: saved) == expected)
    reopened.recipe?.strength = 0.1
    #expect(!reopened.isCurrent)
    do { _ = try await reopened.saveResult(to: root.appendingPathComponent("stale")); Issue.record("Stale preview must not be exported") } catch {}
    #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("stale").path))
    #expect(try Data(contentsOf: input) == original)
    var changed = original; changed[changed.count-1] ^= 1; try changed.write(to: input)
    do { try await reopened.evaluate(inspect: false); Issue.record("Changed source must be rejected") } catch {}
    #expect(!reopened.isCurrent)
}

@Test @MainActor
func backgroundReviewRecipeRejectsInvalidCoordinates() throws {
    let source = URL(fileURLWithPath: "/tmp/source.fits")
    var recipe = BackgroundReviewRecipe(source: source, sourceSHA: String(repeating: "a", count: 64),
        width: 160, height: 120, samples: [.init(x: 30, y: 30)])
    #expect(recipe.isValid)
    recipe.samples[0].x = 160
    #expect(!recipe.isValid)
    recipe.samples[0].x = 30
    recipe.strength = .nan
    #expect(!recipe.isValid)
    recipe.strength = 0.3
    recipe.samples[0].radius = 65
    #expect(!recipe.isValid)
}

@Test @MainActor
func backgroundPointBindingsSurviveDeletionAndDoNotRetarget() {
    let session = BackgroundReviewSession(source: URL(fileURLWithPath: "/tmp/sample.fits"))
    let a = BackgroundReviewPoint(x: 20, y: 30), b = BackgroundReviewPoint(x: 80, y: 70)
    session.recipe = BackgroundReviewRecipe(source: session.source, sourceSHA: String(repeating: "a", count: 64),
        width: 160, height: 120, samples: [a, b])
    let field = backgroundPointBinding(session, point: a, key: \.x)
    field.wrappedValue = 25
    #expect(session.recipe?.samples[0].x == 25)
    session.removePoint(id: a.id)
    #expect(field.wrappedValue == 20)
    field.wrappedValue = 99
    #expect(session.recipe?.samples == [b])
    session.resetGrid()
    field.wrappedValue = 99
    #expect(session.recipe?.samples.first?.x != 99)
}

@Test @MainActor
func backgroundReviewLegacyPolynomialRecipeMigratesWithoutChangingItsSettings() throws {
    let recipe = BackgroundReviewRecipe(source: URL(fileURLWithPath: "/tmp/old-master.fits"),
        sourceSHA: String(repeating: "b", count: 64), width: 200, height: 180,
        degree: 3, strength: 0.63, preserveBrightness: false, samples: [.init(x: 20, y: 30)])
    let encoded = try JSONEncoder().encode(recipe)
    var legacy = try #require(JSONSerialization.jsonObject(with: encoded) as? [String: Any])
    legacy["version"] = 1
    for name in ["model", "rbfSmoothing", "rbfRejectOutliers"] { legacy.removeValue(forKey: name) }
    let restored = try JSONDecoder().decode(BackgroundReviewRecipe.self, from: JSONSerialization.data(withJSONObject: legacy))
    #expect(restored == recipe)
    #expect(restored.version == 2 && restored.model == "polynomial")
    var invalid = legacy; invalid["model"] = "rbf"
    do {
        _ = try JSONDecoder().decode(BackgroundReviewRecipe.self, from: JSONSerialization.data(withJSONObject: invalid))
        Issue.record("Version 1 must not silently apply an RBF recipe as polynomial")
    } catch {}
    invalid = legacy; invalid["version"] = 2
    do {
        _ = try JSONDecoder().decode(BackgroundReviewRecipe.self, from: JSONSerialization.data(withJSONObject: invalid))
        Issue.record("Version 2 requires explicit model settings")
    } catch {}
}

@Test @MainActor
func backgroundRBFRecipeRejectsUnknownModelAndInvalidSmoothing() {
    var recipe = BackgroundReviewRecipe(source: URL(fileURLWithPath: "/tmp/source.fits"),
        sourceSHA: String(repeating: "a", count: 64), width: 160, height: 120, samples: [])
    recipe.model = "rbf"; recipe.rbfSmoothing = 0
    #expect(recipe.isValid)
    recipe.rbfSmoothing = .nan
    #expect(!recipe.isValid)
    recipe.rbfSmoothing = 1.01
    #expect(!recipe.isValid)
    recipe.rbfSmoothing = 0.001; recipe.model = "unknown"
    #expect(!recipe.isValid)
}
