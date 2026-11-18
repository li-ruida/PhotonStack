import Foundation

struct CalibrationMasterDraft: Identifiable, Sendable {
    let id: UUID
    let directory: URL
    let report: CalibrationMasterReport
    let request: CalibrationMasterBuildRequest
}

extension CalibrationLibrary {
    func inspectConstructionFiles(_ dir: URL, expected: CalibrationMasterConstruction? = nil,
                                  verifySources: Bool = false) async throws -> CalibrationMasterReport {
        try directory(dir)
        for name in CalibrationMasterConstruction.names.union(["master.fits"]) { try regular(dir.appendingPathComponent(name)) }
        let report = try await CalibrationMasterReport.inspect(dir, service: service, verifySources: verifySources)
        guard expected == nil || report.construction == expected else { throw fail("Saved master construction files changed.") }
        return report
    }
    func inspectConstruction(_ entry: Entry, verifySources: Bool = false) async throws -> CalibrationMasterReport {
        let dir = try location(entry), record = try receipt(dir, id: entry.id)
        guard record == entry.receipt, let construction = record.construction else { throw fail("No saved construction record for this master.") }
        let report = try await inspectConstructionFiles(dir, expected: construction, verifySources: verifySources)
        let actual = try await CalibrationAcquisition.inspect([dir.appendingPathComponent("master.fits")], service: service)[0]
        guard actual == (try record.acquisition.at(dir.appendingPathComponent("master.fits"))), report.matches(actual), report.role == record.role else {
            throw fail("Construction record does not match the saved master.")
        }
        return report
    }
    func importConstruction(_ dir: URL, label: String, notes: String,
                            expected: CalibrationMasterReport? = nil) async throws -> String {
        let report = try await inspectConstructionFiles(dir, expected: expected?.construction)
        let source = dir.appendingPathComponent("master.fits")
        let acquisition = try await CalibrationAcquisition.inspect([source], service: service)[0]
        guard report.matches(acquisition), expected == nil || expected?.inspection.files == report.inspection.files else {
            throw fail("Master construction changed before import.")
        }
        let id = Self.identity(masterSHA: acquisition.sha, construction: report.construction)
        let record = Receipt(id: id, label: label.trimmingCharacters(in: .whitespacesAndNewlines), role: report.role,
                             biasState: report.biasState, notes: notes, importedAt: Date(), acquisition: acquisition,
                             construction: report.construction)
        guard record.isValid else { throw fail("Enter a valid master name and preparation notes.") }
        try fm.createDirectory(at: root, withIntermediateDirectories: true); try directory(root)
        let stage = root.appendingPathComponent(".construction-" + UUID().uuidString)
        try fm.createDirectory(at: stage, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: stage) }
        for name in CalibrationMasterConstruction.names.union(["master.fits"]) {
            try Task.checkCancellation(); try fm.copyItem(at: dir.appendingPathComponent(name), to: stage.appendingPathComponent(name))
        }
        let copiedReport = try await inspectConstructionFiles(stage, expected: report.construction)
        let copied = try await CalibrationAcquisition.inspect([stage.appendingPathComponent("master.fits")], service: service)[0]
        guard copiedReport.inspection.files == report.inspection.files, copied == (try acquisition.at(stage.appendingPathComponent("master.fits"))) else {
            throw fail("Master changed while copying its construction record.")
        }
        try JSONEncoder().encode(record).write(to: stage.appendingPathComponent("entry.json"), options: .atomic)
        try Task.checkCancellation()
        let target = root.appendingPathComponent(id)
        do { try publish(stage, to: target) }
        catch {
            guard fm.fileExists(atPath: target.path) else { throw error }
            try directory(target)
            let existing = try receipt(target, id: id)
            guard existing.construction == record.construction, existing.acquisition.sha == acquisition.sha,
                  existing.role == record.role, existing.biasState == record.biasState else { throw fail("Conflicting construction record already exists.") }
            _ = try await verify(Entry(id: id, archived: false, receipt: existing))
        }
        return id
    }
    func buildMaster(_ request: CalibrationMasterBuildRequest) async throws -> CalibrationMasterDraft {
        guard request.isValid else { throw fail("Choose 2–256 calibration frames and valid construction settings.") }
        func quote(_ path: String) throws -> String {
            guard !path.contains("\0"), !path.contains("\n"), !path.contains("\r") else { throw fail("Unsupported source filename.") }
            return "\"" + path.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
        }
        let available = try list(), canonicalInputs = try request.frames.map { try CalibrationAcquisition.canonicalPath($0) }
        guard Set(canonicalInputs).count == request.frames.count else { throw fail("Choose distinct calibration frames.") }
        var manifest = "PhotonStack-SensorMaster-1\nrole \(request.role)\nmethod \(request.method)\ntemperature-tolerance \(request.temperatureTolerance)\nminimum-samples \(request.minimumSamples)\nmemory-mib \(request.memoryMiB)\nthreads \(request.workers)\n"
        var corrections: [String: CalibrationAcquisition] = [:]
        for (role, selected) in [("bias", request.biasID), ("dark-flat", request.darkFlatID)] {
            guard let selected else { continue }
            guard let entry = available.first(where: { $0.id == selected }), let receipt = entry.receipt,
                  (role == "bias" ? receipt.role == "bias" && receipt.biasState == "included" : ["dark", "dark-flat"].contains(receipt.role) && ["included", "removed"].contains(receipt.biasState)) else {
                throw fail("Choose an active correction master with a confirmed bias state.")
            }
            let snapshot = try await verify(entry); corrections[role] = snapshot
            manifest += "\(role) \(try quote(snapshot.path)) \(snapshot.sha)" + (role == "dark-flat" ? " \(receipt.biasState)" : "") + "\n"
        }
        for path in canonicalInputs { manifest += "frame \(try quote(path)) -\n" }
        let id = UUID(), scratch = fm.temporaryDirectory.appendingPathComponent("PhotonStack-MasterBuild-" + id.uuidString)
        try fm.createDirectory(at: scratch, withIntermediateDirectories: false)
        var retained = false
        defer { if !retained { try? fm.removeItem(at: scratch) } }
        let file = scratch.appendingPathComponent("request.txt"), output = scratch.appendingPathComponent("result")
        try Data(manifest.utf8).write(to: file, options: .atomic)
        _ = try await service.runCLI(arguments: ["sensor-master-build", "--manifest", file.path, "--output-dir", output.path])
        let report = try await inspectConstructionFiles(output, verifySources: true)
        guard report.role == request.role, report.method == request.method, report.minimumSamples == request.minimumSamples,
              report.temperatureTolerance == request.temperatureTolerance,
              report.workingBytes <= UInt64(request.memoryMiB) * 1024 * 1024, report.workers <= request.workers,
              Set(report.sources.map(\.path)) == Set(canonicalInputs),
              report.corrections.bias?.sha == corrections["bias"]?.sha,
              report.corrections.bias?.path == corrections["bias"]?.path,
              report.corrections.darkFlat?.sha == corrections["dark-flat"]?.sha,
              report.corrections.darkFlat?.path == corrections["dark-flat"]?.path else {
            throw fail("Constructed master does not match the requested inputs and parameters.")
        }
        try Task.checkCancellation()
        let draft = CalibrationMasterDraft(id: id, directory: output, report: report, request: request)
        drafts[id] = draft; retained = true
        return draft
    }
    func saveDraft(_ id: UUID, label: String, notes: String) async throws -> String {
        guard let draft = drafts[id] else { throw fail("Create and review a master before saving.") }
        let entryID = try await importConstruction(draft.directory, label: label, notes: notes, expected: draft.report)
        discardDraft(id)
        return entryID
    }
    func discardDraft(_ id: UUID) {
        guard let draft = drafts.removeValue(forKey: id) else { return }
        try? fm.removeItem(at: draft.directory.deletingLastPathComponent())
    }
}
