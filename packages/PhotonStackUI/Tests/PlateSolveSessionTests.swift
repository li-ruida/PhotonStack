import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

@Test(.enabled(if: ["PHOTONSTACK_TEST_CLI", "PHOTONSTACK_TEST_PLATE_SOLVER", "PHOTONSTACK_TEST_PLATE_INDEX", "PHOTONSTACK_TEST_PLATE_SOURCE"].allSatisfy { ProcessInfo.processInfo.environment[$0] != nil })) @MainActor
func plateSolveReviewExportReopenAndSourceBinding() async throws {
    let env = ProcessInfo.processInfo.environment
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_CLI"])))
    let source = URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_PLATE_SOURCE"]))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PlateSolveTest-\(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let copy = root.appendingPathComponent("source.fits")
    try FileManager.default.copyItem(at: source, to: copy)
    let original = try Data(contentsOf: copy)
    let session = PlateSolveSession(source: copy, service: service); defer { session.close() }
    session.settings.solver = try #require(env["PHOTONSTACK_TEST_PLATE_SOLVER"])
    session.settings.indices = [try #require(env["PHOTONSTACK_TEST_PLATE_INDEX"])]
    session.settings.icrsConfirmed = true
    try await session.solve()
    #expect(session.isCurrent && session.image != nil && session.report?.valid == true)
    #expect(session.report!.validation.count >= 6)
    session.settings.timeout += 10
    do { _ = try await session.save(to: root.appendingPathComponent("stale")); Issue.record("Saved stale plate solve") } catch { }
    #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("stale").path))
    session.settings.timeout -= 10
    let folder = root.appendingPathComponent("saved")
    let saved = try await session.save(to: folder)
    #expect(saved == folder.appendingPathComponent("solved.fits"))
    let result = try Data(contentsOf: saved)
    #expect(result == (try Data(contentsOf: session.output!.appendingPathComponent("solved.fits"))))
    do { _ = try await session.save(to: folder); Issue.record("Replaced existing solved master") } catch { }
    #expect(try Data(contentsOf: saved) == result)
    let reopened = PlateSolveSession(source: copy, service: service); defer { reopened.close() }
    try await reopened.open(folder)
    #expect(reopened.isCurrent && reopened.image != nil && reopened.source == copy.resolvingSymlinksInPath())
    let again = try await reopened.save(to: root.appendingPathComponent("again"))
    #expect(try Data(contentsOf: again) == result)
    #expect(try Data(contentsOf: copy) == original)
    var changed = original; changed[changed.count-1] ^= 1; try changed.write(to: copy)
    do { _ = try await reopened.save(to: root.appendingPathComponent("changed")); Issue.record("Changed source accepted") } catch { }
    #expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("changed").path))
}
