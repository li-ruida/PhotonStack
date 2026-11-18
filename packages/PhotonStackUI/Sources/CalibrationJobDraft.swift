import Foundation

/// An explicit assignment, separate from acquisition-only candidate suggestions.
struct CalibrationJobDraft: Sendable {
    static let unresolved = -2, omitted = -1
    let review: CalibrationPlanReport
    let entries: [CalibrationLibrary.Entry]
    var choices: [[Int]]
    var memoryMiB = 512, workers = 4

    init(review: CalibrationPlanReport, entries: [CalibrationLibrary.Entry]) throws {
        guard review.isValid else { throw CalibrationAcquisition.failure("Invalid calibration review.") }
        self.review = review
        self.entries = try review.masters.map { master in
            let matches = entries.filter { !$0.archived && $0.receipt?.acquisition.sha == master.acquisition.sha && $0.receipt?.role == master.role }
            guard matches.count == 1 else { throw CalibrationAcquisition.failure("Reload the selected master records before assigning frames.") }
            return matches[0]
        }
        choices = Array(repeating: Array(repeating: Self.unresolved, count: 3), count: review.lights.count)
    }
    func candidates(frame: Int, role: Int) -> [Int] {
        guard review.lights.indices.contains(frame), (0..<3).contains(role) else { return [] }
        return review.frameChoices[frame].roles[CalibrationPlanReport.roles[role]]?.compatibleMasters ?? []
    }
    func groupCandidates(_ group: Int, role: Int) -> [Int] {
        guard review.groups.indices.contains(group) else { return [] }
        let members = review.groups[group].members
        return review.masters.indices.filter { index in members.allSatisfy { candidates(frame: $0, role: role).contains(index) } }
    }
    mutating func assign(_ value: Int, role: Int, frames: [Int]) {
        guard (0..<3).contains(role), frames.allSatisfy({ choices.indices.contains($0) && (value == Self.omitted || value == Self.unresolved || candidates(frame: $0, role: role).contains(value)) }) else { return }
        for frame in frames { choices[frame][role] = value }
    }
    mutating func fillUniqueCandidates() {
        for frame in choices.indices {
            for role in 0..<3 where choices[frame][role] == Self.unresolved {
                let possible = candidates(frame: frame, role: role)
                if possible.count == 1 { choices[frame][role] = possible[0] }
            }
        }
    }
    // Codes are localized by the view. Native preflight remains authoritative.
    func issue(frame: Int) -> String? {
        guard choices.indices.contains(frame), choices[frame].count == 3 else { return "invalid" }
        let row = choices[frame]
        if row.contains(Self.unresolved) { return "unresolved" }
        for role in 0..<3 {
            let index = row[role]
            if index == Self.omitted { continue }
            guard candidates(frame: frame, role: role).contains(index), entries.indices.contains(index),
                  let receipt = entries[index].receipt, ["included", "removed"].contains(receipt.biasState) else { return "preparation" }
        }
        if row[0] == Self.omitted && row[1] == Self.omitted { return "bias-or-dark" }
        if row[0] == Self.omitted {
            if row[1] >= 0 && entries[row[1]].receipt?.biasState == "removed" { return "dark-needs-bias" }
            if row[2] >= 0 && entries[row[2]].receipt?.biasState == "included" { return "flat-needs-bias" }
        }
        return nil
    }
    var readyCount: Int { choices.indices.filter { issue(frame: $0) == nil }.count }
    var isReady: Bool {
        choices.count == review.lights.count && readyCount == review.lights.count && (4...8192).contains(memoryMiB) && (1...32).contains(workers)
    }
    var usedMasters: [Int] { Array(Set(choices.flatMap { $0 }.filter { $0 >= 0 })).sorted() }
    static func quote(_ value: String) throws -> String {
        guard !value.contains("\0"), !value.contains("\n"), !value.contains("\r") else { throw CalibrationAcquisition.failure("Unsupported calibration filename.") }
        return "\"" + value.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
    }
    func manifest() throws -> String {
        guard isReady else { throw CalibrationAcquisition.failure("Resolve every frame's master choices and preparation states before starting.") }
        var text = "PhotonStack-CalibrationJob-1\ntemperature-tolerance \(review.temperatureTolerance)\nmemory-mib \(memoryMiB)\nthreads \(workers)\n"
        for i in usedMasters {
            let master = review.masters[i]
            text += "master m\(i) \(master.role) \(try Self.quote(master.acquisition.path)) \(master.acquisition.sha) \(entries[i].receipt!.biasState)\n"
        }
        for (i, light) in review.lights.enumerated() {
            text += "light \(try Self.quote(light.path)) \(light.sha) " + choices[i].map { $0 == Self.omitted ? "-" : "m\($0)" }.joined(separator: " ") + "\n"
        }
        return text
    }
}
