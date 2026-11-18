import CryptoKit
import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

private func libraryFixture() -> Data {
    var text = "PhotonStack-SpectralProfile-1\n\"Test profile\" \"broadband\" \"synthetic\" \"Analytic response\" \"CC0\"\n"
    for low in [600, 500, 400] { text += "1\n2\n\(low) 1\n\(low+100) 1\n" }
    text += "\"Flat white\" \"Analytic constant F_lambda\" \"CC0\"\n2\n350 2\n750 2\n"
    return Data(text.utf8)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func spectralProfileLibraryCopiesDeduplicatesExportsArchivesAndRejectsChanges() async throws {
    let cli = try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: cli))
    let fm = FileManager.default, root = fm.temporaryDirectory.appendingPathComponent("Spectral library ' \(UUID())")
    try fm.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? fm.removeItem(at: root) }
    let source = root.appendingPathComponent("profile.txt"), bytes = libraryFixture()
    let license = root.appendingPathComponent("response-source-license.txt"), licenseBytes = Data("Independent test license\n".utf8)
    try bytes.write(to: source); try licenseBytes.write(to: license)
    let repository = root.appendingPathComponent("Library"), library = SpectralProfileLibrary(root: repository, service: service)
    let otherWindow = SpectralProfileLibrary(root: repository, service: service)
    async let one = library.importProfile(source)
    async let two = otherWindow.importProfile(source)
    let (a, b) = try await (one, two)
    #expect(a == b)
    var entries = try await library.list(); #expect(entries.count == 1)
    let entry = try #require(entries.first)
    let selected = try await library.select(entry)
    #expect(selected.inspection.profile.name == "Test profile" && selected.licenseIncluded)
    for (band, expected) in zip(selected.inspection.bands, [650.0, 550.0, 450.0]) { #expect(abs(band.meanWavelength - expected) < 1e-9) }
    #expect(try Data(contentsOf: selected.url) == bytes)
    try await library.rename(entry, label: "Night 2 / OSC")
    entries = try await otherWindow.list(); #expect(entries.first?.label == "Night 2 / OSC")
    #expect(try Data(contentsOf: selected.url) == bytes)
    try fm.removeItem(at: source); try fm.removeItem(at: license)
    let exported = root.appendingPathComponent("exported")
    try await otherWindow.export(entry, to: exported)
    #expect(try Data(contentsOf: exported.appendingPathComponent("profile.txt")) == bytes)
    #expect(try Data(contentsOf: exported.appendingPathComponent("response-source-license.txt")) == licenseBytes)
    do { try await library.export(entry, to: exported); Issue.record("Existing export replaced") } catch { }
    #expect(try Data(contentsOf: exported.appendingPathComponent("profile.txt")) == bytes)
    try await library.archive(entry)
    #expect(try await library.list().isEmpty)
    let archived = try #require(try await otherWindow.list(archived: true).first)
    #expect(try await library.select(archived).inspection.profile.sha == selected.inspection.profile.sha)
    try await library.archive(archived)
    #expect(try await library.list(archived: true).isEmpty)
    // Identical names with different scientific content must never collapse.
    let changed = Data(String(decoding: bytes, as: UTF8.self).replacingOccurrences(of: "600 1", with: "600 0.5").utf8)
    try changed.write(to: source)
    let different = try await library.importProfile(source)
    #expect(different != a)
    #expect(try await library.list().count == 2)
    // Changed scientific bytes and changed license text both fail closed.
    try changed.write(to: selected.url)
    do { _ = try await library.select(entry); Issue.record("Modified profile accepted") } catch { }
    try bytes.write(to: selected.url)
    let storedLicense = selected.url.deletingLastPathComponent().appendingPathComponent("response-source-license.txt")
    try Data("changed".utf8).write(to: storedLicense)
    do { _ = try await library.select(entry); Issue.record("Modified license accepted") } catch { }
    try licenseBytes.write(to: storedLicense)
    // Library imports and exports never mutate source images or source profiles.
    #expect(try Data(contentsOf: source) == changed)
    #expect(try fm.contentsOfDirectory(atPath: repository.path).allSatisfy { !$0.hasPrefix(".import-") })
    #expect(try fm.contentsOfDirectory(atPath: root.path).allSatisfy { !$0.hasPrefix(".spectral-export-") })
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func spectralProfileLibraryRejectsInvalidInputsAndKeepsRecoverableRecords() async throws {
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let fm = FileManager.default, root = fm.temporaryDirectory.appendingPathComponent("Profile recovery \(UUID())")
    try fm.createDirectory(at: root, withIntermediateDirectories: true); defer { try? fm.removeItem(at: root) }
    let source = root.appendingPathComponent("profile.txt"), repository = root.appendingPathComponent("Library")
    let library = SpectralProfileLibrary(root: repository, service: service)
    try Data("invalid".utf8).write(to: source)
    do { _ = try await library.importProfile(source); Issue.record("Invalid profile imported") } catch { }
    #expect(try await library.list().isEmpty)
    try Data(String(decoding: libraryFixture(), as: UTF8.self).replacingOccurrences(of: "Test profile", with: "   ").utf8).write(to: source)
    do { _ = try await library.importProfile(source); Issue.record("Blank profile label imported") } catch { }
    #expect(try await library.list().isEmpty)
    try libraryFixture().write(to: source)
    let link = root.appendingPathComponent("link.txt")
    try fm.createSymbolicLink(at: link, withDestinationURL: source)
    do { _ = try await library.importProfile(link); Issue.record("Linked input imported without a snapshot") } catch { }
    let id = try await library.importProfile(source)
    let entry = try #require(try await library.list().first)
    try await library.archive(entry)
    _ = try await library.importProfile(source)
    let archived = try #require(try await library.list(archived: true).first)
    do { try await library.archive(archived); Issue.record("Restored over an existing entry") } catch { }
    #expect(try await library.list().count == 1)
    #expect(try await library.list(archived: true).count == 1)
    let record = repository.appendingPathComponent(id).appendingPathComponent("entry.json")
    try Data("bad receipt".utf8).write(to: record)
    let damaged = try #require(try await library.list().first)
    #expect(damaged.issue != nil)
    do { _ = try await library.select(damaged); Issue.record("Damaged receipt used") } catch { }
    do { _ = try await library.select(.init(id: "../escape", label: "invalid", archived: false)); Issue.record("Invalid identity used") } catch { }
    // Existing invalid content is not overwritten by a reimport.
    do { _ = try await library.importProfile(source); Issue.record("Damaged entry overwritten") } catch { }
    #expect(try Data(contentsOf: record) == Data("bad receipt".utf8))
}
