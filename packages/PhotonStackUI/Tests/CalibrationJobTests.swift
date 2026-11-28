import Foundation
import Testing
import PhotonStackAppCore
import PhotonStackProcessing
@testable import PhotonStackUI

private func jobFITS(_ url: URL, role: String, value: Float, exposure: Int = 60, day: Int = 15, sequence: Int = 0) throws {
    let cards: [(String,String)] = [
        ("SIMPLE","T"),("BITPIX","-32"),("NAXIS","2"),("NAXIS1","12"),("NAXIS2","10"),
        ("INSTRUME","'Camera'"),("CAM-SER","'serial-1'"),("CCD-TEMP","-10"),("GAIN","100"),("OFFSET","20"),
        ("READMODE","'normal'"),("EXPTIME",String(exposure)),("XBINNING","1"),("YBINNING","1"),("XORGSUBF","0"),("YORGSUBF","0"),
        ("BAYERPAT","'RGGB'"),("BUNIT","'ADU'"),("PSCOLOR","'Linear'"),("PSOPTICS","'train-1'"),("FILTER","'Ha'"),
        ("OBJECT","'Generated'"),("DATE-OBS","'2026-10-\(day)T22:0\(sequence):00'"),("IMAGETYP","'\(role)'")]
    var header = cards.map { ($0.0.padding(toLength: 8, withPad: " ", startingAt: 0) + "= " + $0.1).padding(toLength: 80, withPad: " ", startingAt: 0) }.joined()
    header += "END".padding(toLength: 80, withPad: " ", startingAt: 0)
    header = header.padding(toLength: ((header.utf8.count + 2879) / 2880) * 2880, withPad: " ", startingAt: 0)
    var data = Data(header.utf8)
    for p in 0..<120 { var bits = (value + Float(p) / 16).bitPattern.bigEndian; withUnsafeBytes(of: &bits) { data.append(contentsOf: $0) } }
    data.append(Data(repeating: 0, count: (2880 - data.count % 2880) % 2880)); try data.write(to: url)
}
private struct JobFixture {
    let root: URL, library: CalibrationLibrary
    var draft: CalibrationJobDraft
}
private func jobFixture(service: (any ProcessingService)? = nil) async throws -> JobFixture {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("Calibration job \(UUID())")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
    do {
        let cli = try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])
        let library = CalibrationLibrary(root: root.appendingPathComponent("library"), service: service ?? CLIProcessingService(executableURL: URL(fileURLWithPath: cli)))
        for (name, role, value, exposure, state) in [("Bias A", "bias", Float(10), 0, "included"), ("Bias B", "bias", Float(11), 0, "included"), ("Dark 60", "dark", Float(13), 60, "included"), ("Dark 120", "dark", Float(16), 120, "included"), ("Flat", "flat", Float(200), 1, "removed")] {
            let url = root.appendingPathComponent(name + ".fits")
            try jobFITS(url, role: role, value: value, exposure: exposure)
            _ = try await library.importMaster(url, label: name, role: role, biasState: state, notes: "Generated")
        }
        let lights = try (0..<4).map { i in
            let url = root.appendingPathComponent("light-\(i).fits")
            try jobFITS(url, role: "Light", value: Float(100 + i), exposure: i < 2 ? 60 : 120, day: i < 2 ? 15 : 16, sequence: i)
            return url
        }
        let entries = try await library.list(), report = try await library.plan(lights: lights, entries: entries, tolerance: 1)
        return JobFixture(root: root, library: library, draft: try CalibrationJobDraft(review: report, entries: entries))
    } catch { try? FileManager.default.removeItem(at: root); throw error }
}
private func ready(_ source: CalibrationJobDraft) -> CalibrationJobDraft {
    var draft = source; draft.fillUniqueCandidates()
    draft.assign(CalibrationJobDraft.omitted, role: 0, frames: Array(draft.choices.indices))
    return draft
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationDraftPreservesPartialDecisionsAndRevalidatesSources() async throws {
    let f = try await jobFixture(), fm = FileManager.default
    defer { try? fm.removeItem(at: f.root) }
    var draft = f.draft; draft.fillUniqueCandidates()
    draft.assign(CalibrationJobDraft.omitted, role: 0, frames: [0])
    draft.workers = 2; draft.memoryMiB = 256
    let file = f.root.appendingPathComponent("assignment.json")
    try await f.library.saveDraft(draft, to: file)
    let reopened = try await f.library.openDraft(file)
    #expect(!reopened.isReady && reopened.choices == draft.choices)
    #expect(reopened.workers == 2 && reopened.memoryMiB == 256)
    let bytes = try Data(contentsOf: file)
    await #expect(throws: (any Error).self) { try await f.library.saveDraft(draft, to: f.root.appendingPathComponent("library/entry.json")) }
    try jobFITS(URL(fileURLWithPath: draft.review.lights[0].path), role: "Light", value: 700)
    await #expect(throws: (any Error).self) { try await f.library.openDraft(file) }
    #expect(try Data(contentsOf: file) == bytes)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationDraftRejectsArchivedMastersAndInvalidAssignmentIndices() async throws {
    let f = try await jobFixture(), fm = FileManager.default
    defer { try? fm.removeItem(at: f.root) }
    let draft = ready(f.draft), file = f.root.appendingPathComponent("draft.json")
    try await f.library.saveDraft(draft, to: file)
    let reopened = try await f.library.openDraft(file)
    #expect(try reopened.manifest() == draft.manifest())
    try await f.library.archive(draft.entries[0])
    await #expect(throws: (any Error).self) { try await f.library.openDraft(file) }
    var json = try #require(JSONSerialization.jsonObject(with: Data(contentsOf: file)) as? [String: Any])
    json["choices"] = [[Int.max]]
    try JSONSerialization.data(withJSONObject: json).write(to: file)
    await #expect(throws: (any Error).self) { try await f.library.openDraft(file) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationMasterReviewIsAuditedAndExcludesRejectedInputs() async throws {
    let f = try await jobFixture(), fm = FileManager.default
    defer { try? fm.removeItem(at: f.root) }
    let entry = f.draft.entries[0], original = try #require(entry.receipt)
    let draftFile = f.root.appendingPathComponent("draft.json")
    try await f.library.saveDraft(f.draft, to: draftFile)
    try await f.library.revise(entry, label: "Rejected master", notes: "Inspection", biasState: original.biasState,
                              quality: "reject", reason: "Visible artifact")
    let current = try #require(try await f.library.list().first { $0.id == entry.id })
    #expect(current.receipt?.rejected == true)
    #expect(current.receipt?.reviewHistory?.last?.previousLabel == original.label)
    #expect(try await f.library.verify(current).sha == original.acquisition.sha)
    await #expect(throws: (any Error).self) { try await f.library.openDraft(draftFile) }
    await #expect(throws: (any Error).self) {
        try await f.library.plan(lights: f.draft.review.lights.map { URL(fileURLWithPath: $0.path) }, entries: [current], tolerance: 1)
    }
    await #expect(throws: (any Error).self) {
        try await f.library.revise(entry, label: original.label, notes: "", biasState: original.biasState, quality: "keep", reason: "Stale edit")
    }
    try await f.library.revise(current, label: original.label, notes: "Rechecked", biasState: original.biasState, quality: "keep", reason: "Independent review")
    let restored = try #require(try await f.library.list().first { $0.id == entry.id })
    #expect(restored.receipt?.rejected == false && restored.receipt?.reviewHistory?.count == 2)
    let exported = f.root.appendingPathComponent("reviewed-export")
    try await f.library.export(restored, to: exported)
    #expect(fm.fileExists(atPath: exported.appendingPathComponent("entry.json").path))
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationDeclarationCopiesWithoutChangingOriginalsThroughAppService() async throws {
    let f = try await jobFixture(), fm = FileManager.default
    defer { try? fm.removeItem(at: f.root) }
    let inputs = f.draft.review.lights.map { URL(fileURLWithPath: $0.path) }
    let original = try inputs.map { try Data(contentsOf: $0) }
    let output = f.root.appendingPathComponent("declared")
    let copies = try await f.library.declareAcquisition(inputs, fields: ["CAM-SER": "correct serial", "PSOPTICS": "correct train"], reason: "Acquisition log", to: output)
    #expect(copies.count == 4)
    let metadata = try await CalibrationAcquisition.inspect(copies, service: CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"]))))
    #expect(metadata.allSatisfy { $0.text["cameraSerial"] == "correct serial" && $0.text["opticalTrain"] == "correct train" })
    #expect(try inputs.map { try Data(contentsOf: $0) } == original)
    #expect(fm.fileExists(atPath: output.appendingPathComponent("declarations.json").path))
    await #expect(throws: (any Error).self) { try await f.library.declareAcquisition(inputs, fields: ["BITPIX": "16"], reason: "Invalid", to: f.root.appendingPathComponent("invalid")) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationQualityMapPersistsInDraftAndResumableJob() async throws {
    let f = try await jobFixture(), fm = FileManager.default
    defer { try? fm.removeItem(at: f.root) }
    let entry = try #require(f.draft.entries.first { $0.receipt?.role == "dark" })
    let quality = f.root.appendingPathComponent("quality")
    let report = try await f.library.inspectQuality(entry, sigma: 1, tolerance: 0, flatMin: 0.5, flatMax: 1.5, to: quality)
    #expect(report.isValid && report.rejected > 0 && report.rejected < 120)
    var draft = ready(f.draft)
    draft.badPixels = try await f.library.inspectBadPixels(quality.appendingPathComponent("bad-pixels.fits"))
    #expect(draft.isReady && draft.validBadPixels)
    let saved = f.root.appendingPathComponent("draft.json")
    try await f.library.saveDraft(draft, to: saved)
    let restored = try await f.library.openDraft(saved)
    #expect(restored.badPixels == draft.badPixels)
    let job = f.root.appendingPathComponent("job")
    let partial = try await f.library.runCalibration(restored, to: job, maximumFrames: 1)
    #expect(partial.matches(restored) && partial.badPixels?.sha == draft.badPixels?.sha)
    let complete = try await f.library.resumeCalibration(job, expected: partial)
    #expect(complete.completed == 4 && complete.matches(restored))
    let path = quality.appendingPathComponent("bad-pixels.fits")
    try Data("changed".utf8).write(to: path)
    await #expect(throws: (any Error).self) { try await f.library.openDraft(saved) }
    await #expect(throws: (any Error).self) { try await f.library.resumeCalibration(job, expected: complete) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationStackProjectCopiesOnlyScienceAndVerifiesOfflineInputs() async throws {
    let f = try await jobFixture(), fm = FileManager.default
    defer { try? fm.removeItem(at: f.root) }
    let lights = try (0..<4).map { i in
        let url = f.root.appendingPathComponent("light-\(i).fits")
        try jobFITS(url, role: "Light", value: Float(100 + i), exposure: 60, day: i < 2 ? 15 : 16, sequence: i)
        return url
    }
    let entries = try await f.library.list()
    let review = try await f.library.plan(lights: lights, entries: entries, tolerance: 1)
    let draft = ready(try CalibrationJobDraft(review: review, entries: entries))
    let job = f.root.appendingPathComponent("job")
    let report = try await f.library.runCalibration(draft, to: job)
    let plan = try await f.library.stackPlan(directory: job, expected: report)
    let groups = Set(plan.groups.map(\.id))
    #expect(plan.groups.count == 2)
    #expect(try plan.frames(in: groups).count == 4)
    #expect(throws: (any Error).self) { try plan.frames(in: [plan.groups[0].id]) }
    let destination = f.root.appendingPathComponent("Stack project")
    _ = try await f.library.createStackProject(plan, groups: groups, name: "Stack project", destination: destination)
    let project = try await ProjectRepository().load(from: destination)
    #expect(project.assets.count == 4 && project.assets.allSatisfy { $0.role == .light })
    #expect(project.deepSkySettings?.values["calibration.sensor-pattern"] == "off")
    for (i, asset) in project.assets.enumerated() {
        #expect(try Data(contentsOf: asset.originalURL) == Data(contentsOf: job.appendingPathComponent(report.frames[i].directory + "/calibrated.fits")))
        #expect(!asset.originalURL.path.contains(".calibration-stack-"))
    }
    await #expect(throws: (any Error).self) {
        try await f.library.createStackProject(plan, groups: groups, name: "Stack project", destination: destination)
    }
    try fm.removeItem(at: job)
    for light in lights { try fm.removeItem(at: light) }
    try await f.library.verifyStackProject(project)
    let science = project.assets[0].originalURL
    try Data("changed".utf8).write(to: science)
    await #expect(throws: (any Error).self) { try await f.library.verifyStackProject(project) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationStackProjectRejectsMixedExposureGroups() async throws {
    let f = try await jobFixture(); defer { try? FileManager.default.removeItem(at: f.root) }
    let job = f.root.appendingPathComponent("job")
    let report = try await f.library.runCalibration(ready(f.draft), to: job)
    let plan = try await f.library.stackPlan(directory: job, expected: report)
    #expect(throws: (any Error).self) { try plan.frames(in: Set(plan.groups.map(\.id))) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationJobAssignmentsKeepAmbiguityExplicitAndAllowPerFrameChoices() async throws {
    let f = try await jobFixture(); defer { try? FileManager.default.removeItem(at: f.root) }
    var draft = f.draft
    #expect(draft.readyCount == 0 && !draft.isReady && draft.review.groups.count == 2)
    draft.fillUniqueCandidates()
    #expect(draft.choices.allSatisfy { $0[0] == CalibrationJobDraft.unresolved && $0[1] >= 0 && $0[2] >= 0 })
    #expect(throws: (any Error).self) { try draft.manifest() }
    draft.assign(CalibrationJobDraft.omitted, role: 0, frames: Array(draft.choices.indices))
    #expect(draft.isReady && draft.readyCount == 4)
    let wrongDark = draft.choices[2][1], original = draft.choices[0][1]
    draft.assign(wrongDark, role: 1, frames: [0]); #expect(draft.choices[0][1] == original)
    draft.assign(CalibrationJobDraft.omitted, role: 1, frames: [0]); #expect(draft.issue(frame: 0) == "bias-or-dark")
    let bias = try #require(draft.candidates(frame: 0, role: 0).first)
    draft.assign(bias, role: 0, frames: [0]); #expect(draft.isReady)
    #expect(draft.choices[1][0] == CalibrationJobDraft.omitted)
    draft.memoryMiB = 3; #expect(!draft.isReady); draft.memoryMiB = 512
    draft.workers = 33; #expect(!draft.isReady); draft.workers = 4
    draft.choices[1][1] = bias; #expect(!draft.isReady)
    #expect(throws: (any Error).self) { try draft.manifest() }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationJobAppServiceResumesAndInspectsOfflineWithoutRewritingScience() async throws {
    let f = try await jobFixture(), fm = FileManager.default; defer { try? fm.removeItem(at: f.root) }
    let draft = ready(f.draft), job = f.root.appendingPathComponent("job")
    let original = try draft.review.lights.map { try Data(contentsOf: URL(fileURLWithPath: $0.path)) }
    let first = try await f.library.runCalibration(draft, to: job, maximumFrames: 1)
    #expect(first.isValid && first.matches(draft) && first.completed == 1 && first.sourcesVerified)
    let science = job.appendingPathComponent("frame-000001/calibrated.fits"), bytes = try Data(contentsOf: science)
    let date = try science.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate
    let moved = URL(fileURLWithPath: draft.review.lights[3].path), backup = f.root.appendingPathComponent("moved.fits")
    try fm.moveItem(at: moved, to: backup)
    let offline = try await f.library.inspectCalibration(job)
    #expect(offline.completed == 1 && !offline.sourcesVerified && offline.jobSHA == first.jobSHA)
    await #expect(throws: (any Error).self) { try await f.library.resumeCalibration(job, expected: offline) }
    try fm.moveItem(at: backup, to: moved)
    let final = try await f.library.resumeCalibration(job, expected: offline)
    #expect(final.completed == 4 && final.status == "complete" && final.sourcesVerified && final.matches(draft))
    #expect(try Data(contentsOf: science) == bytes)
    #expect(try science.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate == date)
    #expect(try draft.review.lights.map { try Data(contentsOf: URL(fileURLWithPath: $0.path)) } == original)
    #expect(try await f.library.inspectCalibration(job).isValid)
    var malformed = final; malformed.completed = 2; #expect(!malformed.isValid)
    malformed = final; malformed.frames[0].directory = "../escape"; #expect(!malformed.isValid)
    malformed = final; malformed.frames[1].sourceSHA = final.frames[0].sourceSHA; #expect(!malformed.isValid)
    malformed = final; malformed.frames[0].products.removeValue(forKey: "valid.fits"); #expect(!malformed.isValid)
    malformed = final; malformed.frames[0].masters["dark"] = .some("unknown"); #expect(!malformed.isValid)
    malformed = final; malformed.productsVerified = false; #expect(!malformed.isValid)
    malformed = final; malformed.frames[0].group = Int.max; #expect(!malformed.isValid)
    malformed = final; malformed.jobSHA = String(repeating: "0", count: 64)
    await #expect(throws: (any Error).self) { try await f.library.resumeCalibration(job, expected: malformed) }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationJobAppRejectsStaleMasterRecordsAndChangedSourceBeforePublication() async throws {
    let f = try await jobFixture(), fm = FileManager.default; defer { try? fm.removeItem(at: f.root) }
    let draft = ready(f.draft), entry = draft.entries[draft.usedMasters[0]], job = f.root.appendingPathComponent("job")
    try await f.library.archive(entry)
    await #expect(throws: (any Error).self) { try await f.library.runCalibration(draft, to: job) }
    #expect(!fm.fileExists(atPath: job.path))
    let archived = try #require(try await f.library.list(archived: true).first)
    try await f.library.archive(archived)
    let light = URL(fileURLWithPath: draft.review.lights[0].path)
    try jobFITS(light, role: "Light", value: 500)
    await #expect(throws: (any Error).self) { try await f.library.runCalibration(draft, to: job) }
    #expect(!fm.fileExists(atPath: job.path))
}

@MainActor private func waitForJob(_ model: CalibrationJobSession) async throws {
    let deadline = ContinuousClock.now + .seconds(30)
    while model.busy && ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(20)) }
    #expect(!model.busy)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
@MainActor func calibrationJobSessionReopensRejectsTamperingAndClearsStaleResults() async throws {
    let f = try await jobFixture(), fm = FileManager.default; defer { try? fm.removeItem(at: f.root) }
    let draft = ready(f.draft), job = f.root.appendingPathComponent("job")
    let first = try await f.library.runCalibration(draft, to: job, maximumFrames: 1)
    let model = CalibrationJobSession(library: f.library, launch: .init(directory: job))
    model.refresh(); try await waitForJob(model)
    #expect(model.report?.completed == 1 && model.canResume && model.error == nil)
    model.resume(); try await waitForJob(model)
    #expect(model.report?.status == "complete" && model.report?.sourcesVerified == true)
    let path = job.appendingPathComponent("frame-000001/report.json")
    try Data("tampered".utf8).write(to: path)
    model.refresh(); #expect(model.report == nil && !model.canResume)
    try await waitForJob(model)
    #expect(model.report == nil && model.error != nil && !model.canResume)
    let fresh = CalibrationJobSession(library: f.library, launch: .init(draft: draft))
    fresh.start(at: job)
    #expect(fresh.directory == nil && fresh.error != nil && !fresh.busy)
    #expect(first.completed == 1)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
@MainActor func calibrationJobSessionCancellationWaitsForExitAndRetainsPublishedFrame() async throws {
    let fm = FileManager.default, cli = try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])
    let wrapper = fm.temporaryDirectory.appendingPathComponent("CalibrationCancel-\(UUID())")
    try fm.createDirectory(at: wrapper, withIntermediateDirectories: false); defer { try? fm.removeItem(at: wrapper) }
    let executable = wrapper.appendingPathComponent("photonstack"), marker = wrapper.appendingPathComponent("ready")
    func shell(_ text: String) -> String { "'" + text.replacingOccurrences(of: "'", with: "'\\''") + "'" }
    let script = """
    #!/bin/bash
    if [ "$1" = calibration-run ]; then
      \(shell(cli)) "$@" --max-frames 1 || exit $?
      trap 'exit 143' TERM
      printf ready > \(shell(marker.path))
      while true; do /bin/sleep 0.1; done
    else
      exec \(shell(cli)) "$@"
    fi
    """
    try script.write(to: executable, atomically: true, encoding: .utf8)
    try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
    let f = try await jobFixture(service: CLIProcessingService(executableURL: executable)); defer { try? fm.removeItem(at: f.root) }
    let model = CalibrationJobSession(library: f.library, launch: .init(draft: ready(f.draft)))
    let job = f.root.appendingPathComponent("job"); model.start(at: job)
    let deadline = ContinuousClock.now + .seconds(30)
    while !fm.fileExists(atPath: marker.path) && ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(20)) }
    #expect(fm.fileExists(atPath: marker.path) && model.busy)
    model.accept(.init(jobID: UUID(), task: "calibration-run", command: "calibration-run", stage: "calibrate", progress: 1))
    #expect(model.progress == nil)
    model.accept(.init(jobID: model.operationID, task: "calibration-run", command: "calibration-run", stage: "calibrate", progress: 0.4))
    #expect(model.progress?.progress == 0.4)
    model.cancel(); #expect(model.busy && model.cancelling && !model.canResume)
    try await waitForJob(model)
    #expect(model.stopped && model.report?.completed == 1 && model.report?.sourcesVerified == false && model.canResume && model.error == nil)
    let bytes = try Data(contentsOf: job.appendingPathComponent("frame-000001/calibrated.fits"))
    model.resume(); try await waitForJob(model)
    #expect(model.report?.completed == 4 && model.report?.sourcesVerified == true && !model.stopped)
    #expect(try Data(contentsOf: job.appendingPathComponent("frame-000001/calibrated.fits")) == bytes)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationJobPreparationRequiresExplicitStatesAndBiasDependencies() async throws {
    let f = try await jobFixture(); defer { try? FileManager.default.removeItem(at: f.root) }
    var entries = f.draft.entries
    for i in entries.indices where entries[i].receipt?.role == "dark" { entries[i].receipt?.biasState = "unknown" }
    var draft = ready(try CalibrationJobDraft(review: f.draft.review, entries: entries))
    #expect(draft.issue(frame: 0) == "preparation" && !draft.isReady)
    for i in entries.indices where entries[i].receipt?.role == "dark" { entries[i].receipt?.biasState = "removed" }
    draft = ready(try CalibrationJobDraft(review: f.draft.review, entries: entries))
    #expect(draft.issue(frame: 0) == "dark-needs-bias" && !draft.isReady)
    let bias = try #require(draft.candidates(frame: 0, role: 0).first)
    draft.assign(bias, role: 0, frames: Array(draft.choices.indices)); #expect(draft.isReady)
    entries = f.draft.entries
    for i in entries.indices where entries[i].receipt?.role == "flat" { entries[i].receipt?.biasState = "included" }
    draft = ready(try CalibrationJobDraft(review: f.draft.review, entries: entries))
    #expect(draft.issue(frame: 0) == "flat-needs-bias" && !draft.isReady)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
@MainActor func calibrationJobPreflightFailureReturnsToAssignmentWithoutFalseCompletion() async throws {
    let f = try await jobFixture(), fm = FileManager.default; defer { try? fm.removeItem(at: f.root) }
    let draft = ready(f.draft), light = URL(fileURLWithPath: draft.review.lights[0].path), before = try Data(contentsOf: light)
    try jobFITS(light, role: "Light", value: 999)
    let model = CalibrationJobSession(library: f.library, launch: .init(draft: draft)), job = f.root.appendingPathComponent("job")
    model.start(at: job); try await waitForJob(model)
    #expect(model.error != nil && model.report == nil && !model.canResume && !fm.fileExists(atPath: job.path))
    model.retryPreparation(); #expect(model.directory == nil && model.canStart && model.error == nil)
    try before.write(to: light)
    model.start(at: job); try await waitForJob(model)
    #expect(model.report?.status == "complete" && model.report?.sourcesVerified == true && model.error == nil)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
@MainActor func calibrationJobSessionDoesNotAdoptAnUnrelatedConcurrentDestination() async throws {
    let f = try await jobFixture(), fm = FileManager.default; defer { try? fm.removeItem(at: f.root) }
    let draft = ready(f.draft)
    var other = draft; other.assign(CalibrationJobDraft.omitted, role: 2, frames: Array(other.choices.indices))
    let existing = f.root.appendingPathComponent("different-job")
    _ = try await f.library.runCalibration(other, to: existing, maximumFrames: 1)
    let target = f.root.appendingPathComponent("job"), model = CalibrationJobSession(library: f.library, launch: .init(draft: draft))
    model.start(at: target)
    // Creation races the async preflight after the destination was selected.
    try fm.copyItem(at: existing, to: target)
    let before = try Data(contentsOf: target.appendingPathComponent("job.txt"))
    try await waitForJob(model)
    #expect(model.error != nil && model.report == nil && !model.canResume)
    #expect(try Data(contentsOf: target.appendingPathComponent("job.txt")) == before)
    #expect(try await f.library.inspectCalibration(target).completed == 1)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationBatchResumesCalibrationAndRetainsFailedGroupAttempts() async throws {
    let f = try await jobFixture(), fm = FileManager.default; defer { try? fm.removeItem(at: f.root) }
    let job = f.root.appendingPathComponent("job"), target = f.root.appendingPathComponent("batch")
    let partial = try await f.library.runCalibration(ready(f.draft), to: job, maximumFrames: 1)
    let science = try Data(contentsOf: job.appendingPathComponent("frame-000001/calibrated.fits"))
    let created = try await f.library.createBatch(calibration: job, expected: partial, destination: target, backend: .cpu)
    #expect(created.isValid && !created.calibrated && created.items.isEmpty)
    await #expect(throws: (any Error).self) { try await f.library.createBatch(calibration: job, expected: partial, destination: target) }
    let run = try await f.library.runBatch(target)
    #expect(run.calibrated && !run.complete && run.items.count == 2)
    #expect(run.items.allSatisfy { $0.count == 2 && $0.status == "failed" && $0.attempt != nil && !$0.message.isEmpty })
    #expect(try Data(contentsOf: job.appendingPathComponent("frame-000001/calibrated.fits")) == science)
    #expect(try await f.library.inspectCalibration(job).completed == 4)
    let reopened = try await f.library.inspectBatch(target)
    #expect(reopened.items.map(\.attempt) == run.items.map(\.attempt))
    let retry = try await f.library.runBatch(target)
    #expect(Set(retry.items.compactMap(\.attempt)).isDisjoint(with: run.items.compactMap(\.attempt)))
    for attempt in run.items.compactMap(\.attempt) { #expect(fm.fileExists(atPath: target.appendingPathComponent(attempt).path)) }
    let journal = target.appendingPathComponent("batch.json")
    var value = try #require(JSONSerialization.jsonObject(with: Data(contentsOf: journal)) as? [String: Any])
    value["jobSHA"] = String(repeating: "0", count: 64)
    try JSONSerialization.data(withJSONObject: value).write(to: journal)
    await #expect(throws: (any Error).self) { try await f.library.runBatch(target) }
}

private func starJobFITS(_ url: URL, role: String, sequence: Int) throws {
    try jobFITS(url, role: role, value: 0, exposure: role == "Bias" ? 0 : 60, day: sequence < 2 ? 15 : 16, sequence: sequence)
    let data = try Data(contentsOf: url)
    var header = String(decoding: data.prefix(2880), as: UTF8.self)
    for (key,value) in [("NAXIS1","256"),("NAXIS2","192")] {
        let start = try #require(header.range(of: key.padding(toLength: 8, withPad: " ", startingAt: 0) + "= ")?.lowerBound)
        let end = header.index(start, offsetBy: 80)
        header.replaceSubrange(start..<end, with: (key.padding(toLength: 8, withPad: " ", startingAt: 0) + "= " + value).padding(toLength: 80, withPad: " ", startingAt: 0))
    }
    var output = Data(header.utf8)
    var stars: [(Double, Double, Double)] = []
    for n in 0..<35 {
        let x = 20 + (n * 53) % 210
        let y = 20 + (n * 31) % 150
        stars.append((Double(x), Double(y), Double(500 + n * 77)))
    }
    for y in 0..<192 { for x in 0..<256 {
        var pixel = 10.0
        if role == "Light" {
            pixel += 100 + 0.01 * Double(x) + 0.02 * Double(y)
            for (sx,sy,flux) in stars {
                let dx = Double(x) - sx - Double(sequence) * 0.6, dy = Double(y) - sy + Double(sequence) * 0.35
                pixel += flux * exp(-(dx*dx+dy*dy)/4.5)
            }
        }
        var bits = Float(pixel).bitPattern.bigEndian
        withUnsafeBytes(of: &bits) { output.append(contentsOf: $0) }
    } }
    output.append(Data(repeating: 0, count: (2880 - output.count % 2880) % 2880))
    try output.write(to: url)
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func calibrationBatchStacksCompatibleDatesAndSkipsVerifiedResults() async throws {
    let fm = FileManager.default, cli = try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])
    let root = fm.temporaryDirectory.appendingPathComponent("CalibrationBatch-\(UUID())")
    try fm.createDirectory(at: root, withIntermediateDirectories: false); defer { try? fm.removeItem(at: root) }
    let library = CalibrationLibrary(root: root.appendingPathComponent("library"), service: CLIProcessingService(executableURL: URL(fileURLWithPath: cli)))
    let bias = root.appendingPathComponent("bias.fits"); try starJobFITS(bias, role: "Bias", sequence: 0)
    _ = try await library.importMaster(bias, label: "Bias", role: "bias", biasState: "included", notes: "Synthetic star field")
    let lights = try (0..<4).map { i in
        let url = root.appendingPathComponent("light-\(i).fits"); try starJobFITS(url, role: "Light", sequence: i); return url
    }
    let entries = try await library.list(), review = try await library.plan(lights: lights, entries: entries, tolerance: 1)
    var draft = try CalibrationJobDraft(review: review, entries: entries)
    draft.fillUniqueCandidates()
    for role in [1,2] { draft.assign(CalibrationJobDraft.omitted, role: role, frames: Array(0..<4)) }
    let job = root.appendingPathComponent("job"), batchURL = root.appendingPathComponent("batch")
    let report = try await library.runCalibration(draft, to: job)
    #expect(Set(report.frames.map(\.group)).count == 2)
    _ = try await library.createBatch(calibration: job, expected: report, destination: batchURL, backend: .cpu)
    let batch = try await library.runBatch(batchURL)
    #expect(batch.complete, "\(batch.items.map(\.message))")
    #expect(batch.items.count == 1 && batch.items[0].count == 4 && batch.items[0].groups.count == 2)
    let attempt = try #require(batch.items.first?.attempt)
    let projectURL = batchURL.appendingPathComponent(attempt + "/project")
    let project = try await ProjectRepository().load(from: projectURL)
    #expect(project.deepSkySettings?.lastReport?.success == true && project.workspaceState?.previewURL != nil)
    try await library.verifyStackProject(project)
    let before = try Data(contentsOf: batchURL.appendingPathComponent("batch.json"))
    // A completed queue can be verified without mounted original files.
    try fm.moveItem(at: job, to: root.appendingPathComponent("offline-job"))
    let resumed = try await library.runBatch(batchURL)
    #expect(resumed.complete && resumed.items[0].attempt == attempt)
    #expect(try Data(contentsOf: batchURL.appendingPathComponent("batch.json")) == before)
    let product = try #require(project.workspaceState?.previewURL)
    try Data("changed".utf8).write(to: product)
    await #expect(throws: (any Error).self) { try await library.inspectBatch(batchURL) }
}

@Test func calibrationBatchFailureMessageKeepsReasonInsteadOfRecipeDump() {
    let error = ProcessingServiceError.commandFailed(exitCode: 1, stderr: "{\"recipe\":\"large recipe\",\"errorCode\":\"SelectionNeedsReview\",\"message\":\"Too few stars\"}")
    #expect(CalibrationBatch.failureMessage(error) == "SelectionNeedsReview: Too few stars")
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil))
func cfaDrizzleAppServiceKeepsCoverageThroughRefinishing() async throws {
    let fm = FileManager.default
    let cli = try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: cli))
    let root = fm.temporaryDirectory.appendingPathComponent("CfaDrizzle-\(UUID())")
    try fm.createDirectory(at: root, withIntermediateDirectories: false)
    defer { try? fm.removeItem(at: root) }
    let lights = try (0..<8).map { i in
        let url = root.appendingPathComponent("light-\(i).fits")
        try starJobFITS(url, role: "Light", sequence: i)
        return url
    }
    let schema = try await service.deepSkySchema()
    var settings = DeepSkySettings()
    settings.setCompatibleValue("on", for: "stack.cfa-drizzle", schema: schema)
    settings.values["registration.minimum-matches"] = "4"
    settings.values["selection.apply"] = "off"
    settings.values["stack.drizzle-scale"] = "1.5"
    let recipe = root.appendingPathComponent("recipe.txt")
    try settings.recipe(schema: schema, inputs: lights).write(to: recipe, atomically: true, encoding: .utf8)
    let report = try await service.deepSky(recipe: recipe, outputDirectory: root.appendingPathComponent("stack"), analyzeOnly: false, computeBackend: .cpu)
    #expect(report.success && report.drizzlePaths.count == 2)
    #expect(report.drizzleCoverage?.count == 3 && (report.drizzleCompleteCoverage ?? 0) > 0.9)
    let master = URL(fileURLWithPath: report.master)
    let bytes = try Data(contentsOf: master)
    settings.values["develop.saturation"] = "0.7"
    try settings.recipe(schema: schema, inputs: lights).write(to: recipe, atomically: true, encoding: .utf8)
    let finished = try await service.finishDeepSky(recipe: recipe, master: master, outputDirectory: root.appendingPathComponent("finish"), previous: report)
    #expect(finished.drizzlePaths == report.drizzlePaths)
    #expect(finished.drizzleCoverage == report.drizzleCoverage && finished.drizzleCompleteCoverage == report.drizzleCompleteCoverage)
    #expect(try Data(contentsOf: master) == bytes)
    try fm.removeItem(atPath: try #require(report.drizzleWeights))
    await #expect(throws: (any Error).self) {
        try await service.finishDeepSky(recipe: recipe, master: master, outputDirectory: root.appendingPathComponent("missing-weight"), previous: report)
    }
}
