import Foundation

struct PhotonStackBuildIdentity: Equatable, Sendable {
    let version: String
    let build: String
    let commit: String
    let buildDate: String
    let bundleIdentity: String?
    let executableFingerprint: String

    static var current: PhotonStackBuildIdentity {
        let info = Bundle.main.infoDictionary ?? [:]
        return PhotonStackBuildIdentity(
            version: info["CFBundleShortVersionString"] as? String ?? "dev",
            build: info["CFBundleVersion"] as? String ?? "local",
            commit: info["PhotonStackGitCommit"] as? String ?? "local",
            buildDate: info["PhotonStackBuildDate"] as? String ?? "local",
            bundleIdentity: info["PhotonStackBuildIdentity"] as? String,
            executableFingerprint: executableFingerprint(at: Bundle.main.executableURL)
        )
    }

    init(
        version: String,
        build: String,
        commit: String,
        buildDate: String,
        bundleIdentity: String?,
        executableFingerprint: String
    ) {
        self.version = version
        self.build = build
        self.commit = commit
        self.buildDate = buildDate
        let trimmedIdentity = bundleIdentity?.trimmingCharacters(in: .whitespacesAndNewlines)
        self.bundleIdentity = trimmedIdentity?.isEmpty == false ? trimmedIdentity : nil
        self.executableFingerprint = executableFingerprint
    }

    var cacheIdentity: String {
        if let bundleIdentity {
            return bundleIdentity
        }
        return "development:\(version):\(commit):\(build):\(executableFingerprint)"
    }

    var displayText: String {
        if let bundleIdentity {
            return bundleIdentity
        }
        let versionLabel = version == "dev" ? "development" : "v\(version)"
        return "\(versionLabel) (\(executableFingerprint))"
    }

    var helpText: String {
        var lines = [
            "PhotonStack \(version)",
            "Build: \(build)",
            "Commit: \(commit)",
            "Date: \(buildDate)",
        ]
        if bundleIdentity == nil {
            lines.append("Executable: \(executableFingerprint)")
        }
        return lines.joined(separator: "\n")
    }

    static func executableFingerprint(
        at url: URL?,
        processIdentifier: Int32 = ProcessInfo.processInfo.processIdentifier
    ) -> String {
        guard let url,
              let attributes = try? FileManager.default.attributesOfItem(atPath: url.path)
        else {
            return "process-\(processIdentifier)"
        }

        let size = (attributes[.size] as? NSNumber)?.uint64Value ?? 0
        let fileNumber = (attributes[.systemFileNumber] as? NSNumber)?.uint64Value ?? 0
        let modifiedAt = (attributes[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
        let modifiedBits = modifiedAt.bitPattern
        return "dev-\(String(modifiedBits, radix: 16))-\(size)-\(fileNumber)"
    }
}
