import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

@Test(.enabled(if: ["PHOTONSTACK_TEST_CLI", "PHOTONSTACK_TEST_ANNOTATION_SOURCE"].allSatisfy { ProcessInfo.processInfo.environment[$0] != nil })) @MainActor
func annotationReviewFilterExportReopenAndSourceBinding() async throws {
    let env = ProcessInfo.processInfo.environment
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_CLI"])))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("AnnotationTest-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let source = root.appendingPathComponent("source.fits")
    try FileManager.default.copyItem(at: URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_ANNOTATION_SOURCE"])), to: source)
    let original = try Data(contentsOf: source)
    let session = SkyAnnotationSession(source: source, service: service); defer { session.close() }
    try await session.annotate()
    #expect(session.report?.valid == true && session.image != nil && session.isCurrent)
    #expect(session.report?.objects.contains { $0.id == "NGC6992" } == true)
    session.settings.query = "NGC6992"
    do { _ = try await session.save(to: root.appendingPathComponent("stale")); Issue.record("Saved stale settings") } catch { }
    #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("stale").path))
    try await session.annotate()
    #expect(session.report?.objects.map(\.id) == ["NGC6992"])
    let folder = root.appendingPathComponent("saved")
    let svg = try await session.save(to: folder)
    #expect(svg.lastPathComponent == "annotated.svg")
    let originalExport = try Data(contentsOf: svg)
    #expect(originalExport == (try Data(contentsOf: session.output!.appendingPathComponent("annotated.svg"))))
    do { _ = try await session.save(to: folder); Issue.record("Existing directory overwritten") } catch { }
    #expect(try Data(contentsOf: svg) == originalExport)
    let reopened = SkyAnnotationSession(source: source, service: service); defer { reopened.close() }
    try await reopened.open(folder)
    #expect(reopened.isCurrent && reopened.settings == session.settings && reopened.image != nil)
    let again = try await reopened.save(to: root.appendingPathComponent("again"))
    #expect(try Data(contentsOf: again) == originalExport)
    #expect(try Data(contentsOf: source) == original)
    var modified = original; modified[modified.count-1] ^= 1; try modified.write(to: source)
    do { _ = try await reopened.save(to: root.appendingPathComponent("changed")); Issue.record("Changed source accepted") } catch { }
    #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("changed").path))
}
