import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

private func calibrationFixture(_ url: URL, type: String = "Light", object: String = "Veil", date: String = "2026-10-15T22:00:00",
                                filter: String = "Ha", exposure: Double = 60, temperature: Double = -10) throws {
    let cards: [(String,String)] = [
        ("SIMPLE","T"),("BITPIX","-32"),("NAXIS","2"),("NAXIS1","12"),("NAXIS2","10"),
        ("INSTRUME","'Test camera'"),("CAM-SER","'SERIAL-1'"),("CCD-TEMP",String(temperature)),
        ("OBJECT","'\(object)'"),("DATE-OBS","'\(date)'"),("FILTER","'\(filter)'"),
        ("EXPTIME",String(exposure)),("GAIN","100"),("OFFSET","20"),("READMODE","'fast'"),
        ("XBINNING","1"),("YBINNING","1"),("XORGSUBF","0"),("YORGSUBF","0"),
        ("BAYERPAT","'RGGB'"),("BUNIT","'ADU'"),("PSCOLOR","'Linear'"),("PSOPTICS","'train-1'"),("IMAGETYP","'\(type)'")]
    var header = cards.map { ($0.0.padding(toLength: 8, withPad: " ", startingAt: 0) + "= " + $0.1).padding(toLength: 80, withPad: " ", startingAt: 0) }.joined()
    header += "END".padding(toLength: 80, withPad: " ", startingAt: 0)
    header = header.padding(toLength: ((header.utf8.count+2879)/2880)*2880, withPad: " ", startingAt: 0)
    var bytes = Data(header.utf8)
    for i in 0..<120 { var pixel = Float(100+Double(i)/10).bitPattern.bigEndian; withUnsafeBytes(of: &pixel) { bytes.append(contentsOf: $0) } }
    bytes.append(Data(repeating: 0, count: (2880-bytes.count%2880)%2880))
    try bytes.write(to: url)
}

private func calibrationTestService() throws -> CLIProcessingService {
    CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
}
private func calibrationTestRoot() throws -> URL {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("Calibration tests \(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false); return root
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationLibraryCopiesReopensArchivesRestoresAndExportsMasters() async throws {
    let fm = FileManager.default, root = try calibrationTestRoot(); defer { try? fm.removeItem(at: root) }
    let service = try calibrationTestService(), input = root.appendingPathComponent("master dark.fits")
    try calibrationFixture(input, type: "Master Dark")
    let original = try Data(contentsOf: input), storage = root.appendingPathComponent("Library")
    let library = CalibrationLibrary(root: storage, service: service)
    let id = try await library.importMaster(input, label: "60 s dark", role: "dark", biasState: "included", notes: "Measured -10°C")
    #expect(try Data(contentsOf: input) == original)
    #expect(try await library.importMaster(input, label: "Another name", role: "dark", biasState: "included", notes: "") == id)
    #expect(try await library.list().count == 1)
    do {
        _ = try await library.importMaster(input, label: "Wrong declaration", role: "dark", biasState: "removed", notes: "")
        Issue.record("Conflicting declaration replaced an existing master")
    } catch { }
    try fm.removeItem(at: input)
    let reopened = CalibrationLibrary(root: storage, service: service)
    let entry = try #require(try await reopened.list().first)
    #expect(entry.receipt?.label == "60 s dark" && entry.receipt?.notes == "Measured -10°C")
    #expect(try await reopened.verify(entry).sha == id)
    let output = root.appendingPathComponent("Export")
    try await reopened.export(entry, to: output)
    #expect(try Data(contentsOf: output.appendingPathComponent("master.fits")) == original)
    do { try await reopened.export(entry, to: output); Issue.record("Existing export replaced") } catch { }
    try await reopened.archive(entry)
    #expect(try await reopened.list().isEmpty)
    let archived = try #require(try await reopened.list(archived: true).first)
    #expect(try await reopened.verify(archived).sha == id)
    try await reopened.archive(archived)
    #expect(try await reopened.list().count == 1)
    #expect(try await reopened.list(archived: true).isEmpty)
    #expect(try fm.contentsOfDirectory(atPath: storage.path).allSatisfy { !$0.hasPrefix(".") })
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationLibraryPlansTwoNightsAndChecksReportIndicesAndAggregates() async throws {
    let fm = FileManager.default, root = try calibrationTestRoot(); defer { try? fm.removeItem(at: root) }
    let service = try calibrationTestService(), library = CalibrationLibrary(root: root.appendingPathComponent("Library"), service: service)
    var lights: [URL] = []
    for day in [15,16] { for filter in ["Ha","OIII"] { for exposure in [60.0,120.0] {
        let url = root.appendingPathComponent("\(day)-\(filter)-\(exposure).fits")
        try calibrationFixture(url, date: "2026-10-\(day)T22:00:00", filter: filter, exposure: exposure)
        lights.append(url)
    } } }
    for exposure in [60.0,120.0] {
        let url = root.appendingPathComponent("dark-\(exposure).fits"); try calibrationFixture(url, type: "Dark", exposure: exposure)
        _ = try await library.importMaster(url, label: "Dark \(exposure)", role: "dark", biasState: "included", notes: "")
    }
    for filter in ["Ha","OIII","SII"] {
        let url = root.appendingPathComponent("flat-\(filter).fits"); try calibrationFixture(url, type: "Flat", filter: filter, exposure: 0.2)
        _ = try await library.importMaster(url, label: "Flat \(filter)", role: "flat", biasState: "removed", notes: "")
    }
    let before = try lights.map { try Data(contentsOf: $0) }, entries = try await library.list()
    let report = try await library.plan(lights: lights, entries: entries, tolerance: 1)
    #expect(report.isValid && report.groups.count == 8 && report.frameChoices.count == 8 && !report.canApply)
    for g in report.groups {
        let dark = try #require(g.roles["dark"]?.suggestedMaster), flat = try #require(g.roles["flat"]?.suggestedMaster)
        #expect(report.masters[dark].acquisition.numeric["exposure"] == g.numeric["exposure"])
        #expect(report.masters[flat].acquisition.text["filter"] == g.text["filter"])
        #expect(g.roles["bias"]?.status == "missing-master")
    }
    let saved = try JSONEncoder().encode(report), restored = try JSONDecoder().decode(CalibrationPlanReport.self, from: saved)
    #expect(restored.isValid)
    let object = try #require(JSONSerialization.jsonObject(with: saved) as? [String: Any])
    let choices = try #require(object["frameChoices"] as? [[String: Any]])
    let roles = try #require(choices.first?["roles"] as? [String: [String: Any]])
    #expect(roles["bias"]?["suggestedMaster"] is NSNull)
    var broken = restored; broken.frameChoices[0].light = 1; #expect(!broken.isValid)
    broken = restored; broken.groups[0].members = [Int.max]; #expect(!broken.isValid)
    broken = restored; broken.groups[0].candidates[0].compatibleCount = Int.max; #expect(!broken.isValid)
    broken = restored; broken.groups[0].candidates[0].master = -1; #expect(!broken.isValid)
    broken = restored; broken.frameChoices[0].roles["dark"]?.suggestedMaster = Int.max; #expect(!broken.isValid)
    broken = restored; broken.canApply = true; #expect(!broken.isValid)
    broken = restored; broken.lights[0].sha = "bad"; #expect(!broken.isValid)
    #expect(try lights.map { try Data(contentsOf: $0) } == before)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationLibraryRejectsChangedFilesBadRecordsAndUnsafeImports() async throws {
    let fm = FileManager.default, root = try calibrationTestRoot(); defer { try? fm.removeItem(at: root) }
    let service = try calibrationTestService(), input = root.appendingPathComponent("dark.fits"), light = root.appendingPathComponent("light.fits")
    try calibrationFixture(input, type: "Dark"); try calibrationFixture(light)
    let library = CalibrationLibrary(root: root.appendingPathComponent("Library"), service: service)
    do { _ = try await library.importMaster(light, label: "Wrong", role: "dark", biasState: "included", notes: ""); Issue.record("Light imported as dark") } catch { }
    _ = try await library.importMaster(input, label: "Dark", role: "dark", biasState: "unknown", notes: "")
    let entry = try #require(try await library.list().first), verified = try await library.verify(entry)
    let path = URL(fileURLWithPath: verified.path), original = try Data(contentsOf: path)
    var changed = original; changed[3000] ^= 1; try changed.write(to: path)
    do { _ = try await library.verify(entry); Issue.record("Changed pixels passed verification") } catch { }
    do { _ = try await library.plan(lights: [light], entries: [entry], tolerance: 1); Issue.record("Changed master entered a plan") } catch { }
    try original.write(to: path)
    for tolerance in [Double.nan, -1, 11] {
        do { _ = try await library.plan(lights: [light], entries: [entry], tolerance: tolerance); Issue.record("Invalid tolerance accepted") } catch { }
    }
    try fm.removeItem(at: path); try fm.createSymbolicLink(at: path, withDestinationURL: input)
    do { _ = try await library.verify(entry); Issue.record("Linked library file accepted") } catch { }
    #expect(try await library.list().first?.issue != nil)
    try fm.removeItem(at: path); try original.write(to: path)
    try Data("{}".utf8).write(to: path.deletingLastPathComponent().appendingPathComponent("entry.json"))
    #expect(try await library.list().first?.issue != nil)
    #expect(try Data(contentsOf: input) == original)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationLibraryConcurrentImportAndFolderScanPreserveSources() async throws {
    let fm = FileManager.default, root = try calibrationTestRoot(); defer { try? fm.removeItem(at: root) }
    let service = try calibrationTestService(), input = root.appendingPathComponent("dark.fits"), storage = root.appendingPathComponent("Library")
    try calibrationFixture(input, type: "Dark")
    let a = CalibrationLibrary(root: storage, service: service), b = CalibrationLibrary(root: storage, service: service)
    async let first = a.importMaster(input, label: "Dark", role: "dark", biasState: "included", notes: "")
    async let second = b.importMaster(input, label: "Dark", role: "dark", biasState: "included", notes: "")
    #expect(try await first == second)
    #expect(try await a.list().count == 1)
    #expect(try fm.contentsOfDirectory(atPath: storage.path).allSatisfy { !$0.hasPrefix(".") })
    let folder = root.appendingPathComponent("Night"), child = folder.appendingPathComponent("Ha")
    try fm.createDirectory(at: child, withIntermediateDirectories: true)
    let light = child.appendingPathComponent("light.fit"); try calibrationFixture(light)
    try Data("JPEG".utf8).write(to: child.appendingPathComponent("preview.jpg"))
    try fm.createSymbolicLink(at: child.appendingPathComponent("alias.fit"), withDestinationURL: input)
    try fm.createSymbolicLink(at: child.appendingPathComponent("loop"), withDestinationURL: root)
    #expect(try await a.scanLights([folder, light]) == [light.resolvingSymlinksInPath()])
    let cancelled = Task { try Task.checkCancellation(); return try await a.scanLights([folder]) }
    cancelled.cancel()
    do { _ = try await cancelled.value; Issue.record("Cancelled scan completed") } catch is CancellationError { }
}

@Test @MainActor func calibrationReviewSessionRejectsInvalidToleranceAndClearsStaleReview() {
    let library = CalibrationLibrary(root: URL(fileURLWithPath: "/tmp/unused-calibration-test"))
    let session = CalibrationLibrarySession(library: library, lights: [])
    #expect(session.parsedTolerance == 1)
    for value in ["", "bad", "nan", "-1", "11"] { session.tolerance = value; #expect(session.parsedTolerance == nil) }
    session.tolerance = "0.5"; #expect(session.parsedTolerance == 0.5)
    session.groupSelection = 5; session.frameSelection = 9; session.invalidate()
    #expect(session.groupSelection == nil && session.frameSelection == nil && session.report == nil)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil)) @MainActor
func calibrationReviewSessionRetainsViewDataButInvalidatesObsoleteResults() async throws {
    let fm = FileManager.default, root = try calibrationTestRoot(); defer { try? fm.removeItem(at: root) }
    let input = root.appendingPathComponent("light.fits"); try calibrationFixture(input)
    let library = CalibrationLibrary(root: root.appendingPathComponent("Library"), service: try calibrationTestService())
    let dark = root.appendingPathComponent("dark.fits"); try calibrationFixture(dark, type: "Dark")
    let id = try await library.importMaster(dark, label: "Dark", role: "dark", biasState: "included", notes: "")
    let session = CalibrationLibrarySession(library: library, lights: [input])
    try await session.reload(); #expect(session.selectedMasters == [id])
    session.selectedMasters.remove(id); try await session.reload(); #expect(session.selectedMasters.isEmpty)
    session.check()
    let deadline = Date().addingTimeInterval(10)
    while session.busy && Date() < deadline { try await Task.sleep(for: .milliseconds(10)) }
    #expect(!session.busy && session.reportIsCurrent && session.report?.lights.count == 1)
    session.invalidate()
    #expect(!session.reportIsCurrent && session.report != nil && session.groupSelection == nil && session.frameSelection == nil)
    session.check()
    while session.busy && Date() < deadline { try await Task.sleep(for: .milliseconds(10)) }
    #expect(session.reportIsCurrent && session.groupSelection == 0 && session.frameSelection == 0)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationLibraryImportsConstructedMasterAndPreservesPreparationAndTemperatureRange() async throws {
    let fm = FileManager.default, root = try calibrationTestRoot(); defer { try? fm.removeItem(at: root) }
    let service = try calibrationTestService(), library = CalibrationLibrary(root: root.appendingPathComponent("Library"), service: service)
    let first = root.appendingPathComponent("dark-1.fits"), second = root.appendingPathComponent("dark-2.fits")
    try calibrationFixture(first, type: "Dark", temperature: -10)
    try calibrationFixture(second, type: "Dark", temperature: -9.2)
    let manifest = root.appendingPathComponent("inputs.txt"), output = root.appendingPathComponent("Built")
    try "PhotonStack-SensorMaster-1\nrole dark\nframe \"\(first.path)\" -\nframe \"\(second.path)\" -\n".write(to: manifest, atomically: true, encoding: .utf8)
    _ = try await service.runCLI(arguments: ["sensor-master-build", "--manifest", manifest.path, "--output-dir", output.path])
    let master = output.appendingPathComponent("master.fits")
    do {
        _ = try await library.importMaster(master, label: "Wrong declaration", role: "dark", biasState: "removed", notes: "")
        Issue.record("Constructed master accepted a contradictory bias declaration")
    } catch { }
    _ = try await library.importMaster(master, label: "Constructed dark", role: "dark", biasState: "included", notes: "Two raw dark exposures")
    let entry = try #require(try await library.list().first), acquisition = try await library.verify(entry)
    #expect(acquisition.text["biasState"] == "included")
    #expect(acquisition.numeric["sensorTemperatureMin"] == -10 && acquisition.numeric["sensorTemperatureMax"] == -9.2)
    #expect(CalibrationPresentation.value("sensorTemperature", in: acquisition, chinese: true) == "-10…-9.2")
    let light = root.appendingPathComponent("light.fits"); try calibrationFixture(light, temperature: -10.4)
    let report = try await library.plan(lights: [light], entries: [entry], tolerance: 1)
    #expect(report.isValid && report.frameChoices.first?.roles["dark"]?.status == "no-compatible-master")
    _ = try await service.runCLI(arguments: ["sensor-master-inspect", "--recipe", output.appendingPathComponent("recipe.txt").path])
}
