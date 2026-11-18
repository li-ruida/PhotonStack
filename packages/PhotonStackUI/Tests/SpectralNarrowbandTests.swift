import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

@Test func spectralNarrowbandMetadataValidationAndOldProfileCompatibility() throws {
    let old = """
    {"name":"Base","mode":"broadband","quality":"measured","provenance":"Test","license":"CC0",
     "sha":"\(String(repeating: "a", count: 64))","whiteName":"White","whiteProvenance":"Test","whiteLicense":"CC0"}
    """
    var profile = try JSONDecoder().decode(SpectralProfileMetadata.self, from: Data(old.utf8))
    #expect(profile.isValid && profile.narrowband == nil)
    let good = SpectralNarrowbandModel(layout: "extracted-hoo", baseSHA: profile.sha, centers: [656.3, 500.7, 500.7], widths: [7, 6, 6])
    #expect(good.isValid)
    profile.narrowband = good; #expect(!profile.isValid)
    profile.mode = "narrowband"; #expect(!profile.isValid)
    profile.quality = "approximate"; #expect(profile.isValid)
    let encoded = try JSONEncoder().encode(profile)
    #expect(try JSONDecoder().decode(SpectralProfileMetadata.self, from: encoded) == profile)
    var invalid = good
    invalid.centers = [656.3]; #expect(!invalid.isValid)
    invalid = good; invalid.widths = [7, 6, .nan]; #expect(!invalid.isValid)
    invalid = good; invalid.widths[2] = 7; #expect(!invalid.isValid)
    invalid = good; invalid.centers[0] = 336; #expect(!invalid.isValid)
    invalid = good; invalid.centers[0] = 500; #expect(!invalid.isValid)
    invalid = good; invalid.model = "measured"; #expect(!invalid.isValid)
    invalid = good; invalid.baseSHA = "bad"; #expect(!invalid.isValid)
    invalid = good; invalid.layout = "unknown"; #expect(!invalid.isValid)
    invalid = good; invalid.layout = "mono"; invalid.centers[2] = 672; #expect(invalid.isValid)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func spectralNarrowbandLibraryCreatesReopensExportsAndProtectsBase() async throws {
    let fm = FileManager.default, root = fm.temporaryDirectory.appendingPathComponent("Narrowband library \(UUID())")
    try fm.createDirectory(at: root, withIntermediateDirectories: false)
    defer { try? fm.removeItem(at: root) }
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    var text = "PhotonStack-SpectralProfile-1\n\"Base\" \"broadband\" \"measured\" \"Analytic test detector\" \"CC0\"\n"
    for c in 0..<3 { text += "1\n2\n350 \(0.2+Double(c)*0.1)\n750 \(0.8-Double(c)*0.1)\n" }
    text += "\"Flat white\" \"Constant F_lambda\" \"CC0\"\n2\n350 2\n750 2\n"
    let source = root.appendingPathComponent("profile.txt"), license = Data("Source license\n".utf8)
    try Data(text.utf8).write(to: source)
    try license.write(to: root.appendingPathComponent("response-source-license.txt"))
    let repository = root.appendingPathComponent("Library")
    let library = SpectralProfileLibrary(root: repository, service: service)
    _ = try await library.importProfile(source)
    let base = try #require(try await library.list().first)
    let selected = try await library.select(base)
    var expected: [String: SpectralProfileMetadata] = [:]
    for layout in ["mono", "osc-dual", "extracted-hoo"] {
        let id = try await library.createNarrowband(from: base, name: layout, layout: layout, centers: [656.3,500.7,500.7], widths: [7,6,6])
        let entry = try #require(try await library.list().first { $0.id == id })
        let s = try await library.select(entry)
        #expect(s.inspection.profile.mode == "narrowband" && s.inspection.profile.quality == "approximate")
        #expect(s.inspection.profile.narrowband?.layout == layout)
        #expect(s.inspection.profile.narrowband?.baseSHA == selected.inspection.profile.sha)
        #expect(s.licenseIncluded)
        expected[id] = s.inspection.profile
        let output = root.appendingPathComponent(layout)
        try await library.export(entry, to: output)
        #expect(try Data(contentsOf: output.appendingPathComponent("profile.txt")) == Data(contentsOf: s.url))
        #expect(try Data(contentsOf: output.appendingPathComponent("response-source-license.txt")) == license)
        do {
            _ = try await library.createNarrowband(from: entry, name: "Double", layout: layout, centers: [656.3,500.7,500.7], widths: [7,6,6])
            Issue.record("Filter applied twice")
        } catch { }
    }
    #expect(try Data(contentsOf: selected.url) == Data(text.utf8))
    #expect(try Data(contentsOf: source) == Data(text.utf8))
    try fm.removeItem(at: source)
    let reopened = SpectralProfileLibrary(root: repository, service: service)
    let entries = try await reopened.list(); #expect(entries.count == 4)
    for entry in entries where expected[entry.id] != nil {
        #expect(try await reopened.select(entry).inspection.profile == expected[entry.id])
    }
    do {
        _ = try await reopened.createNarrowband(from: base, name: "Invalid", layout: "osc-dual", centers: [500,501,501], widths: [7,6,6])
        Issue.record("Overlapping dual passbands accepted")
    } catch { }
    #expect(try await reopened.list().count == 4)
    #expect(try fm.contentsOfDirectory(atPath: repository.path).allSatisfy { !$0.hasPrefix(".") })
}
