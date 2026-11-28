import Foundation
import Combine

@MainActor
final class CalibrationMasterBuildSession: ObservableObject {
    let library: CalibrationLibrary
    let entries: [CalibrationLibrary.Entry]
    @Published var frames: [URL] = []
    @Published var role = "dark"
    @Published var method = "median"
    @Published var correction = "bias"
    @Published var subtractBias = false
    @Published var biasID: String?
    @Published var darkFlatID: String?
    @Published var temperature = "1"
    @Published var minimum = "2"
    @Published var memory = "512"
    @Published var workers = "4"
    @Published var name = ""
    @Published var notes = ""
    @Published private(set) var draft: CalibrationMasterDraft?
    @Published private(set) var busy = false
    @Published private(set) var error: String?
    private var task: Task<Void, Never>?
    private var generation = UUID()
    init(library: CalibrationLibrary, entries: [CalibrationLibrary.Entry]) { self.library = library; self.entries = entries.filter { !$0.archived } }
    var biasEntries: [CalibrationLibrary.Entry] { entries.filter { $0.receipt?.role == "bias" && $0.receipt?.biasState == "included" } }
    var darkFlatEntries: [CalibrationLibrary.Entry] { entries.filter { ["dark", "dark-flat"].contains($0.receipt?.role ?? "") && ["included", "removed"].contains($0.receipt?.biasState ?? "") } }
    var usesDarkFlat: Bool { role == "flat" && correction == "dark-flat" }
    var needsBias: Bool {
        role != "bias" && (role == "flat" ? (!usesDarkFlat || darkFlatEntries.first { $0.id == darkFlatID }?.receipt?.biasState == "removed") : subtractBias)
    }
    var configuration: [String] { frames.map(\.path) + [role, method, correction, String(subtractBias), biasID ?? "", darkFlatID ?? "", temperature, minimum, memory, workers] }
    var request: CalibrationMasterBuildRequest? {
        guard let t = Double(temperature), let n = Int(minimum), let m = Int(memory), let w = Int(workers),
              !needsBias || biasID != nil, !usesDarkFlat || darkFlatID != nil else { return nil }
        let request = CalibrationMasterBuildRequest(frames: frames, role: role, method: method,
            biasID: needsBias ? biasID : nil, darkFlatID: usesDarkFlat ? darkFlatID : nil,
            temperatureTolerance: t, minimumSamples: n, memoryMiB: m, workers: w)
        return request.isValid ? request : nil
    }
    var currentDraft: CalibrationMasterDraft? { draft?.request == request ? draft : nil }
    var canSave: Bool { !busy && currentDraft != nil && CalibrationLibrary.validLabel(name) && notes.utf8.count <= 4096 }
    func invalidate() {
        if let old = draft { Task { await library.discardDraft(old.id) } }
        draft = nil; error = nil
    }
    // Only the task's completion may release busy and permit another operation.
    func cancel() { task?.cancel() }
    func close() { cancel(); invalidate() }
    func choose(_ urls: [URL]) {
        guard !busy else { return }
        invalidate(); generation = UUID(); let token = generation; busy = true
        task = Task {
            defer { if generation == token { busy = false; task = nil } }
            do {
                let files = try await library.scanLights(urls)
                try Task.checkCancellation()
                guard files.count <= 256 else { throw CalibrationAcquisition.failure("Choose at most 256 raw calibration frames.") }
                if generation == token { frames = files }
            } catch { if !Task.isCancelled && generation == token { self.error = error.localizedDescription } }
        }
    }
    func build() {
        guard !busy, let request else { return }
        invalidate(); generation = UUID(); let token = generation; busy = true
        task = Task {
            defer { if generation == token { busy = false; task = nil } }
            do {
                let value = try await library.buildMaster(request)
                if Task.isCancelled || generation != token { await library.discardDraft(value.id); return }
                draft = value
            } catch { if !Task.isCancelled && generation == token { self.error = error.localizedDescription } }
        }
    }
    func save(_ onSaved: @escaping @MainActor (String) async -> Void) {
        guard canSave, let draft = currentDraft else { return }
        generation = UUID(); let token = generation; busy = true; error = nil
        let label = name, note = notes
        task = Task {
            defer { if generation == token { busy = false; task = nil } }
            do {
                let id = try await library.saveDraft(draft.id, label: label, notes: note)
                guard generation == token else { return }
                if self.draft?.id == draft.id { self.draft = nil }
                try Task.checkCancellation()
                // Publication remains durable, but a cancelled sheet must not
                // clear newer state or invoke its completion callback.
                await onSaved(id)
            } catch { if !Task.isCancelled && generation == token { self.error = error.localizedDescription } }
        }
    }
}
