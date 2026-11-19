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
