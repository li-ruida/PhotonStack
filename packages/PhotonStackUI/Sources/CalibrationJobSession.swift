import Foundation
import Combine
import PhotonStackProcessing

struct CalibrationJobLaunch: Identifiable {
    let id = UUID()
    var draft: CalibrationJobDraft?
    var directory: URL?
}

@MainActor
final class CalibrationJobSession: ObservableObject {
    let library: CalibrationLibrary
    @Published var draft: CalibrationJobDraft?
    @Published private(set) var directory: URL?
    @Published private(set) var report: CalibrationJobReport?
    @Published private(set) var busy = false
    @Published private(set) var cancelling = false
    @Published private(set) var stopped = false
    @Published private(set) var error: String?
    @Published private(set) var progress: ProcessingProgressEvent?
    private(set) var operationID = UUID()
    private var boundJobSHA: String?
    private var task: Task<Void, Never>?
    init(library: CalibrationLibrary, launch: CalibrationJobLaunch) {
        self.library = library; draft = launch.draft; directory = launch.directory
    }
    var canStart: Bool { !busy && directory == nil && draft?.isReady == true }
    var canResume: Bool { !busy && directory != nil && report != nil }
    func accept(_ event: ProcessingProgressEvent) {
        guard busy, event.jobID == operationID, ["calibration-run", "calibration-resume", "calibration-job-inspect"].contains(event.command), event.progress.isFinite else { return }
        progress = event
    }
    func cancel() {
        guard busy else { return }
        cancelling = true; task?.cancel()
        // Do not unlock actions until the child has terminated and its journal
        // has been re-inspected. Cancellation can publish one last full frame.
    }
    func start(at url: URL) {
        guard canStart, let draft else { return }
        guard !FileManager.default.fileExists(atPath: url.path), (try? url.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true else {
            error = "Choose a new task folder. Open an existing task to inspect or resume it."; return
        }
        directory = url
        perform { try await self.library.runCalibration(draft, to: url) }
    }
    func refresh() {
        guard !busy, let directory else { return }
        perform(recoverOnFailure: false) { try await self.library.inspectCalibration(directory) }
    }
    func resume() {
        guard canResume, let directory, let report else { return }
        perform { try await self.library.resumeCalibration(directory, expected: report) }
    }
    func retryPreparation() {
        guard !busy, report == nil, let directory, !FileManager.default.fileExists(atPath: directory.path) else { return }
        self.directory = nil; boundJobSHA = nil; error = nil; stopped = false
    }
    private func acceptReport(_ value: CalibrationJobReport) throws {
        guard (boundJobSHA == nil || boundJobSHA == value.jobSHA), draft.map({ value.matches($0) }) ?? true else {
            throw CalibrationAcquisition.failure("The folder contains a different task. Reopen it explicitly to review its inputs.")
        }
        boundJobSHA = value.jobSHA; report = value
    }
    private func perform(recoverOnFailure: Bool = true, _ operation: @escaping @MainActor () async throws -> CalibrationJobReport) {
        guard !busy else { return }
        operationID = UUID(); let id = operationID
        busy = true; cancelling = false; stopped = false; error = nil; progress = nil; report = nil
        task = Task {
            do {
                let value = try await ProcessingProgressContext.$jobID.withValue(id) { try await operation() }
                try acceptReport(value)
            } catch {
                let wasCancelled = Task.isCancelled || error is CancellationError
                if !wasCancelled { self.error = error.localizedDescription }
                stopped = wasCancelled
                if recoverOnFailure, let directory {
                    let library = library
                    // A cancelled task cannot run a new CLI through the same
                    // cancellation context. Verify only after the child exits.
                    let verification = Task.detached { try await library.inspectCalibration(directory) }
                    do { try acceptReport(try await verification.value) }
                    catch {
                        if FileManager.default.fileExists(atPath: directory.path), self.error != error.localizedDescription {
                            self.error = [self.error, error.localizedDescription].compactMap { $0 }.joined(separator: "\n")
                        }
                    }
                }
            }
            busy = false; cancelling = false; task = nil
        }
    }
}
