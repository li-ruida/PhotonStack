import Foundation
import ImageIO
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

@Test func spectralReviewSettingsRejectInvalidRangesAndIDs() {
    var s = SpectralCalibrationSettings()
    #expect(s.isValid)
    s.skyInner = 9; #expect(!s.isValid)
    s = .init(); s.minSNR = .nan; #expect(!s.isValid)
    s = .init(); s.excluded = ["1", "1"]; #expect(!s.isValid)
    s.excluded = ["bad id"]; #expect(!s.isValid)
    s.excluded = ["Gaia:DR3-1", "a.b_2"]; #expect(s.isValid)
    s.maxValidationLogError = 0; #expect(!s.isValid)
    #expect(spectralStarStatus("expanded-signal-to-noise", chinese: true) == "扩大孔径：信噪比不足")
}

@Test(.enabled(if: ["PHOTONSTACK_TEST_CLI", "PHOTONSTACK_TEST_SPCC_SOURCE", "PHOTONSTACK_TEST_SPCC_INPUT"].allSatisfy { ProcessInfo.processInfo.environment[$0] != nil })) @MainActor
func spectralReviewExclusionsSaveReopenInvalidResultAndSourceGuards() async throws {
    let env = ProcessInfo.processInfo.environment
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_CLI"])))
    let input = URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_SPCC_INPUT"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("SPCC session ' \(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let source = root.appendingPathComponent("source.fits")
    try FileManager.default.copyItem(at: URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_SPCC_SOURCE"])), to: source)
    let original = try Data(contentsOf: source)
    let session = SpectralCalibrationSession(source: source, service: service); defer { session.close() }
    #expect(!session.canReview && !session.isCurrent)
    session.chooseProfile(input.appendingPathComponent("profile.txt")); session.chooseCatalog(input.appendingPathComponent("catalog.txt"))
    #expect(session.canReview)
    try await session.review()
    #expect(session.isCurrent && session.report?.fit.valid == true && session.beforeImage != nil && session.afterImage != nil)
    let first = try #require(session.report), firstOutput = try #require(session.output)
    let science = try Data(contentsOf: firstOutput.appendingPathComponent("calibrated.fits"))
    // The UI's decoder accepts only internally consistent evidence.
    let json = try Data(contentsOf: firstOutput.appendingPathComponent("report.json"))
    var inconsistent = try #require(JSONSerialization.jsonObject(with: json) as? [String: Any])
    var fit = try #require(inconsistent["fit"] as? [String: Any]); fit["training"] = 0; inconsistent["fit"] = fit
    let broken = try JSONDecoder().decode(SpectralCalibrationReport.self, from: JSONSerialization.data(withJSONObject: inconsistent))
    #expect(!broken.isValid)
    let excludedID = try #require(first.stars.first(where: { $0.status == "fit" })?.id)
    session.exclude(excludedID, true)
    #expect(!session.isCurrent)
    do { _ = try await session.save(to: root.appendingPathComponent("stale")); Issue.record("Saved stale exclusions") } catch { }
    #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("stale").path))
    try await session.review()
    #expect(session.report?.stars.first(where: { $0.id == excludedID })?.status == "excluded")
    #expect(session.isCurrent && session.report?.fit.valid == true)
    let saved = root.appendingPathComponent("saved")
    let calibrated = try #require(try await session.save(to: saved))
    let expected = try Data(contentsOf: calibrated)
    #expect(expected == (try Data(contentsOf: session.output!.appendingPathComponent("calibrated.fits"))))
    // The main canvas must use the verified comparison display, while keeping
    // the scientific FITS separate. Generic stretching changes its black point.
    let model = PhotonStackWorkspaceModel(autosaveEnabled: false, processingService: service, previewDirectory: root)
    let previews = DeepSkyReviewPreviewStore()
    let display = try await previews.load(calibrated.path, model: model)
    let displaySource = saved.appendingPathComponent("after.tiff")
    let displayBytes = try Data(contentsOf: displaySource)
    let fullReference = root.appendingPathComponent("full-display.tiff")
    _ = try await service.runCLI(arguments: ["spcc-preview", "--recipe", saved.appendingPathComponent("recipe.txt").path,
                                           "--output", fullReference.path, "--full-resolution", "1"])
    #expect(try Data(contentsOf: display) == Data(contentsOf: fullReference))
    let displayImage = try #require(CGImageSourceCreateWithURL(display as CFURL, nil))
    let properties = try #require(CGImageSourceCopyPropertiesAtIndex(displayImage, 0, nil) as? [String: Any])
    #expect(properties[kCGImagePropertyPixelWidth as String] as? Int == first.width)
    #expect(properties[kCGImagePropertyPixelHeight as String] as? Int == first.height)
    #expect(model.currentInputURL == nil && model.project.assets.isEmpty)
    #expect(previews.cachedURL(calibrated.path, model: model) == display)
    try (displayBytes + Data([0])).write(to: displaySource)
    #expect(previews.cachedURL(calibrated.path, model: model) == nil)
    do { _ = try await previews.load(calibrated.path, model: model); Issue.record("Changed display product accepted") } catch { }
    try displayBytes.write(to: displaySource)
    #expect(try Data(contentsOf: calibrated) == expected)
    do { _ = try await session.save(to: saved); Issue.record("Overwrote existing review") } catch { }
    #expect(try Data(contentsOf: calibrated) == expected)
    let reopened = SpectralCalibrationSession(source: source, service: service); defer { reopened.close() }
    try await reopened.open(saved)
    #expect(reopened.isCurrent && reopened.settings == session.settings && reopened.afterImage != nil)
    let again = root.appendingPathComponent("again")
    _ = try await reopened.save(to: again)
    for p in try FileManager.default.contentsOfDirectory(at: saved, includingPropertiesForKeys: nil) {
        #expect(try Data(contentsOf: p) == Data(contentsOf: again.appendingPathComponent(p.lastPathComponent)))
    }
    let exported = root.appendingPathComponent("exported.fits")
    try await reopened.export(to: exported); #expect(try Data(contentsOf: exported) == expected)
    // Reopen recovers the full quality configuration, not just aperture controls.
    reopened.settings.maxValidationLogError = 0.19
    try await reopened.review(); #expect(reopened.report?.options.maxValidationLogError == 0.19)
    reopened.chooseProfile(input.appendingPathComponent("profile.txt")); #expect(!reopened.isCurrent)
    // A quality failure is reviewable and replayable, never an exportable image.
    session.settings.excluded = first.stars.map(\.id).sorted()
    try await session.review()
    #expect(session.isCurrent && session.report?.fit.valid == false && session.beforeImage != nil && session.afterImage == nil)
    let rejected = root.appendingPathComponent("rejected")
    #expect(try await session.save(to: rejected) == nil)
    try await reopened.open(rejected)
    #expect(reopened.isCurrent && reopened.afterImage == nil && reopened.report?.fit.valid == false)
    do { try await reopened.export(to: root.appendingPathComponent("bad.fits")); Issue.record("Exported rejected calibration") } catch { }
    #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("bad.fits").path))
    session.settings.excluded = []; try await session.review()
    #expect(try Data(contentsOf: session.output!.appendingPathComponent("calibrated.fits")) == science)
    #expect(try Data(contentsOf: source) == original)
    var modified = original; modified[modified.count-1] ^= 1; try modified.write(to: source)
    do { try await session.review(); Issue.record("Changed source accepted") } catch { }
    do { _ = try await session.save(to: root.appendingPathComponent("changed")); Issue.record("Changed source saved") } catch { }
    do { try await session.export(to: root.appendingPathComponent("changed.fits")); Issue.record("Changed source exported") } catch { }
    #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("changed").path))
    #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("changed.fits").path))
}
