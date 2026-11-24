import Foundation

struct CalibrationJobReport: Codable, Sendable {
    struct BadPixels: Codable, Sendable { var path, sha: String }
    struct Master: Codable, Sendable { var role, path, sha, biasState: String }
    struct Frame: Codable, Sendable {
        var index, group: Int
        var source, sourceSHA, directory: String
        var complete: Bool
        var masters: [String: String?]
        var products: [String: String]
    }
    var schema, total, completed: Int
    var command, domain, jobSHA, status, dateGrouping: String
    var sourcesVerified, productsVerified: Bool
    var masters: [String: Master]
    var frames: [Frame]
    var badPixels: BadPixels? = nil
    var isValid: Bool {
        if let map = badPixels, !Self.path(map.path) || !SpectralCalibrationReport.digest(map.sha) { return false }
        let roles = Set(CalibrationPlanReport.roles)
        let products: Set<String> = ["calibrated.fits", "valid.fits", "report.json"]
        guard schema == 1, command == "calibration-job", domain == "sensor-linear", dateGrouping == "UTC-calendar-date",
              SpectralCalibrationReport.digest(jobSHA), productsVerified, (1...4096).contains(total), (0...total).contains(completed),
              frames.count == total, completed == frames.filter(\.complete).count,
              status == (completed == total ? "complete" : "partial"), (1...256).contains(masters.count) else { return false }
        var sources = Set<String>(), hashes = Set<String>(), used = Set<String>()
        for (id, master) in masters {
            guard !id.isEmpty, id.count <= 64, id != "-", id.utf8.allSatisfy({ (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || $0 == 45 || $0 == 95 }),
                  roles.contains(master.role), Self.path(master.path), SpectralCalibrationReport.digest(master.sha),
                  sources.insert(master.path).inserted, hashes.insert(master.sha).inserted,
                  ["included", "removed"].contains(master.biasState), master.role != "bias" || master.biasState == "included" else { return false }
        }
        for (i, frame) in frames.enumerated() {
            guard frame.index == i, (0..<total).contains(frame.group), Self.path(frame.source), SpectralCalibrationReport.digest(frame.sourceSHA),
                  sources.insert(frame.source).inserted, hashes.insert(frame.sourceSHA).inserted,
                  frame.directory == String(format: "frame-%06d", i + 1), Set(frame.masters.keys) == roles,
                  Set(frame.products.keys) == (frame.complete ? products : []), frame.products.values.allSatisfy(SpectralCalibrationReport.digest) else { return false }
            for (role, id) in frame.masters {
                if let id { guard masters[id]?.role == role else { return false }; used.insert(id) }
            }
            let bias = frame.masters["bias"] ?? nil, dark = frame.masters["dark"] ?? nil, flat = frame.masters["flat"] ?? nil
            guard bias != nil || dark != nil else { return false }
            if bias == nil {
                if let dark, masters[dark]?.biasState == "removed" { return false }
                if let flat, masters[flat]?.biasState == "included" { return false }
            }
        }
        return used == Set(masters.keys)
    }
    private static func path(_ text: String) -> Bool { text.hasPrefix("/") && !text.contains("\0") && !text.contains("\n") && !text.contains("\r") }
    static func decode(_ text: String) throws -> Self {
        guard text.utf8.count <= 16 * 1024 * 1024 else { throw CalibrationAcquisition.failure("Calibration task report is too large.") }
        let report = try JSONDecoder().decode(Self.self, from: Data(text.utf8))
        guard report.isValid else { throw CalibrationAcquisition.failure("Invalid calibration task report.") }
        return report
    }
    func matches(_ draft: CalibrationJobDraft) -> Bool {
        guard badPixels?.path == draft.badPixels?.path, badPixels?.sha == draft.badPixels?.sha else { return false }
        guard total == draft.review.lights.count, Set(masters.keys) == Set(draft.usedMasters.map { "m\($0)" }) else { return false }
        for i in draft.usedMasters {
            let expected = draft.review.masters[i]
            guard let actual = masters["m\(i)"], actual.role == expected.role, actual.path == expected.acquisition.path,
                  actual.sha == expected.acquisition.sha, actual.biasState == draft.entries[i].receipt?.biasState else { return false }
        }
        return frames.enumerated().allSatisfy { i, frame in
            let source = draft.review.lights[i]
            return frame.source == source.path && frame.sourceSHA == source.sha && (0..<3).allSatisfy { r in
                (frame.masters[CalibrationPlanReport.roles[r]] ?? nil) == (draft.choices[i][r] < 0 ? nil : "m\(draft.choices[i][r])")
            }
        }
    }
}

extension CalibrationLibrary {
    func runCalibration(_ draft: CalibrationJobDraft, to directory: URL, maximumFrames: Int? = nil) async throws -> CalibrationJobReport {
        let manifest = try draft.manifest()
        for i in draft.usedMasters {
            let entry = draft.entries[i]
            guard !entry.archived, try await verify(entry) == draft.review.masters[i].acquisition else {
                throw fail("Master changed after review; review and assign it again.")
            }
        }
        let scratch = fm.temporaryDirectory.appendingPathComponent("PhotonStack-CalibrationJob-" + UUID().uuidString)
        try fm.createDirectory(at: scratch, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: scratch) }
        let file = scratch.appendingPathComponent("manifest.txt")
        try manifest.write(to: file, atomically: true, encoding: .utf8)
        var args = ["calibration-run", "--manifest", file.path, "--output-dir", directory.path]
        if let maximumFrames { args += ["--max-frames", String(maximumFrames)] }
        let value = try await service.runCLI(arguments: args)
        let report = try CalibrationJobReport.decode(value.standardOutput)
        guard report.sourcesVerified, report.matches(draft) else { throw fail("Task result does not match the selected inputs and masters.") }
        return report
    }
    func inspectCalibration(_ directory: URL) async throws -> CalibrationJobReport {
        let result = try await service.runCLI(arguments: ["calibration-job-inspect", "--job", directory.path])
        let report = try CalibrationJobReport.decode(result.standardOutput)
        guard !report.sourcesVerified else { throw fail("Offline task inspection cannot verify source files.") }
        return report
    }
    func resumeCalibration(_ directory: URL, expected: CalibrationJobReport) async throws -> CalibrationJobReport {
        let current = try await inspectCalibration(directory)
        guard current.jobSHA == expected.jobSHA else { throw fail("The selected task changed; reopen it before resuming.") }
        let result = try await service.runCLI(arguments: ["calibration-resume", "--job", directory.path])
        let report = try CalibrationJobReport.decode(result.standardOutput)
        guard report.jobSHA == expected.jobSHA, report.sourcesVerified, report.completed >= expected.completed else { throw fail("Unexpected resumed task result.") }
        return report
    }
}
