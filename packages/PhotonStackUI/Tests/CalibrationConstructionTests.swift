import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

private func constructionRoot() throws -> URL {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("Construction tests " + UUID().uuidString)
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: false)
    return url
}
private func constructionService() throws -> CLIProcessingService {
    CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
}
private func constructionFrames(_ root: URL, role: String = "Dark", name: String = "raw", values: [Float] = [100, 101, 102]) throws -> [URL] {
    try values.enumerated().map { index, value in
        let url = root.appendingPathComponent("\(name)-\(index).fits")
        let cards: [(String, String)] = [
            ("SIMPLE","T"),("BITPIX","-32"),("NAXIS","2"),("NAXIS1","12"),("NAXIS2","10"),
            ("INSTRUME","'Camera'"),("CAM-SER","'serial-1'"),("CCD-TEMP","-10"),
            ("FILTER","'Ha'"),("EXPTIME",role == "Bias" ? "0" : "60"),("GAIN","100"),("OFFSET","20"),
            ("READMODE","'normal'"),("XBINNING","1"),("YBINNING","1"),("XORGSUBF","0"),("YORGSUBF","0"),
            ("BAYERPAT","'RGGB'"),("BUNIT","'ADU'"),("PSCOLOR","'Linear'"),("PSOPTICS","'train-1'"),("IMAGETYP","'\(role)'")]
        var header = cards.map { ($0.0.padding(toLength: 8, withPad: " ", startingAt: 0) + "= " + $0.1).padding(toLength: 80, withPad: " ", startingAt: 0) }.joined()
        header += "END".padding(toLength: 80, withPad: " ", startingAt: 0)
        header = header.padding(toLength: ((header.utf8.count + 2879) / 2880) * 2880, withPad: " ", startingAt: 0)
        var data = Data(header.utf8)
        for p in 0..<120 { var bits = (value + Float(p) / 16).bitPattern.bigEndian; withUnsafeBytes(of: &bits) { data.append(contentsOf: $0) } }
        data.append(Data(repeating: 0, count: (2880 - data.count % 2880) % 2880))
        try data.write(to: url); return url
    }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func constructionBuildSaveRelocateArchiveExportAndReimportPreservesHistory() async throws {
    let root = try constructionRoot(), fm = FileManager.default; defer { try? fm.removeItem(at: root) }
    let service = try constructionService(), library = CalibrationLibrary(root: root.appendingPathComponent("library"), service: service)
    let frames = try constructionFrames(root), originals = try frames.map { try Data(contentsOf: $0) }
    let draft = try await library.buildMaster(.init(frames: frames))
    #expect(draft.report.isValid && draft.report.inspection.sourcesVerified && draft.report.invalidPixels == 0)
    var invalid = draft.report; invalid.width = Int.max; #expect(!invalid.isValid)
    invalid = draft.report; invalid.factors[0] = []; #expect(!invalid.isValid)
    invalid = draft.report; invalid.inspection.productsVerified = false; #expect(!invalid.isValid)
    invalid = draft.report; invalid.missingSamples = UInt64.max; #expect(!invalid.isValid)
    invalid = draft.report; invalid.sources[1] = invalid.sources[0]; #expect(!invalid.isValid)
    invalid = draft.report; invalid.inspection.files.removeValue(forKey: "accepted.fits"); #expect(!invalid.isValid)
    let id = try await library.saveDraft(draft.id, label: "Dark history", notes: "Sensor samples")
    #expect(!fm.fileExists(atPath: draft.directory.path))
    let entry = try #require(try await library.list().first)
    #expect(entry.id == id && entry.receipt?.construction != nil)
    let fresh = try await library.inspectConstruction(entry, verifySources: true)
    #expect(fresh.inspection.sourcesVerified)
    #expect(try frames.map { try Data(contentsOf: $0) } == originals)
    for file in frames { try fm.removeItem(at: file) }
    let offline = try await library.inspectConstruction(entry)
    #expect(!offline.inspection.sourcesVerified && offline.sources == fresh.sources)
    await #expect(throws: (any Error).self) { try await library.inspectConstruction(entry, verifySources: true) }
    _ = try await library.verify(entry)
    try await library.archive(entry)
    let archived = try #require(try await library.list(archived: true).first)
    let export = root.appendingPathComponent("export")
    try await library.export(archived, to: export)
    try await library.archive(archived)
    let other = CalibrationLibrary(root: root.appendingPathComponent("other"), service: service)
    #expect(try await other.importConstruction(export, label: "Imported history", notes: "") == id)
    let copied = try #require(try await other.list().first)
    #expect(try await other.inspectConstruction(copied).inspection.files == offline.inspection.files)
    for name in CalibrationMasterConstruction.names.union(["master.fits"]) {
        #expect(try Data(contentsOf: export.appendingPathComponent(name)) == Data(contentsOf: root.appendingPathComponent("other/\(id)/\(name)")))
    }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_MASTER_BENCHMARK"] != nil && ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func constructionLargeMasterRetainsScientificBytesAcrossSaveAndExport() async throws {
    let fixture = URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_MASTER_BENCHMARK"]))
    let root = try constructionRoot(); defer { try? FileManager.default.removeItem(at: root) }
    let library = CalibrationLibrary(root: root.appendingPathComponent("library"), service: try constructionService())
    let inputs = (0..<12).map { fixture.appendingPathComponent(String(format: "benchmark-%02d.fits", $0)) }
    let clock = ContinuousClock(), start = clock.now
    let draft = try await library.buildMaster(.init(frames: inputs))
    let built = clock.now
    let id = try await library.saveDraft(draft.id, label: "Full-size benchmark", notes: "Generated data")
    let saved = clock.now
    let entry = try #require(try await library.list().first)
    let inspected = try await library.inspectConstruction(entry)
    let checked = clock.now
    let output = root.appendingPathComponent("export")
    try await library.export(entry, to: output)
    let exported = clock.now
    for name in ["master.fits", "accepted.fits"] {
        #expect(try Data(contentsOf: output.appendingPathComponent(name)) == Data(contentsOf: fixture.appendingPathComponent("benchmark-0/\(name)")))
    }
    #expect(entry.id == id && !inspected.inspection.sourcesVerified && inspected.width * inspected.height == 2160 * 3840)
    print("MASTER_LIBRARY_BENCHMARK build=\(start.duration(to: built)) save=\(built.duration(to: saved)) inspect=\(saved.duration(to: checked)) export=\(checked.duration(to: exported))")
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func constructionChangedProductsRejectReviewSaveExportAndMatching() async throws {
    let root = try constructionRoot(), fm = FileManager.default; defer { try? fm.removeItem(at: root) }
    let service = try constructionService(), library = CalibrationLibrary(root: root.appendingPathComponent("library"), service: service)
    let draft = try await library.buildMaster(.init(frames: constructionFrames(root)))
    let map = draft.directory.appendingPathComponent("accepted.fits"), data = try Data(contentsOf: map)
    try Data("changed".utf8).write(to: map)
    await #expect(throws: (any Error).self) { try await library.saveDraft(draft.id, label: "Bad", notes: "") }
    #expect(try await library.list().isEmpty)
    try data.write(to: map)
    let id = try await library.saveDraft(draft.id, label: "Good", notes: "")
    let entry = try #require(try await library.list().first)
    let stored = root.appendingPathComponent("library/\(id)/report.json")
    try Data("{}".utf8).write(to: stored)
    await #expect(throws: (any Error).self) { try await library.verify(entry) }
    let output = root.appendingPathComponent("export")
    await #expect(throws: (any Error).self) { try await library.export(entry, to: output) }
    #expect(!fm.fileExists(atPath: output.path))
    let lights = try constructionFrames(root, role: "Light", name: "lights")
    await #expect(throws: (any Error).self) { try await library.plan(lights: lights, entries: [entry], tolerance: 1) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
@MainActor func constructionDistinctHistoriesSharePixelsWithoutDuplicateCandidates() async throws {
    let root = try constructionRoot(), fm = FileManager.default; defer { try? fm.removeItem(at: root) }
    let library = CalibrationLibrary(root: root.appendingPathComponent("library"), service: try constructionService())
    for (name, values) in [("a", [Float(100), 101, 102]), ("b", [Float(100), 101, 103])] {
        let draft = try await library.buildMaster(.init(frames: constructionFrames(root, name: name, values: values)))
        _ = try await library.saveDraft(draft.id, label: name, notes: "")
    }
    let entries = try await library.list()
    #expect(entries.count == 2 && entries[0].id != entries[1].id)
    #expect(entries[0].receipt?.acquisition.sha == entries[1].receipt?.acquisition.sha)
    let session = CalibrationLibrarySession(library: library, lights: [])
    try await session.reload(); #expect(session.selectedMasters.count == 1)
    let lights = try constructionFrames(root, role: "Light", name: "lights")
    await #expect(throws: (any Error).self) { try await library.plan(lights: lights, entries: entries, tolerance: 1) }
    let report = try await library.plan(lights: lights, entries: [entries[0]], tolerance: 1)
    #expect(report.masters.count == 1 && report.masters[0].acquisition.sha == entries[0].receipt?.acquisition.sha)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
@MainActor func constructionCorrectionsUseLibraryStateAndDarkFlatsStayOutOfLightReview() async throws {
    let root = try constructionRoot(), fm = FileManager.default; defer { try? fm.removeItem(at: root) }
    let library = CalibrationLibrary(root: root.appendingPathComponent("library"), service: try constructionService())
    let bias = try await library.buildMaster(.init(frames: constructionFrames(root, role: "Bias", name: "bias", values: [10,11,12]), role: "bias"))
    let biasID = try await library.saveDraft(bias.id, label: "Bias", notes: "")
    let dark = try await library.buildMaster(.init(frames: constructionFrames(root, name: "dark"), role: "dark-flat", biasID: biasID))
    #expect(dark.report.biasState == "removed")
    let darkID = try await library.saveDraft(dark.id, label: "Dark flat", notes: "")
    let flatFrames = try constructionFrames(root, role: "Flat", name: "flat", values: [1000,2000,3000])
    await #expect(throws: (any Error).self) { try await library.buildMaster(.init(frames: flatFrames, role: "flat", darkFlatID: darkID)) }
    let flat = try await library.buildMaster(.init(frames: flatFrames, role: "flat", biasID: biasID, darkFlatID: darkID))
    #expect(flat.report.role == "flat" && flat.report.biasState == "removed")
    _ = try await library.saveDraft(flat.id, label: "Flat", notes: "")
    let session = CalibrationLibrarySession(library: library, lights: [])
    try await session.reload(); #expect(!session.selectedMasters.contains(darkID) && session.selectedMasters.count == 2)
    let model = CalibrationMasterBuildSession(library: library, entries: session.entries)
    model.frames = flatFrames; model.role = "flat"; model.correction = "dark-flat"; model.darkFlatID = darkID
    #expect(model.needsBias && model.request == nil)
    model.biasID = biasID; #expect(model.request != nil)
    let entry = try #require(session.entries.first { $0.id == biasID }); try await library.archive(entry)
    await #expect(throws: (any Error).self) { try await library.buildMaster(model.request!) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
@MainActor func constructionSessionInvalidatesDraftAndCleansTemporaryProducts() async throws {
    let root = try constructionRoot(), fm = FileManager.default; defer { try? fm.removeItem(at: root) }
    let library = CalibrationLibrary(root: root.appendingPathComponent("library"), service: try constructionService())
    let model = CalibrationMasterBuildSession(library: library, entries: [])
    model.frames = try constructionFrames(root); model.name = "Dark"; model.build()
    let clock = ContinuousClock(), deadline = clock.now.advanced(by: .seconds(30))
    while model.busy && clock.now < deadline { try await Task.sleep(for: .milliseconds(10)) }
    #expect(!model.busy && model.error == nil)
    let draft = try #require(model.currentDraft); #expect(model.canSave)
    model.notes = "New note"; #expect(model.canSave)
    model.minimum = "3"; #expect(!model.canSave && model.currentDraft == nil)
    model.invalidate()
    while fm.fileExists(atPath: draft.directory.path) && clock.now < deadline { try await Task.sleep(for: .milliseconds(10)) }
    #expect(!fm.fileExists(atPath: draft.directory.path))
    model.build(); model.close()
    while model.busy && clock.now < deadline { try await Task.sleep(for: .milliseconds(10)) }
    while !(await library.drafts.isEmpty) && clock.now < deadline { try await Task.sleep(for: .milliseconds(10)) }
    let retained = await library.drafts
    #expect(!model.busy && model.draft == nil && retained.isEmpty)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationJobNativeServiceRetainsPartialProductsAndResumes() async throws {
    let root = try constructionRoot(), fm = FileManager.default; defer { try? fm.removeItem(at: root) }
    let service = try constructionService(), library = CalibrationLibrary(root: root.appendingPathComponent("library"), service: service)
    let draft = try await library.buildMaster(.init(frames: constructionFrames(root, role: "Bias", name: "bias", values: [10,11,12]), role: "bias"))
    let id = try await library.saveDraft(draft.id, label: "Bias", notes: "")
    let bias = root.appendingPathComponent("library/\(id)/master.fits")
    let lights = try constructionFrames(root, role: "Light", name: "lights")
    let manifest = root.appendingPathComponent("job-input.txt")
    let text = "PhotonStack-CalibrationJob-1\nmaster b bias \"\(bias.path)\" - included\n" + lights.map { "light \"\($0.path)\" - b - -\n" }.joined()
    try text.write(to: manifest, atomically: true, encoding: .utf8)
    let job = root.appendingPathComponent("job")
    let first = try await service.runCLI(arguments: ["calibration-run", "--manifest", manifest.path, "--output-dir", job.path, "--max-frames", "1"])
    let partial = try #require(JSONSerialization.jsonObject(with: Data(first.standardOutput.utf8)) as? [String: Any])
    #expect(partial["status"] as? String == "partial" && partial["completed"] as? Int == 1)
    let committed = job.appendingPathComponent("frame-000001/calibrated.fits"), bytes = try Data(contentsOf: committed)
    let result = try await service.runCLI(arguments: ["calibration-resume", "--job", job.path])
    let complete = try #require(JSONSerialization.jsonObject(with: Data(result.standardOutput.utf8)) as? [String: Any])
    #expect(complete["status"] as? String == "complete" && complete["completed"] as? Int == 3)
    #expect(try Data(contentsOf: committed) == bytes)
    let acquisition = try await CalibrationAcquisition.inspect([committed], service: service)[0]
    #expect(acquisition.text["calibration"] == "sensor-v1" && acquisition.text["biasState"] == "removed")
    #expect(try fm.contentsOfDirectory(atPath: root.path).allSatisfy { !$0.contains("photonstack-output") })
}
