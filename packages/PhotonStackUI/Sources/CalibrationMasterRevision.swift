import Foundation
import Darwin

struct CalibrationMasterRevision: Codable, Equatable, Sendable {
    var date: Date
    var label, notes, biasState, quality, reason: String
    var previousLabel, previousNotes, previousBiasState: String
    var isValid: Bool {
        date.timeIntervalSince1970.isFinite && CalibrationLibrary.validLabel(label) &&
        CalibrationLibrary.validLabel(previousLabel) && notes.utf8.count <= 4096 && previousNotes.utf8.count <= 4096 &&
        ["unknown", "included", "removed"].contains(biasState) && ["unknown", "included", "removed"].contains(previousBiasState) &&
        ["unreviewed", "keep", "reject"].contains(quality) && CalibrationLibrary.validLabel(reason)
    }
}

extension CalibrationLibrary {
    func revise(_ entry: Entry, label: String, notes: String, biasState: String, quality: String, reason: String) async throws {
        _ = try await verify(entry)
        let dir = try location(entry)
        let lock = open(dir.appendingPathComponent(".review.lock").path, O_CREAT | O_RDWR | O_NOFOLLOW, 0o600)
        guard lock >= 0 else { throw fail("Cannot lock master review.") }
        defer { close(lock) }
        guard flock(lock, LOCK_EX | LOCK_NB) == 0 else { throw fail("Another review is being saved. Reload before trying again.") }
        defer { flock(lock, LOCK_UN) }
        var record = try receipt(dir, id: entry.id)
        guard record == entry.receipt else { throw fail("Master record changed. Reload before editing.") }
        let revision = CalibrationMasterRevision(date: Date(), label: label, notes: notes, biasState: biasState,
            quality: quality, reason: reason, previousLabel: record.label, previousNotes: record.notes, previousBiasState: record.biasState)
        guard revision.isValid, record.reviewHistory?.count ?? 0 < 64,
              record.role != "bias" || biasState == "included",
              record.acquisition.text["biasState"].map({ $0 == biasState }) ?? true,
              record.construction == nil || record.biasState == biasState else {
            throw fail("Provide a reason and valid declaration consistent with the FITS header and construction record.")
        }
        record.label = label; record.notes = notes; record.biasState = biasState
        record.reviewHistory = (record.reviewHistory ?? []) + [revision]
        guard record.isValid else { throw fail("Invalid revised master record.") }
        let data = try JSONEncoder().encode(record)
        guard data.count <= 131072 else { throw fail("Review history is too large.") }
        try Task.checkCancellation(); try data.write(to: dir.appendingPathComponent("entry.json"), options: .atomic)
    }
}
