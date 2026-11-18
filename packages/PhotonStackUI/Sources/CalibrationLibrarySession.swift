import Foundation
import Combine

@MainActor
final class CalibrationLibrarySession: ObservableObject {
    let library: CalibrationLibrary
    @Published private(set) var entries: [CalibrationLibrary.Entry] = []
    @Published private(set) var report: CalibrationPlanReport?
    @Published private(set) var reportIsCurrent = false
    @Published private(set) var verified: CalibrationAcquisition?
    @Published private(set) var busy = false
    @Published private(set) var error: String?
    @Published var archived = false
    @Published var selection: String?
    @Published var groupSelection: Int?
    @Published var frameSelection: Int?
    @Published var selectedMasters: Set<String> = []
    @Published private(set) var lights: [URL]
    @Published var tolerance = "1"
    private var task: Task<Void, Never>?
    private var generation = UUID()
    init(library: CalibrationLibrary, lights: [URL]) {
        self.library = library
        self.lights = Array(Set(lights.filter { ["fit", "fits", "fts"].contains($0.pathExtension.lowercased()) })).sorted { $0.path < $1.path }
    }
    var selectedEntry: CalibrationLibrary.Entry? { entries.first { $0.id == selection } }
    var parsedTolerance: Double? {
        guard let n = Double(tolerance.trimmingCharacters(in: .whitespaces)), n.isFinite, (0...10).contains(n) else { return nil }
        return n
    }
    func invalidate() { reportIsCurrent = false; groupSelection = nil; frameSelection = nil; verified = nil }
    func verifiedSelectionChanged() { verified = nil }
    func clearError() { error = nil }
    func setScannedLights(_ urls: [URL]) { lights = urls; invalidate() }
    func replaceLights(_ urls: [URL]) {
        guard !busy else { return }
        lights = Array(Set(urls)).sorted { $0.path < $1.path }; invalidate()
    }
    func cancel() {
        generation = UUID(); task?.cancel(); task = nil; busy = false
    }
    func perform(_ work: @escaping @MainActor () async throws -> Void) {
        guard !busy else { return }
        let id = UUID(); generation = id; busy = true; error = nil
        task = Task { [weak self] in
            guard let self else { return }
            do { try await work(); try Task.checkCancellation() }
            catch { if !Task.isCancelled && generation == id { self.error = error.localizedDescription } }
            if generation == id { busy = false; task = nil }
        }
    }
    func reload() async throws {
        let entries = try await library.list(archived: archived)
        try Task.checkCancellation()
        let known = Set(self.entries.filter { !$0.archived }.map(\.id))
        let available = Set(entries.filter { $0.receipt != nil && !$0.archived }.map(\.id))
        let candidateSelection = selectedMasters.intersection(available).union(available.subtracting(known))
        self.entries = entries
        selectedMasters = candidateSelection
        if !entries.contains(where: { $0.id == selection }) { selection = entries.first?.id }
        invalidate()
    }
    func check() {
        guard let tolerance = parsedTolerance, !archived else { return }
        let inputs = lights, candidates = entries.filter { selectedMasters.contains($0.id) }
        invalidate()
        perform {
            let value = try await self.library.plan(lights: inputs, entries: candidates, tolerance: tolerance)
            try Task.checkCancellation()
            self.report = value; self.reportIsCurrent = true; self.groupSelection = value.groups.isEmpty ? nil : 0
            self.frameSelection = value.groups.first?.members.first
        }
    }
    func inspectSelected() {
        guard let entry = selectedEntry else { return }
        verified = nil
        perform {
            let value = try await self.library.verify(entry)
            try Task.checkCancellation()
            if self.selection == entry.id { self.verified = value }
        }
    }
}
