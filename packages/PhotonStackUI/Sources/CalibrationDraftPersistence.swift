import Foundation

/// Decisions are persisted, but matching is revalidated before reuse.
private struct CalibrationSavedDraft: Codable {
    var schema = 1
    var review: CalibrationPlanReport
    var masters: [CalibrationLibrary.Receipt]
    var choices: [[Int]]
    var memoryMiB: Int
    var workers: Int
    var badPixels: CalibrationAcquisition?
    var valid: Bool {
        guard schema == 1, review.isValid, badPixels?.isValid ?? true, masters.count == review.masters.count,
              choices.count == review.lights.count, (4...8192).contains(memoryMiB), (1...32).contains(workers) else { return false }
        for (i, master) in masters.enumerated() {
            guard master.isValid, master.acquisition.sha == review.masters[i].acquisition.sha,
                  master.role == review.masters[i].role else { return false }
        }
        for (i, row) in choices.enumerated() {
            guard row.count == 3 else { return false }
            for (role, value) in row.enumerated() {
                guard value == CalibrationJobDraft.omitted || value == CalibrationJobDraft.unresolved ||
                        review.frameChoices[i].roles[CalibrationPlanReport.roles[role]]!.compatibleMasters.contains(value) else { return false }
            }
        }
        return true
    }
}

extension CalibrationLibrary {
    func saveDraft(_ draft: CalibrationJobDraft, to file: URL) throws {
        let value = CalibrationSavedDraft(review: draft.review, masters: draft.entries.compactMap(\.receipt),
                                          choices: draft.choices, memoryMiB: draft.memoryMiB, workers: draft.workers, badPixels: draft.badPixels)
        guard value.valid, file.pathExtension.lowercased() == "json" else { throw fail("Choose a JSON file for a valid assignment draft.") }
        let path = file.standardizedFileURL.resolvingSymlinksInPath().path
        guard !path.hasPrefix(root.path + "/"),
              !(draft.review.lights + draft.review.masters.map(\.acquisition)).contains(where: { $0.path == path }) else {
            throw fail("Save the draft outside the calibration library and source files.")
        }
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(value)
        guard data.count <= 32 * 1024 * 1024 else { throw fail("Split this batch into smaller drafts.") }
        try Task.checkCancellation(); try data.write(to: file, options: .atomic)
    }
    func openDraft(_ file: URL) async throws -> CalibrationJobDraft {
        try regular(file)
        guard (try file.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? Int.max) <= 32 * 1024 * 1024 else { throw fail("Draft is too large.") }
        let data = try Data(contentsOf: file)
        guard data.count <= 32 * 1024 * 1024 else { throw fail("Draft grew during reading.") }
        let saved = try JSONDecoder().decode(CalibrationSavedDraft.self, from: data)
        guard saved.valid else { throw fail("Invalid calibration draft.") }
        let available = try list()
        let entries = try saved.masters.map { receipt in
            guard let entry = available.first(where: { $0.id == receipt.id && !$0.archived }), entry.receipt == receipt else {
                throw fail("A draft master is missing, archived or changed. Review its assignments again.")
            }
            return entry
        }
        let current = try await plan(lights: saved.review.lights.map { URL(fileURLWithPath: $0.path) },
                                     entries: entries, tolerance: saved.review.temperatureTolerance)
        let oldLights = Dictionary(uniqueKeysWithValues: saved.review.lights.enumerated().map { ($0.element.sha, $0.offset) })
        let masterIndices = Dictionary(uniqueKeysWithValues: current.masters.enumerated().map { ($0.element.acquisition.sha, $0.offset) })
        var draft = try CalibrationJobDraft(review: current, entries: entries)
        for (i, light) in current.lights.enumerated() {
            guard let old = oldLights[light.sha], light == saved.review.lights[old] else { throw fail("A draft light changed. Create a new review.") }
            for role in 0..<3 {
                let previous = saved.choices[old][role]
                let choice: Int
                if previous < 0 { choice = previous }
                else {
                    guard let mapped = masterIndices[saved.review.masters[previous].acquisition.sha],
                          draft.candidates(frame: i, role: role).contains(mapped) else { throw fail("A saved master no longer matches this frame.") }
                    choice = mapped
                }
                draft.choices[i][role] = choice
            }
        }
        draft.memoryMiB = saved.memoryMiB; draft.workers = saved.workers
        if let map = saved.badPixels {
            let currentMap = try await CalibrationAcquisition.inspect([URL(fileURLWithPath: map.path)], service: service)[0]
            guard currentMap == map else { throw fail("Saved defect map changed.") }
            draft.badPixels = currentMap
            guard draft.validBadPixels else { throw fail("Saved defect map is incompatible with the lights.") }
        }
        return draft
    }
}
