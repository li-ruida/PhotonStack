import Foundation
import Darwin
import PhotonStackProcessing

struct CalibrationAcquisition: Codable, Equatable, Sendable {
    struct Issue: Codable, Equatable, Sendable { var field, code, message: String }
    var path, sha: String
    var bytes: UInt64
    var width, height, channels, bitpix: Int
    var text: [String: String]
    var numeric: [String: Double]
    var sourceKeys: [String: [String]]
    var issues: [Issue]
    var isValid: Bool {
        path.hasPrefix("/") && !path.contains("\0") && SpectralCalibrationReport.digest(sha) && bytes > 0 &&
        width > 0 && height > 0 && width <= UInt32.max && height <= UInt32.max && [1,3,4].contains(channels) && [8,16,32,-32,-64].contains(bitpix) &&
        numeric.values.allSatisfy(\.isFinite) && text.count <= 64 && numeric.count <= 64 && issues.count <= 128
    }
    static func canonicalPath(_ url: URL) throws -> String {
        guard let path = realpath(url.path, nil) else { throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno)) }
        defer { free(path) }
        return String(cString: path)
    }
    func at(_ url: URL) throws -> Self { var copy = self; copy.path = try Self.canonicalPath(url); return copy }
    static func inspect(_ urls: [URL], service: any ProcessingService) async throws -> [Self] {
        struct Report: Decodable {
            struct Entry: Decodable { var status: String; var acquisition: CalibrationAcquisition }
            var type, command, scope: String; var schema: Int; var entries: [Entry]
        }
        guard (1...256).contains(urls.count) else { throw failure("Inspect 1–256 FITS images at a time.") }
        let response = try await service.runCLI(arguments: ["calibration-inspect"] + urls.map(\.path))
        let report = try JSONDecoder().decode(Report.self, from: Data(response.standardOutput.utf8))
        guard report.type == "complete", report.command == "calibration-inspect", report.scope == "acquisition-only",
              report.schema == 1, report.entries.count == urls.count else { throw failure("Invalid acquisition report.") }
        for (entry, url) in zip(report.entries, urls) {
            guard entry.status == "inspected", entry.acquisition.isValid,
                  entry.acquisition.path == (try canonicalPath(url)) else {
                throw failure("Acquisition report does not match the requested file.")
            }
        }
        try Task.checkCancellation()
        return report.entries.map(\.acquisition)
    }
    static func failure(_ message: String) -> NSError {
        NSError(domain: "PhotonStack.Calibration", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
    }
}

struct CalibrationPlanReport: Codable, Sendable {
    struct Master: Codable, Sendable { var role: String; var acquisition: CalibrationAcquisition }
    struct Evidence: Codable, Sendable { var field, status, input, master, message: String }
    struct Candidate: Codable, Sendable {
        struct Issue: Codable, Sendable { var count, firstMember: Int; var example: Evidence }
        var master: Int; var status: String
        var compatibleCount, reviewCount, incompatibleCount: Int
        var issues: [Issue]
    }
    struct Choice: Codable, Sendable {
        var status: String; var suggestedMaster: Int?
        var compatibleMasters, reviewMasters: [Int]
        enum CodingKeys: String, CodingKey { case status, suggestedMaster, compatibleMasters, reviewMasters }
        func encode(to encoder: any Encoder) throws {
            var c = encoder.container(keyedBy: CodingKeys.self)
            try c.encode(status, forKey: .status)
            // Preserve the CLI schema's explicit null, including in exported reviews.
            try c.encode(suggestedMaster, forKey: .suggestedMaster)
            try c.encode(compatibleMasters, forKey: .compatibleMasters)
            try c.encode(reviewMasters, forKey: .reviewMasters)
        }
    }
    struct Group: Codable, Sendable {
        var members: [Int]; var text: [String: String]; var numeric: [String: Double]
        var issues: [String]; var candidates: [Candidate]; var roles: [String: Choice]
    }
    struct Frame: Codable, Sendable { var light, group: Int; var roles: [String: Choice] }
    var type, command, scope, dateGrouping, manifestSHA: String
    var schema: Int; var canApply, pixelsValidated: Bool; var temperatureTolerance: Double
    var lights: [CalibrationAcquisition]; var masters: [Master]; var groups: [Group]; var frameChoices: [Frame]
    static let roles = ["bias", "dark", "flat"]
    var isValid: Bool {
        guard type == "complete", command == "calibration-plan", scope == "acquisition-only", schema == 1,
              !canApply, !pixelsValidated, dateGrouping == "UTC-calendar-date", SpectralCalibrationReport.digest(manifestSHA),
              temperatureTolerance.isFinite, (0...10).contains(temperatureTolerance),
              (1...4096).contains(lights.count), masters.count <= 256, lights.count * masters.count <= 262144,
              lights.allSatisfy(\.isValid), masters.allSatisfy({ Self.roles.contains($0.role) && $0.acquisition.isValid }),
              frameChoices.count == lights.count else { return false }
        let all = lights + masters.map(\.acquisition)
        guard Set(all.map(\.sha)).count == all.count, Set(all.map(\.path)).count == all.count else { return false }
        var membership = Array(repeating: -1, count: lights.count)
        for (g, group) in groups.enumerated() {
            guard !group.members.isEmpty, group.numeric.values.allSatisfy(\.isFinite), validChoices(group.roles),
                  group.candidates.count == masters.count, Set(group.candidates.map(\.master)).count == masters.count else { return false }
            for member in group.members {
                guard lights.indices.contains(member), membership[member] == -1 else { return false }
                membership[member] = g
            }
            for c in group.candidates {
                guard masters.indices.contains(c.master), (0...group.members.count).contains(c.compatibleCount),
                      (0...group.members.count).contains(c.reviewCount), (0...group.members.count).contains(c.incompatibleCount),
                      c.compatibleCount + c.reviewCount + c.incompatibleCount == group.members.count,
                      c.status == (c.incompatibleCount > 0 ? "incompatible" : c.reviewCount > 0 ? "review" : "compatible"),
                      c.issues.allSatisfy({ $0.count > 0 && $0.count <= group.members.count && group.members.contains($0.firstMember) &&
                          ["missing", "invalid", "conflict"].contains($0.example.status) }) else { return false }
                let choice = group.roles[masters[c.master].role]!
                guard choice.compatibleMasters.contains(c.master) == (c.status == "compatible"),
                      choice.reviewMasters.contains(c.master) == (c.status == "review") else { return false }
            }
        }
        guard !membership.contains(-1) else { return false }
        for (i, frame) in frameChoices.enumerated() {
            guard frame.light == i, frame.group == membership[i], validChoices(frame.roles) else { return false }
        }
        // Cross-check aggregates against every frame's decision, not just counts or the first frame.
        for group in groups {
            for c in group.candidates {
                let role = masters[c.master].role
                let compatible = group.members.filter { frameChoices[$0].roles[role]!.compatibleMasters.contains(c.master) }.count
                let review = group.members.filter { frameChoices[$0].roles[role]!.reviewMasters.contains(c.master) }.count
                guard compatible == c.compatibleCount, review == c.reviewCount else { return false }
            }
        }
        return true
    }
    private func validChoices(_ choices: [String: Choice]) -> Bool {
        guard Set(choices.keys) == Set(Self.roles) else { return false }
        for (role, c) in choices {
            let indices = c.compatibleMasters + c.reviewMasters
            guard Set(indices).count == indices.count,
                  indices.allSatisfy({ masters.indices.contains($0) && masters[$0].role == role }) else { return false }
            let expected = c.compatibleMasters.count > 1 ? "ambiguous" : c.compatibleMasters.count == 1 ? "unique-compatible" :
                !c.reviewMasters.isEmpty ? "review-required" : masters.contains(where: { $0.role == role }) ? "no-compatible-master" : "missing-master"
            guard c.status == expected, c.suggestedMaster == (c.compatibleMasters.count == 1 ? c.compatibleMasters[0] : nil) else { return false }
        }
        return true
    }
}
