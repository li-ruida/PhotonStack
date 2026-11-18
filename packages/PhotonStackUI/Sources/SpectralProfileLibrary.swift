import CryptoKit
import Darwin
import Foundation
import PhotonStackProcessing

/// Content-addressed user data. No camera curves or third-party databases are
/// bundled. Every use revalidates the original profile with the scientific CLI.
actor SpectralProfileLibrary {
    struct Entry: Identifiable, Sendable {
        var id, label: String
        var profile: SpectralProfileMetadata?
        var archived: Bool
        var issue: String?
    }
    struct Receipt: Codable {
        var schema = 1
        var id, label: String
        var profile: SpectralProfileMetadata
        var licenseSHA: String?
    }
    struct Selection: Sendable {
        var url: URL
        var inspection: SpectralProfileInspection
        var licenseIncluded: Bool
    }
    let root: URL
    private let service: any ProcessingService
    private let fm = FileManager.default
    init(root: URL? = nil, service: any ProcessingService = CLIProcessingService.makeDefault()) {
        self.root = (root ?? ProcessInfo.processInfo.environment["PHOTONSTACK_SPECTRAL_LIBRARY"].map { URL(fileURLWithPath: $0) } ??
            FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("PhotonStack/SpectralProfiles")).resolvingSymlinksInPath()
        self.service = service
    }
    private func digest(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
    private func key(profile: String, license: String?) -> String { digest(Data((profile + "|" + (license ?? "")).utf8)) }
    private func file(_ url: URL, limit: Int) throws -> Data {
        let v = try url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
        guard v.isRegularFile == true, v.isSymbolicLink != true, let size = v.fileSize, size <= limit else {
            throw failure("Invalid, linked or oversized library file: \(url.lastPathComponent)")
        }
        let data = try Data(contentsOf: url)
        guard data.count <= limit else { throw failure("Library file grew during read") }
        return data
    }
    private func failure(_ message: String) -> NSError { NSError(domain: "PhotonStack.SpectralLibrary", code: 1, userInfo: [NSLocalizedDescriptionKey: message]) }
    private func base(_ archived: Bool) -> URL { archived ? root.appendingPathComponent("Archive") : root }
    private func directory(_ id: String, archived: Bool) throws -> URL {
        guard SpectralCalibrationReport.digest(id) else { throw failure("Invalid profile identity") }
        let parent = base(archived)
        if archived { try requireDirectory(parent) }
        let dir = parent.appendingPathComponent(id)
        try requireDirectory(dir)
        return dir
    }
    private func requireDirectory(_ url: URL) throws {
        let v = try url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
        guard v.isDirectory == true, v.isSymbolicLink != true else { throw failure("Library directory is missing or linked") }
    }
    private func receipt(_ dir: URL, id: String) throws -> Receipt {
        let r = try JSONDecoder().decode(Receipt.self, from: file(dir.appendingPathComponent("entry.json"), limit: 65536))
        guard r.schema == 1, r.id == id, r.profile.isValid, validLabel(r.label),
              r.licenseSHA == nil || SpectralCalibrationReport.digest(r.licenseSHA!),
              key(profile: r.profile.sha, license: r.licenseSHA) == id else { throw failure("Invalid profile receipt") }
        return r
    }
    private func validLabel(_ name: String) -> Bool {
        !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && name.utf8.count <= 4096 &&
        !name.unicodeScalars.contains { $0.value < 32 || $0.value == 127 }
    }
    func list(archived: Bool = false) throws -> [Entry] {
        guard fm.fileExists(atPath: base(archived).path) else { return [] }
        try requireDirectory(base(archived))
        return try fm.contentsOfDirectory(at: base(archived), includingPropertiesForKeys: nil).filter {
            SpectralCalibrationReport.digest($0.lastPathComponent)
        }.map { dir in
            let id = dir.lastPathComponent
            do {
                try requireDirectory(dir)
                let r = try receipt(dir, id: id)
                return Entry(id: id, label: r.label, profile: r.profile, archived: archived)
            } catch { return Entry(id: id, label: String(id.prefix(12)), archived: archived, issue: error.localizedDescription) }
        }.sorted { $0.label == $1.label ? $0.id < $1.id : $0.label.localizedStandardCompare($1.label) == .orderedAscending }
    }
    /// Owns only the newly created staging directory. RENAME_EXCL protects a
    /// destination created by another window or process during validation.
    private func publish(_ stage: URL, to destination: URL) throws {
        guard renamex_np(stage.path, destination.path, UInt32(RENAME_EXCL)) == 0 else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
        }
    }
    private func validateFiles(_ dir: URL, receipt r: Receipt) throws {
        guard digest(try file(dir.appendingPathComponent("profile.txt"), limit: 16 * 1024 * 1024)) == r.profile.sha else {
            throw failure("Response profile changed; import it as a new configuration")
        }
        if let expected = r.licenseSHA {
            guard digest(try file(dir.appendingPathComponent("response-source-license.txt"), limit: 1024 * 1024)) == expected else {
                throw failure("Response license changed")
            }
        }
    }
    func select(_ entry: Entry) async throws -> Selection {
        let dir = try directory(entry.id, archived: entry.archived), r = try receipt(dir, id: entry.id)
        try validateFiles(dir, receipt: r)
        let url = dir.appendingPathComponent("profile.txt")
        let inspection = try await SpectralProfileInspection.read(url, expectedSHA: r.profile.sha, service: service)
        guard inspection.profile == r.profile else { throw failure("Profile metadata differs from the stored receipt") }
        // Recheck after the child process; no cached metadata authorizes use.
        try requireDirectory(dir); try validateFiles(dir, receipt: r)
        return Selection(url: url, inspection: inspection, licenseIncluded: r.licenseSHA != nil)
    }
    func importProfile(_ source: URL) async throws -> String {
        let profile = try file(source, limit: 16 * 1024 * 1024)
        let licenseURL = source.deletingLastPathComponent().appendingPathComponent("response-source-license.txt")
        let license = fm.fileExists(atPath: licenseURL.path) ? try file(licenseURL, limit: 1024 * 1024) : nil
        if let license, String(data: license, encoding: .utf8) == nil { throw failure("Response license must be UTF-8 text") }
        try fm.createDirectory(at: root, withIntermediateDirectories: true); try requireDirectory(root)
        let stage = root.appendingPathComponent(".import-" + UUID().uuidString)
        try fm.createDirectory(at: stage, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: stage) }
        try profile.write(to: stage.appendingPathComponent("profile.txt"), options: .withoutOverwriting)
        if let license { try license.write(to: stage.appendingPathComponent("response-source-license.txt"), options: .withoutOverwriting) }
        let info = try await SpectralProfileInspection.read(stage.appendingPathComponent("profile.txt"), expectedSHA: digest(profile), service: service)
        guard validLabel(info.profile.name) else { throw failure("Response profile needs a nonempty display name") }
        let licenseSHA = license.map(digest), id = key(profile: info.profile.sha, license: licenseSHA)
        let r = Receipt(id: id, label: info.profile.name, profile: info.profile, licenseSHA: licenseSHA)
        try JSONEncoder().encode(r).write(to: stage.appendingPathComponent("entry.json"), options: .withoutOverwriting)
        try validateFiles(stage, receipt: r); try Task.checkCancellation()
        let destination = root.appendingPathComponent(id)
        do { try publish(stage, to: destination) }
        catch {
            // Idempotent import only when the independently published record is
            // complete and all scientific/provenance bytes still agree.
            let existing = try directory(id, archived: false), previous = try receipt(existing, id: id)
            try validateFiles(existing, receipt: previous)
            guard previous.profile == r.profile, previous.licenseSHA == r.licenseSHA else { throw error }
        }
        return id
    }
    func rename(_ entry: Entry, label: String) throws {
        guard validLabel(label) else { throw failure("Enter a nonempty profile name") }
        let dir = try directory(entry.id, archived: entry.archived)
        var r = try receipt(dir, id: entry.id); r.label = label.trimmingCharacters(in: .whitespaces)
        try JSONEncoder().encode(r).write(to: dir.appendingPathComponent("entry.json"), options: .atomic)
    }
    func archive(_ entry: Entry) throws {
        let from = try directory(entry.id, archived: entry.archived), target = base(!entry.archived)
        try fm.createDirectory(at: target, withIntermediateDirectories: true); try requireDirectory(target)
        try publish(from, to: target.appendingPathComponent(entry.id))
    }
    func export(_ entry: Entry, to destination: URL) async throws {
        _ = try await select(entry)
        let dir = try directory(entry.id, archived: entry.archived), r = try receipt(dir, id: entry.id)
        let parent = destination.deletingLastPathComponent()
        let stage = parent.appendingPathComponent(".spectral-export-" + UUID().uuidString)
        try fm.createDirectory(at: stage, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: stage) }
        for name in ["profile.txt", "entry.json"] + (r.licenseSHA == nil ? [] : ["response-source-license.txt"]) {
            try fm.copyItem(at: dir.appendingPathComponent(name), to: stage.appendingPathComponent(name))
        }
        let copied = try receipt(stage, id: entry.id)
        guard copied.profile == r.profile, copied.licenseSHA == r.licenseSHA else { throw failure("Profile changed during export") }
        try validateFiles(stage, receipt: copied); try Task.checkCancellation()
        try publish(stage, to: destination)
    }
}
