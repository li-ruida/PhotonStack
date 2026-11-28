import CryptoKit
import Darwin
import Foundation
import PhotonStackProcessing

/// Immutable master snapshots with declared preparation state. Header matching
/// never upgrades these declarations into evidence of calibrated pixel quality.
actor CalibrationLibrary {
    static let roles = ["bias", "dark", "dark-flat", "flat"]
    var drafts: [UUID: CalibrationMasterDraft] = [:]
    struct Receipt: Codable, Equatable, Sendable {
        var schema = 1
        var id, label, role, biasState, notes: String
        var importedAt: Date
        var acquisition: CalibrationAcquisition
        var construction: CalibrationMasterConstruction? = nil
        var reviewHistory: [CalibrationMasterRevision]? = nil
        var rejected: Bool { reviewHistory?.last?.quality == "reject" }
        var isValid: Bool {
            schema == 1 && id == CalibrationLibrary.identity(masterSHA: acquisition.sha, construction: construction) &&
            (construction?.isValid ?? true) && acquisition.isValid && acquisition.channels == 1 &&
            CalibrationLibrary.roles.contains(role) && ["unknown", "included", "removed"].contains(biasState) &&
            (role != "bias" || biasState == "included") && CalibrationLibrary.validLabel(label) &&
            notes.utf8.count <= 4096 && importedAt.timeIntervalSince1970.isFinite &&
            (reviewHistory.map { !$0.isEmpty && $0.count <= 64 && $0.allSatisfy(\.isValid) &&
                $0.last?.label == label && $0.last?.notes == notes && $0.last?.biasState == biasState } ?? true)
        }
    }
    struct Entry: Identifiable, Sendable {
        var id: String; var archived: Bool; var receipt: Receipt?; var issue: String?
        var label: String { receipt?.label ?? String(id.prefix(12)) }
    }
    let root: URL
    let service: any ProcessingService
    let fm = FileManager.default
    init(root: URL? = nil, service: any ProcessingService = CLIProcessingService.makeDefault()) {
        self.root = (root ?? ProcessInfo.processInfo.environment["PHOTONSTACK_CALIBRATION_LIBRARY"].map { URL(fileURLWithPath: $0) } ??
            FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("PhotonStack/CalibrationMasters")).resolvingSymlinksInPath()
        self.service = service
    }
    static func identity(masterSHA: String, construction: CalibrationMasterConstruction?) -> String {
        guard let construction else { return masterSHA }
        let text = "PhotonStack-Calibration-Master-1\n" + masterSHA + "\n" + construction.files.sorted { $0.key < $1.key }.map { $0.key + " " + $0.value }.joined(separator: "\n")
        return SHA256.hash(data: Data(text.utf8)).map { String(format: "%02x", $0) }.joined()
    }
    static func validLabel(_ value: String) -> Bool {
        !value.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && value.utf8.count <= 256 &&
        !value.unicodeScalars.contains { $0.value < 32 || $0.value == 127 }
    }
    func fail(_ message: String) -> NSError { CalibrationAcquisition.failure(message) }
    func directory(_ url: URL) throws {
        let v = try url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
        guard v.isDirectory == true, v.isSymbolicLink != true else { throw fail("Library directory is missing or linked.") }
    }
    func regular(_ url: URL) throws {
        let v = try url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
        guard v.isRegularFile == true, v.isSymbolicLink != true else { throw fail("Library file is missing or linked.") }
    }
    func base(_ archived: Bool) -> URL { archived ? root.appendingPathComponent("Archive") : root }
    func location(_ entry: Entry) throws -> URL {
        guard SpectralCalibrationReport.digest(entry.id) else { throw fail("Invalid master identity.") }
        try directory(root); try directory(base(entry.archived))
        let dir = base(entry.archived).appendingPathComponent(entry.id); try directory(dir)
        return dir
    }
    func receipt(_ dir: URL, id: String) throws -> Receipt {
        let url = dir.appendingPathComponent("entry.json"); try regular(url)
        guard (try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? Int.max) <= 131072 else { throw fail("Master record is too large.") }
        let data = try Data(contentsOf: url)
        guard data.count <= 131072 else { throw fail("Master record grew during read.") }
        let record = try JSONDecoder().decode(Receipt.self, from: data)
        guard record.isValid, record.id == id else { throw fail("Invalid master record.") }
        try regular(dir.appendingPathComponent("master.fits"))
        if record.construction != nil { for name in CalibrationMasterConstruction.names { try regular(dir.appendingPathComponent(name)) } }
        return record
    }
    func publish(_ stage: URL, to destination: URL) throws {
        guard renamex_np(stage.path, destination.path, UInt32(RENAME_EXCL)) == 0 else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
        }
    }
    func scanLights(_ urls: [URL]) throws -> [URL] {
        var result = Set<URL>()
        func add(_ url: URL) throws {
            let v = try url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
            if v.isRegularFile == true, v.isSymbolicLink != true, ["fit", "fits", "fts"].contains(url.pathExtension.lowercased()) {
                result.insert(url.resolvingSymlinksInPath())
                if result.count > 4096 { throw fail("Select at most 4096 FITS light frames per review.") }
            }
        }
        for url in urls {
            try Task.checkCancellation()
            let v = try url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            if v.isDirectory == true, v.isSymbolicLink != true {
                var scanError: Error?
                guard let iterator = fm.enumerator(at: url, includingPropertiesForKeys: [.isRegularFileKey, .isSymbolicLinkKey],
                    options: [.skipsHiddenFiles], errorHandler: { _, error in scanError = error; return false }) else { throw fail("Cannot read the selected folder.") }
                for case let child as URL in iterator {
                    try Task.checkCancellation()
                    if try child.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink == true { iterator.skipDescendants(); continue }
                    try add(child)
                }
                if let scanError { throw scanError }
            } else { try add(url) }
        }
        guard !result.isEmpty else { throw fail("No FITS images were found in the selection.") }
        return result.sorted { $0.path < $1.path }
    }
    func list(archived: Bool = false) throws -> [Entry] {
        guard fm.fileExists(atPath: base(archived).path) else { return [] }
        try directory(root); try directory(base(archived))
        return try fm.contentsOfDirectory(at: base(archived), includingPropertiesForKeys: nil).filter {
            SpectralCalibrationReport.digest($0.lastPathComponent)
        }.map { url in
            let id = url.lastPathComponent
            do { try directory(url); return Entry(id: id, archived: archived, receipt: try receipt(url, id: id)) }
            catch { return Entry(id: id, archived: archived, issue: error.localizedDescription) }
        }.sorted { $0.label == $1.label ? $0.id < $1.id : $0.label.localizedStandardCompare($1.label) == .orderedAscending }
    }
    func verify(_ entry: Entry) async throws -> CalibrationAcquisition {
        let dir = try location(entry), record = try receipt(dir, id: entry.id), url = dir.appendingPathComponent("master.fits")
        let actual = try await CalibrationAcquisition.inspect([url], service: service)[0]
        guard actual == (try record.acquisition.at(url)), record == entry.receipt else { throw fail("Master or its record changed; reload the library.") }
        if let construction = record.construction {
            let report = try await inspectConstructionFiles(dir, expected: construction)
            guard report.matches(actual), report.role == record.role else { throw fail("Construction record does not match the master.") }
        }
        return actual
    }
    func importMaster(_ url: URL, label: String, role: String, biasState: String, notes: String) async throws -> String {
        let original = try await CalibrationAcquisition.inspect([url], service: service)[0]
        if let declared = original.text["biasState"], declared != biasState {
            throw fail("The selected bias state contradicts PSBIAS in the FITS header.")
        }
        let record = Receipt(id: original.sha, label: label.trimmingCharacters(in: .whitespacesAndNewlines),
                             role: role, biasState: biasState, notes: notes, importedAt: Date(), acquisition: original)
        guard record.isValid else { throw fail("Choose a single-plane FITS master, a valid name, role and bias state.") }
        // These common header labels can reject an obvious mistaken import;
        // absence or an unknown label remains an explicit user declaration.
        var kind = original.text["frameType", default: ""].uppercased().filter { $0.isLetter || $0.isNumber }
        if kind.hasPrefix("MASTER") { kind.removeFirst(6) }; if kind.hasSuffix("FRAME") { kind.removeLast(5) }
        let known = ["LIGHT":"light", "DARK":"dark", "BIAS":"bias", "OFFSET":"bias", "DARKFLAT":"dark-flat", "FLATDARK":"dark-flat", "FLAT":"flat"]
        if let headerRole = known[kind], headerRole != role { throw fail("The selected role contradicts IMAGETYP in the FITS header.") }
        try fm.createDirectory(at: root, withIntermediateDirectories: true); try directory(root)
        let stage = root.appendingPathComponent(".import-" + UUID().uuidString)
        try fm.createDirectory(at: stage, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: stage) }
        let copy = stage.appendingPathComponent("master.fits")
        try fm.copyItem(at: url.resolvingSymlinksInPath(), to: copy)
        let verified = try await CalibrationAcquisition.inspect([copy], service: service)[0]
        guard verified == (try original.at(copy)) else { throw fail("Master changed while copying into the library.") }
        try JSONEncoder().encode(record).write(to: stage.appendingPathComponent("entry.json"), options: .atomic)
        try Task.checkCancellation()
        let target = root.appendingPathComponent(record.id)
        do { try publish(stage, to: target) }
        catch {
            guard fm.fileExists(atPath: target.path) else { throw error }
            try directory(target)
            let existing = try receipt(target, id: record.id)
            guard existing.role == role, existing.biasState == biasState else {
                throw fail("This master already has a different role or bias declaration in the library.")
            }
            _ = try await verify(Entry(id: existing.id, archived: false, receipt: existing))
        }
        return record.id
    }
    func archive(_ entry: Entry) throws {
        let from = try location(entry), target = base(!entry.archived)
        try fm.createDirectory(at: target, withIntermediateDirectories: true); try directory(target)
        try publish(from, to: target.appendingPathComponent(entry.id))
    }
    func export(_ entry: Entry, to destination: URL) async throws {
        _ = try await verify(entry)
        let from = try location(entry), stage = destination.deletingLastPathComponent().appendingPathComponent(".calibration-export-" + UUID().uuidString)
        try fm.createDirectory(at: stage, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: stage) }
        for name in ["master.fits", "entry.json"] + (entry.receipt?.construction == nil ? [] : Array(CalibrationMasterConstruction.names).sorted()) { try fm.copyItem(at: from.appendingPathComponent(name), to: stage.appendingPathComponent(name)) }
        let r = try receipt(stage, id: entry.id), copy = stage.appendingPathComponent("master.fits")
        let actual = try await CalibrationAcquisition.inspect([copy], service: service)[0]
        guard r == entry.receipt, actual == (try r.acquisition.at(copy)) else { throw fail("Master changed during export.") }
        if let construction = r.construction {
            let report = try await inspectConstructionFiles(stage, expected: construction)
            guard report.matches(actual), report.role == r.role else { throw fail("Construction changed during export.") }
        }
        try Task.checkCancellation(); try publish(stage, to: destination)
    }
    func plan(lights: [URL], entries: [Entry], tolerance: Double) async throws -> CalibrationPlanReport {
        guard !lights.isEmpty, lights.count <= 4096, entries.count <= 256, lights.count * entries.count <= 262144,
              tolerance.isFinite, (0...10).contains(tolerance), Set(lights.map { $0.resolvingSymlinksInPath() }).count == lights.count,
              Set(entries.map(\.id)).count == entries.count, entries.allSatisfy({ !$0.archived && $0.receipt?.rejected == false && CalibrationPlanReport.roles.contains($0.receipt?.role ?? "") }),
              Set(entries.compactMap { $0.receipt?.acquisition.sha }).count == entries.count else {
            throw fail("Choose distinct light frames and active masters, and a temperature tolerance within 0–10°C.")
        }
        func quote(_ value: String) throws -> String {
            guard !value.contains("\n"), !value.contains("\r"), !value.contains("\0") else { throw fail("Unsupported filename in calibration plan.") }
            return "\"" + value.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
        }
        var manifest = "PhotonStack-CalibrationPlan-1\ntemperature-tolerance \(tolerance)\n"
        for url in lights { manifest += "light \(try quote(url.path)) -\n" }
        var expectedMasters: [String: Receipt] = [:]
        for entry in entries {
            let dir = try location(entry), r = try receipt(dir, id: entry.id)
            guard r == entry.receipt else { throw fail("Master record changed; reload before matching.") }
            if let construction = r.construction {
                let report = try await inspectConstructionFiles(dir, expected: construction)
                guard report.role == r.role, report.matches(r.acquisition) else { throw fail("Master construction record changed.") }
            }
            let url = dir.appendingPathComponent("master.fits")
            expectedMasters[try CalibrationAcquisition.canonicalPath(url)] = r
            manifest += "\(r.role) \(try quote(url.path)) \(r.acquisition.sha)\n"
        }
        let scratch = fm.temporaryDirectory.appendingPathComponent("PhotonStack-Calibration-" + UUID().uuidString)
        try fm.createDirectory(at: scratch, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: scratch) }
        let file = scratch.appendingPathComponent("plan.txt"), data = Data(manifest.utf8)
        try data.write(to: file, options: .atomic)
        let response = try await service.runCLI(arguments: ["calibration-plan", "--manifest", file.path])
        let report = try JSONDecoder().decode(CalibrationPlanReport.self, from: Data(response.standardOutput.utf8))
        let sha = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        guard report.isValid, report.manifestSHA == sha, report.temperatureTolerance == tolerance,
              Set(report.lights.map(\.path)) == Set(try lights.map { try CalibrationAcquisition.canonicalPath($0) }),
              Set(report.masters.map { $0.acquisition.path }) == Set(expectedMasters.keys) else { throw fail("Calibration plan does not match the requested sources.") }
        for master in report.masters {
            let expected = expectedMasters[master.acquisition.path]!
            guard master.role == expected.role, master.acquisition == (try expected.acquisition.at(URL(fileURLWithPath: master.acquisition.path))) else {
                throw fail("Master metadata changed since import.")
            }
        }
        try Task.checkCancellation()
        return report
    }
}
